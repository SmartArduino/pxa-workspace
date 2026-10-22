#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_private_copy.h"
#include "pxa/fs.h"
#include "pxa/openssl/pxa_openssl.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open pxa_esp_open
#define openat pxa_esp_openat
#define close pxa_esp_close
#define fstat pxa_esp_fstat
#define fstatat pxa_esp_fstatat
#define mkdirat pxa_esp_mkdirat
#define fsync pxa_esp_fsync
#define dup pxa_esp_dup
#define fdopendir pxa_esp_fdopendir
#endif

#define PRIVATE_PATH_BYTES (PXA_FS_SCOPED_MAX_PATH_BYTES+1u)
#define PRIVATE_DEPTH ((PXA_FS_SCOPED_MAX_PATH_BYTES+1u)/2u+1u)
#define OPEN_DIRECTORIES 4u
typedef struct { DIR *directory;long cookie;uint16_t length; } copy_frame_t;
typedef struct {
    copy_frame_t frames[PRIVATE_DEPTH];
    pxa_esp_private_copy_info_t info;
    const pxa_esp_private_copy_config_t *config;
    char relative[PRIVATE_PATH_BYTES],entry[PRIVATE_PATH_BYTES],absolute[512+PRIVATE_PATH_BYTES+1];
    uint8_t buffer[4096],digest[32],verified[32];
    pxa_openssl_sha256_stream_t hash;
    int source,destination;
    unsigned depth,open_count;
    uint32_t last_yield_tick;
} copy_state_t;

size_t pxa_esp_private_copy_workspace_size(void) {return sizeof(copy_state_t);}
static pxa_status_t io_error(void) {
    if(errno==ENOSPC || errno==EDQUOT)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==ENOMEM || errno==EMFILE || errno==ENFILE)return PXA_STATUS_RESOURCE_LIMIT;
    if(errno==ELOOP || errno==ENOTDIR)return PXA_STATUS_DENIED;
    if(errno==ENAMETOOLONG)return PXA_STATUS_LIMIT_EXCEEDED;
    if(errno==ENOENT)return PXA_STATUS_NOT_FOUND;
    return PXA_STATUS_IO_ERROR;
}
static pxa_status_t check(copy_state_t *state) {
#ifdef ESP_PLATFORM
    pxa_esp_posix_io_checkpoint(&state->last_yield_tick);
#endif
    return state->config->check?state->config->check(state->config->context):PXA_STATUS_OK;
}
static int root_valid(const char *root) {
    if(!root || root[0]!='/' || !root[1] || strlen(root)>511)return 0;
    const char *part=root+1;
    for(const char *p=part;;++p)if(!*p || *p=='/') {
        size_t n=(size_t)(p-part);
        if(!n || (n==1 && part[0]=='.') || (n==2 && part[0]=='.' && part[1]=='.'))return 0;
        if(!*p)return 1;
        part=p+1;
    }
}
static int contains(const char *root,const char *other) {
    size_t n=strlen(root);return !strncmp(root,other,n) && (!other[n] || other[n]=='/');
}
static int name_valid(const char *name) {
    size_t bytes=strlen(name);
    if(!bytes || bytes>PXA_FS_SCOPED_MAX_SEGMENT_BYTES || name[bytes-1]=='.' || name[bytes-1]==' ')return 0;
    /* Host KV/state names also contain .pxa- prefixes. Validate structural
     * path grammar here, not the Guest's reserved-name exclusion policy. */
    for(size_t i=0;i<bytes;++i) {
        unsigned char c=(unsigned char)name[i];
        if(c<0x20 || c==0x7f || c=='/' || c=='\\' || c==':')return 0;
    }
    return strcmp(name,".") && strcmp(name,"..");
}
static pxa_status_t add_entry(copy_state_t *state,const struct stat *info) {
    uint64_t bytes=0,allocated=state->config->allocation_unit;
    if(S_ISREG(info->st_mode)) {
        if(info->st_nlink>1 || info->st_size<0)return PXA_STATUS_DENIED;
        if(state->info.files==UINT32_MAX)return PXA_STATUS_LIMIT_EXCEEDED;
        bytes=(uint64_t)info->st_size;
        if(state->config->max_file_bytes && bytes>state->config->max_file_bytes)return PXA_STATUS_LIMIT_EXCEEDED;
        if(bytes>UINT64_MAX-state->info.logical_bytes)return PXA_STATUS_LIMIT_EXCEEDED;
        if(state->config->max_logical_bytes && bytes>state->config->max_logical_bytes-state->info.logical_bytes)
            return PXA_STATUS_QUOTA_EXCEEDED;
        if(bytes) {
            uint64_t remainder=allocated?bytes%allocated:0;
            allocated=bytes;
            if(remainder) {
                uint64_t extra=state->config->allocation_unit-remainder;
                if(allocated>UINT64_MAX-extra)return PXA_STATUS_LIMIT_EXCEEDED;
                allocated+=extra;
            }
        }
        ++state->info.files;
    } else if(S_ISDIR(info->st_mode)) {
        if(state->info.directories==UINT32_MAX)return PXA_STATUS_LIMIT_EXCEEDED;
        ++state->info.directories;
    } else return PXA_STATUS_DENIED;
    if(allocated>UINT64_MAX-state->info.allocated_bytes)return PXA_STATUS_LIMIT_EXCEEDED;
    if(state->config->max_allocated_bytes && allocated>state->config->max_allocated_bytes-state->info.allocated_bytes)
        return PXA_STATUS_QUOTA_EXCEEDED;
    state->info.logical_bytes+=bytes;state->info.allocated_bytes+=allocated;
    return PXA_STATUS_OK;
}
static pxa_status_t close_file(int *fd,pxa_status_t status) {
    if(*fd>=0 && close(*fd)!=0 && status==PXA_STATUS_OK)status=io_error();
    *fd=-1;return status;
}
/* Resolve each ancestor by a no-follow directory descriptor. Checking only
 * the last name would allow a substituted intermediate symlink to escape. */
static int parent_directory(copy_state_t *state,int root,const char *relative,char **leaf) {
#if defined(ESP_PLATFORM) && !defined(__linux__) && !defined(__APPLE__)
    /* The ESP directory shim targets link-free FAT/LittleFS and checks the
     * complete relative path against the root's mount epoch. Resolve the
     * parent once, using the existing workspace rather than stack storage. */
    strcpy(state->absolute,relative);
    char *slash=strrchr(state->absolute,'/');
    if(slash) {*slash=0;*leaf=slash+1;} else *leaf=state->absolute;
    return openat(root,slash?state->absolute:".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
#else
    strcpy(state->absolute,relative);char *part=state->absolute,*slash;int directory=dup(root);
    if(directory<0)return -1;
    while((slash=strchr(part,'/'))) {
        *slash=0;
        int next=openat(directory,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int error=errno;
        if(close(directory)!=0 && next>=0) {error=errno;close(next);next=-1;}
        if(next<0) {errno=error;return -1;}
        directory=next;part=slash+1;
    }
    *leaf=part;return directory;
#endif
}
static int open_relative(copy_state_t *state,int root,const char *relative,int flags) {
    if(!*relative)return dup(root);
    char *leaf;int parent=parent_directory(state,root,relative,&leaf);
    if(parent<0)return -1;
    int file=openat(parent,leaf,flags|O_NOFOLLOW|O_CLOEXEC,0600),error=errno;
    if(close(parent)!=0 && file>=0) {error=errno;close(file);file=-1;}
    errno=error;return file;
}
static pxa_status_t stat_entry(copy_state_t *state,struct stat *info) {
    char *leaf;int parent=parent_directory(state,state->source,state->entry,&leaf);
    if(parent<0)return io_error();
    pxa_status_t status=fstatat(parent,leaf,info,AT_SYMLINK_NOFOLLOW)==0?PXA_STATUS_OK:io_error();
    return close_file(&parent,status);
}
static pxa_status_t make_directory(copy_state_t *state) {
    char *leaf;int parent=parent_directory(state,state->destination,state->entry,&leaf);
    if(parent<0)return io_error();
    pxa_status_t status=mkdirat(parent,leaf,0700)==0?PXA_STATUS_OK:errno==EEXIST?PXA_STATUS_BAD_STATE:io_error();
    return close_file(&parent,status);
}
static pxa_status_t copy_file(copy_state_t *state,const struct stat *expected) {
    int source=-1,target=-1;struct stat before,after;uint64_t total=0;
    pxa_status_t status=PXA_STATUS_OK;
    source=open_relative(state,state->source,state->entry,O_RDONLY);
    if(source<0)return io_error();
    if(fstat(source,&before)!=0) {status=io_error();goto done;}
    if(!S_ISREG(before.st_mode) || before.st_nlink>1 || before.st_size!=expected->st_size ||
        before.st_dev!=expected->st_dev || before.st_ino!=expected->st_ino) {status=PXA_STATUS_DENIED;goto done;}
    target=open_relative(state,state->destination,state->entry,O_RDWR|O_CREAT|O_EXCL);
    if(target<0) {status=errno==EEXIST?PXA_STATUS_BAD_STATE:io_error();goto done;}
    status=pxa_openssl_sha256_stream_begin(&state->hash);if(status!=PXA_STATUS_OK)goto done;
    for(;;) {
        status=check(state);if(status!=PXA_STATUS_OK)goto done;
        ssize_t count=read(source,state->buffer,sizeof(state->buffer));
        if(count<0 && errno==EINTR)continue;
        if(count<0) {status=io_error();goto done;}
        if(!count)break;
        if(total>(uint64_t)before.st_size || (uint64_t)count>(uint64_t)before.st_size-total) {status=PXA_STATUS_DENIED;goto done;}
        total+=(uint64_t)count;
        status=pxa_openssl_sha256_stream_update(&state->hash,state->buffer,(size_t)count);
        if(status!=PXA_STATUS_OK)goto done;
        size_t written=0;
        while(written<(size_t)count) {
            ssize_t n=write(target,state->buffer+written,(size_t)count-written);
            if(n<0 && errno==EINTR)continue;
            if(n<=0) {status=io_error();goto done;}
            written+=(size_t)n;
        }
    }
    if(total!=(uint64_t)before.st_size || fstat(source,&after)!=0 ||
        before.st_dev!=after.st_dev || before.st_ino!=after.st_ino || before.st_size!=after.st_size ||
        before.st_mtime!=after.st_mtime) {status=PXA_STATUS_DENIED;goto done;}
    status=pxa_openssl_sha256_stream_finish(&state->hash,state->digest);if(status!=PXA_STATUS_OK)goto done;
    if(fsync(target)!=0 || lseek(target,0,SEEK_SET)!=0) {status=io_error();goto done;}
    status=pxa_openssl_sha256_stream_begin(&state->hash);if(status!=PXA_STATUS_OK)goto done;
    total=0;
    for(;;) {
        status=check(state);if(status!=PXA_STATUS_OK)goto done;
        ssize_t count=read(target,state->buffer,sizeof(state->buffer));
        if(count<0 && errno==EINTR)continue;
        if(count<0) {status=io_error();goto done;}
        if(!count)break;
        if(total>(uint64_t)before.st_size || (uint64_t)count>(uint64_t)before.st_size-total) {status=PXA_STATUS_DENIED;goto done;}
        total+=(uint64_t)count;
        status=pxa_openssl_sha256_stream_update(&state->hash,state->buffer,(size_t)count);
        if(status!=PXA_STATUS_OK)goto done;
    }
    status=pxa_openssl_sha256_stream_finish(&state->hash,state->verified);
    if(status==PXA_STATUS_OK && (total!=(uint64_t)before.st_size || memcmp(state->digest,state->verified,32)))status=PXA_STATUS_DENIED;
done:
    pxa_openssl_sha256_stream_abort(&state->hash);
    status=close_file(&target,status);return close_file(&source,status);
}
static pxa_status_t open_directory(copy_state_t *state) {
    copy_frame_t *frame=&state->frames[state->depth];
    if(frame->directory)return PXA_STATUS_OK;
#if defined(ESP_PLATFORM) && !defined(__linux__) && !defined(__APPLE__)
    frame->directory=pxa_esp_opendirat(state->source,*state->relative?state->relative:".");
    if(!frame->directory)return io_error();
#else
    int fd=open_relative(state,state->source,state->relative,O_RDONLY|O_DIRECTORY);
    if(fd<0)return io_error();
    frame->directory=fdopendir(fd);
    if(!frame->directory) {pxa_status_t status=io_error();close(fd);return status;}
#endif
    ++state->open_count;
    if(frame->cookie) {
        errno=0;seekdir(frame->directory,frame->cookie);
        if(errno || telldir(frame->directory)!=frame->cookie)return PXA_STATUS_IO_ERROR;
    }
    return PXA_STATUS_OK;
}
static pxa_status_t sync_directory(copy_state_t *state) {
    if(state->destination<0)return PXA_STATUS_OK;
    int directory=*state->relative?open_relative(state,state->destination,state->relative,O_RDONLY|O_DIRECTORY):-1;
    if(*state->relative && directory<0)return io_error();
    pxa_status_t status=fsync(directory>=0?directory:state->destination)==0?PXA_STATUS_OK:io_error();
    return close_file(&directory,status);
}
static pxa_status_t run(const char *source,const char *destination,uint8_t measure_authority,const pxa_esp_private_copy_config_t *config,
    void *workspace,size_t bytes,pxa_esp_private_copy_info_t *output) {
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    memset(output,0,sizeof(*output));
    if(!config || !root_valid(source) || (destination && (!root_valid(destination) ||
        contains(source,destination) || contains(destination,source))) || !workspace)return PXA_STATUS_INVALID_ARGUMENT;
    if(bytes<sizeof(copy_state_t))return PXA_STATUS_RESOURCE_LIMIT;
    if((uintptr_t)workspace%sizeof(uint64_t))return PXA_STATUS_INVALID_ARGUMENT;
    copy_state_t *state=workspace;memset(state,0,sizeof(*state));state->source=state->destination=-1;state->config=config;
    pxa_status_t status=check(state);struct stat info;
    if(status!=PXA_STATUS_OK)goto done;
    state->source=open(source,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(state->source<0) {status=io_error();goto done;}
    if(fstat(state->source,&info)!=0) {status=io_error();goto done;}
    status=add_entry(state,&info);if(status!=PXA_STATUS_OK)goto done;
    if(destination) {
        state->destination=open(destination,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(state->destination<0) {status=io_error();goto done;}
        struct stat target;
        if(fstat(state->destination,&target)!=0) {status=io_error();goto done;}
        /* Embedded VFS implementations may report zero for every inode.
         * Root/session ownership supplies isolation when inode IDs are absent. */
        if(info.st_ino && target.st_ino && info.st_dev==target.st_dev && info.st_ino==target.st_ino) {
            status=PXA_STATUS_DENIED;goto done;
        }
        DIR *directory=opendir(destination);if(!directory) {status=io_error();goto done;}
        struct dirent *entry;errno=0;
        while((entry=readdir(directory)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) {status=PXA_STATUS_BAD_STATE;break;}
        if(errno && status==PXA_STATUS_OK)status=io_error();
        if(closedir(directory)!=0 && status==PXA_STATUS_OK)status=io_error();
        if(status!=PXA_STATUS_OK)goto done;
    }
    for(;;) {
        status=check(state);if(status!=PXA_STATUS_OK)break;
        status=open_directory(state);if(status!=PXA_STATUS_OK)break;
        copy_frame_t *frame=&state->frames[state->depth];errno=0;
        struct dirent *entry=readdir(frame->directory);
        if(!entry) {
            if(errno) {status=io_error();break;}
            status=sync_directory(state);if(status!=PXA_STATUS_OK)break;
            if(closedir(frame->directory)!=0) {frame->directory=NULL;status=io_error();break;}
            frame->directory=NULL;--state->open_count;
            if(!state->depth)break;
            --state->depth;state->relative[state->frames[state->depth].length]=0;continue;
        }
        frame->cookie=telldir(frame->directory);if(frame->cookie<0) {status=PXA_STATUS_IO_ERROR;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        if(!measure_authority && !state->depth && (!strcmp(entry->d_name,".pxa-policy") ||
            !strcmp(entry->d_name,".pxa-directory-grants") || !strcmp(entry->d_name,".pxa-directory-grants.tmp")))continue;
        int n=snprintf(state->entry,sizeof(state->entry),"%s%s%s",state->relative,*state->relative?"/":"",entry->d_name);
        if(n<0 || (size_t)n>=sizeof(state->entry) || !name_valid(entry->d_name)) {status=PXA_STATUS_LIMIT_EXCEEDED;break;}
#if defined(ESP_PLATFORM) && !defined(__linux__) && !defined(__APPLE__)
        if(entry->d_type==DT_DIR) {memset(&info,0,sizeof(info));info.st_mode=S_IFDIR;}
        else {status=stat_entry(state,&info);if(status!=PXA_STATUS_OK)break;}
#else
        status=stat_entry(state,&info);if(status!=PXA_STATUS_OK)break;
#endif
        status=add_entry(state,&info);if(status!=PXA_STATUS_OK)break;
        if(S_ISDIR(info.st_mode)) {
            if(state->depth+1>=PRIVATE_DEPTH) {status=PXA_STATUS_LIMIT_EXCEEDED;break;}
            if(destination) {status=make_directory(state);if(status!=PXA_STATUS_OK)break;}
            if(state->open_count>=OPEN_DIRECTORIES)for(unsigned i=0;i<=state->depth;++i)if(state->frames[i].directory) {
                int closed=closedir(state->frames[i].directory);state->frames[i].directory=NULL;--state->open_count;
                if(closed)status=io_error();
                break;
            }
            if(status!=PXA_STATUS_OK)break;
            ++state->depth;strcpy(state->relative,state->entry);memset(&state->frames[state->depth],0,sizeof(copy_frame_t));
            state->frames[state->depth].length=(uint16_t)n;
        } else if(destination) {
            status=copy_file(state,&info);if(status!=PXA_STATUS_OK)break;
        }
    }
done:
    for(unsigned i=0;i<PRIVATE_DEPTH;++i)if(state->frames[i].directory) {
        if(closedir(state->frames[i].directory)!=0 && status==PXA_STATUS_OK)status=io_error();
        state->frames[i].directory=NULL;
    }
    status=close_file(&state->destination,status);status=close_file(&state->source,status);
    if(status==PXA_STATUS_OK)*output=state->info;
    return status;
}
pxa_status_t pxa_esp_private_copy_measure(const char *source,const pxa_esp_private_copy_config_t *config,
    void *workspace,size_t bytes,pxa_esp_private_copy_info_t *output) {return run(source,NULL,0,config,workspace,bytes,output);}
pxa_status_t pxa_esp_private_copy_measure_install(const char *source,const pxa_esp_private_copy_config_t *config,
    void *workspace,size_t bytes,pxa_esp_private_copy_info_t *output) {return run(source,NULL,1,config,workspace,bytes,output);}
pxa_status_t pxa_esp_private_copy_tree(const char *source,const char *destination,const pxa_esp_private_copy_config_t *config,
    void *workspace,size_t bytes,pxa_esp_private_copy_info_t *output) {
    if(!destination) {
        if(output)memset(output,0,sizeof(*output));
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    return run(source,destination,0,config,workspace,bytes,output);
}
