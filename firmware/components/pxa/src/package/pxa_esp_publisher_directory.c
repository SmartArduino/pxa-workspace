#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_publisher_directory.h"
#include "pxa_esp_record_seal.h"
#include "pxa/pxa_esp_storage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open(...) pxa_esp_open(__VA_ARGS__)
#define openat pxa_esp_openat
#define close pxa_esp_close
#define fstat pxa_esp_fstat
#define fstatat pxa_esp_fstatat
#define mkdirat pxa_esp_mkdirat
#define renameat pxa_esp_renameat
#define unlinkat pxa_esp_unlinkat
#define fsync pxa_esp_fsync
#define dup pxa_esp_dup
#define fdopendir pxa_esp_fdopendir
#endif
/* Fixed byte encoding, independent of host alignment. The directory is allowed
 * at its canonical hash or its sealed name, so a power loss between the marker
 * commit and directory rename leaves exactly the same installation reachable. */
#define BODY_BYTES 105u
#define MARKER_BYTES (BODY_BYTES+32u)
#define MARKER ".publisher.pxi"
static const uint8_t magic[8]={'P','X','A','P','U','B',1,0};
static pxa_status_t error(void) {
    if(errno==ENOENT)return PXA_STATUS_NOT_FOUND;
    if(errno==ENOSPC)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==ENOTDIR || errno==ELOOP)return PXA_STATUS_DENIED;
    return PXA_STATUS_IO_ERROR;
}
static int component(const char *s,size_t n) {
    if(!n || n>64 || !((s[0]>='a' && s[0]<='z') || (s[0]>='0' && s[0]<='9')))return 0;
    for(size_t i=0;i<n;++i)if(!((s[i]>='a' && s[i]<='z') || (s[i]>='0' && s[i]<='9') ||
        s[i]=='-' || s[i]=='_' || s[i]=='.'))return 0;
    return 1;
}
static void root_text(const uint8_t root[32],char text[65]) {
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i) {text[2*i]=hex[root[i]>>4];text[2*i+1]=hex[root[i]&15];}
    text[64]=0;
}
static int root_bytes(const char *text,uint8_t root[32]) {
    if(strlen(text)!=64)return 0;
    for(unsigned i=0;i<32;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {
            unsigned c=(unsigned char)text[2*i+j];
            if(c>='0' && c<='9')value=16*value+c-'0';
            else if(c>='a' && c<='f')value=16*value+c-'a'+10;
            else return 0;
        }
        root[i]=(uint8_t)value;
    }
    return 1;
}
static pxa_status_t tag(const pxa_volume_id_t *volume,const char *kind,const uint8_t *body,uint8_t out[32]) {
    uint8_t binding[21]={0};memcpy(binding,volume->bytes,16);memcpy(binding+16,kind,strlen(kind));
    static const uint8_t purpose[]="publisher-directory/1";
    return pxa_esp_record_seal((pxa_bytes_t){purpose,sizeof(purpose)-1},
        (pxa_bytes_t){binding,sizeof(binding)},(pxa_bytes_t){body,BODY_BYTES},out);
}
static int parent_directory(const char *root,const char *kind,int create) {
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    if(create && mkdirat(fd,kind,0700)!=0 && errno!=EEXIST) {int saved=errno;close(fd);errno=saved;return -1;}
    if(create && fsync(fd)!=0) {int saved=errno;close(fd);errno=saved;return -1;}
    int parent=openat(fd,kind,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int saved=errno;close(fd);errno=saved;return parent;
}
static pxa_status_t read_marker(int parent,const char *name,const pxa_volume_id_t *volume,
    const char *kind,uint8_t body[MARKER_BYTES]) {
    int dir=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return error();
    int fd=openat(dir,MARKER,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);int saved=errno;close(dir);errno=saved;
    if(fd<0)return error();
    struct stat st;pxa_status_t status=PXA_STATUS_DENIED;
    if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==MARKER_BYTES) {
        size_t done=0;
        while(done<MARKER_BYTES) {ssize_t n=read(fd,body+done,MARKER_BYTES-done);if(n<0 && errno==EINTR)continue;if(n<=0)break;done+=(size_t)n;}
        if(done==MARKER_BYTES && !memcmp(body,magic,8) && memchr(body+40,0,65) &&
            component((char *)body+40,strlen((char *)body+40))) {
            uint8_t expected[32]={0};char canonical[65];root_text(body+8,canonical);
            status=tag(volume,kind,body,expected);unsigned difference=0;
            for(unsigned i=0;i<32;++i)difference|=expected[i]^body[BODY_BYTES+i];
            if(status==PXA_STATUS_OK && (difference || (strcmp(name,(char *)body+40) && strcmp(name,canonical))))status=PXA_STATUS_DENIED;
        }
    }
    if(close(fd)!=0 && status==PXA_STATUS_OK)status=error();
    return status;
}
/* Find one unambiguous owner. Unclaimed directories can never become aliases.
 * An invalid claimed directory fails closed instead of creating empty saves. */
static pxa_status_t find_name(int parent,const pxa_volume_id_t *volume,const char *kind,
    const uint8_t root[32],char out[65]) {
    char canonical[65];root_text(root,canonical);strcpy(out,canonical);
    if(parent<0)return PXA_STATUS_OK;
    int copy=dup(parent);if(copy<0)return error();
    DIR *stream=fdopendir(copy);if(!stream) {close(copy);return error();}
    pxa_status_t status=PXA_STATUS_OK;unsigned matches=0,unresolved=0;
    for(;;) {
        errno=0;struct dirent *entry=readdir(stream);if(!entry) {if(errno)status=error();break;}
        if(!component(entry->d_name,strlen(entry->d_name)))continue;
        struct stat metadata;
        if(fstatat(parent,entry->d_name,&metadata,AT_SYMLINK_NOFOLLOW)!=0) {status=error();break;}
        if(!S_ISDIR(metadata.st_mode)) {
            if(!strcmp(entry->d_name,canonical)) {status=PXA_STATUS_DENIED;break;}
            continue;
        }
        uint8_t record[MARKER_BYTES]={0};
        pxa_status_t read_status=read_marker(parent,entry->d_name,volume,kind,record);
        if(read_status==PXA_STATUS_NOT_FOUND) {
            if(strcmp(entry->d_name,canonical))continue;
        } else if(read_status==PXA_STATUS_OK) {
            if(memcmp(record+8,root,32))continue;
        } else if(read_status==PXA_STATUS_DENIED) {
            if(!strcmp(entry->d_name,canonical) || !memcmp(record+8,root,32)) {status=read_status;break;}
            /* Do not let an unrelated broken namespace disable a verified
             * existing home. Without any owner, refuse to fabricate a new
             * save: the unreadable marker might have named this publisher. */
            ++unresolved;continue;
        } else {status=read_status;break;}
        if(++matches>1) {status=PXA_STATUS_DENIED;break;}
        strcpy(out,entry->d_name);
    }
    if(status==PXA_STATUS_OK && !matches && unresolved)status=PXA_STATUS_DENIED;
    if(closedir(stream)!=0 && status==PXA_STATUS_OK)status=error();
    return status;
}
pxa_status_t pxa_esp_publisher_map(const char *pxa_root,const pxa_volume_id_t *volume,
    const char *kind,const char *locator,unsigned reverse,char *out,size_t capacity) {
    if(!pxa_root || !volume || !kind || (strcmp(kind,"apps") && strcmp(kind,"media")) || !locator || !out || !capacity)return PXA_STATUS_INVALID_ARGUMENT;
    const char *slash=strchr(locator,'/');
    if(!slash || strchr(slash+1,'/') || !component(locator,(size_t)(slash-locator)) || !component(slash+1,strlen(slash+1)))return PXA_STATUS_INVALID_ARGUMENT;
    char name[65],resolved[65];memcpy(name,locator,(size_t)(slash-locator));name[slash-locator]=0;
    int parent=parent_directory(pxa_root,kind,0);
    if(parent<0 && errno!=ENOENT)return error();
    uint8_t root[32],record[MARKER_BYTES];pxa_status_t status=PXA_STATUS_OK;
    if(reverse) {
        status=parent<0?PXA_STATUS_NOT_FOUND:read_marker(parent,name,volume,kind,record);
        if(status==PXA_STATUS_OK)root_text(record+8,resolved);
        else if(status==PXA_STATUS_NOT_FOUND && root_bytes(name,root)) {strcpy(resolved,name);status=PXA_STATUS_OK;}
    } else if(!root_bytes(name,root))status=PXA_STATUS_INVALID_ARGUMENT;
    else status=find_name(parent,volume,kind,root,resolved);
    if(parent>=0 && close(parent)!=0 && status==PXA_STATUS_OK)status=error();
    if(status==PXA_STATUS_OK) {int n=snprintf(out,capacity,"%s/%s",resolved,slash+1);if(n<0 || (size_t)n>=capacity)status=PXA_STATUS_LIMIT_EXCEEDED;}
    return status;
}
pxa_status_t pxa_esp_publisher_reserve(const char *pxa_root,const pxa_volume_id_t *volume,
    const char *kind,const uint8_t root[32],const char *handle,uint8_t migrate_existing) {
    if(!pxa_root || !volume || !root || !kind || (strcmp(kind,"apps") && strcmp(kind,"media")) ||
        !handle || strlen(handle)>24 || !component(handle,strlen(handle)) || handle[0]<'a' || handle[0]>'z')return PXA_STATUS_INVALID_ARGUMENT;
    int parent=parent_directory(pxa_root,kind,1);if(parent<0)return error();
    char existing[65],canonical[65],chosen[65];uint8_t record[MARKER_BYTES]={0};struct stat st;
    root_text(root,canonical);
    pxa_status_t status=find_name(parent,volume,kind,root,existing);
    if(status!=PXA_STATUS_OK)goto done;
    status=read_marker(parent,existing,volume,kind,record);
    if(status==PXA_STATUS_OK) {
        strcpy(chosen,(char *)record+40);
        if(!strcmp(existing,chosen))goto done;
        if(!migrate_existing) {status=PXA_STATUS_BUSY;goto done;}
    } else if(status==PXA_STATUS_NOT_FOUND) {
        if(!migrate_existing) {
            if(fstatat(parent,existing,&st,AT_SYMLINK_NOFOLLOW)==0) {status=PXA_STATUS_BUSY;goto done;}
            if(errno!=ENOENT) {status=error();goto done;}
        }
        for(unsigned digits=0;;digits=digits?digits+8:8) {
            if(digits>32) {status=PXA_STATUS_LIMIT_EXCEEDED;goto done;}
            if(digits)snprintf(chosen,sizeof(chosen),"%s-%.*s",handle,(int)digits,canonical);
            else strcpy(chosen,handle);
            if(fstatat(parent,chosen,&st,AT_SYMLINK_NOFOLLOW)!=0) {if(errno==ENOENT)break;status=error();goto done;}
        }
        memcpy(record,magic,8);memcpy(record+8,root,32);strcpy((char *)record+40,chosen);
        status=tag(volume,kind,record,record+BODY_BYTES);if(status!=PXA_STATUS_OK)goto done;
        /* Create or reuse only the full-root directory; never take over an
         * unclaimed human name. Commit the binding before the atomic rename. */
        if(mkdirat(parent,existing,0700)!=0 && errno!=EEXIST) {status=error();goto done;}
        int dir=openat(parent,existing,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(dir<0) {status=error();goto done;}
        if(unlinkat(dir,MARKER ".pending",0)!=0 && errno!=ENOENT)status=error();
        int fd=status==PXA_STATUS_OK?openat(dir,MARKER ".pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600):-1;
        if(fd<0 && status==PXA_STATUS_OK)status=error();
        if(fd>=0) {
            size_t written=0;
            while(written<sizeof(record)) {ssize_t n=write(fd,record+written,sizeof(record)-written);if(n<0 && errno==EINTR)continue;if(n<=0) {status=error();break;}written+=(size_t)n;}
            if(status==PXA_STATUS_OK && fsync(fd)!=0)status=error();
            if(close(fd)!=0 && status==PXA_STATUS_OK)status=error();
        }
        if(status==PXA_STATUS_OK && renameat(dir,MARKER ".pending",dir,MARKER)!=0)status=error();
        if(status==PXA_STATUS_OK && fsync(dir)!=0)status=error();
        if(close(dir)!=0 && status==PXA_STATUS_OK)status=error();
        if(status!=PXA_STATUS_OK)goto done;
    } else goto done;
    if(fstatat(parent,chosen,&st,AT_SYMLINK_NOFOLLOW)==0) {status=PXA_STATUS_DENIED;goto done;}
    if(errno!=ENOENT) {status=error();goto done;}
    if(renameat(parent,existing,parent,chosen)!=0) {status=error();goto done;}
    if(fsync(parent)!=0)status=error();
done:
    if(close(parent)!=0 && status==PXA_STATUS_OK)status=error();
    return status;
}
pxa_status_t pxa_esp_publisher_bind(const char *pxa_root,const pxa_volume_id_t *volume,
    const char *kind,const uint8_t root[32],const char *handle) {
    return pxa_esp_publisher_reserve(pxa_root,volume,kind,root,handle,1);
}
pxa_status_t pxa_esp_publisher_path(uint64_t epoch,const char *kind,const char *locator,char *out,size_t capacity) {
    pxa_volume_info_t volume;char root[PXA_ESP_STORAGE_PATH_BYTES+8],physical[130];
    pxa_status_t status=pxa_esp_storage_volume(epoch,&volume);
    if(status!=PXA_STATUS_OK)return status;
    if(volume.state!=PXA_VOLUME_READY || volume.ambiguous)return PXA_STATUS_UNAVAILABLE;
    status=pxa_esp_storage_volume_path(epoch,root,sizeof(root));if(status!=PXA_STATUS_OK)return status;
    size_t n=strlen(root);if(n+5>sizeof(root))return PXA_STATUS_LIMIT_EXCEEDED;memcpy(root+n,"/PXA",5);
    status=pxa_esp_publisher_map(root,&volume.id,kind,locator,0,physical,sizeof(physical));
    if(status!=PXA_STATUS_OK)return status;
    int length=snprintf(out,capacity,"%s/%s/%s",root,kind,physical);
    return length<0 || (size_t)length>=capacity?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
}
