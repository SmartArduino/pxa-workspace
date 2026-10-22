#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_private_authority.h"
#include "pxa/fs.h"
#include "pxa/openssl/pxa_openssl.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open(...) pxa_esp_open(__VA_ARGS__)
#define openat pxa_esp_openat
#define close pxa_esp_close
#define mkdirat pxa_esp_mkdirat
#define fstat pxa_esp_fstat
#define fstatat pxa_esp_fstatat
#define fsync pxa_esp_fsync
#define fdopendir pxa_esp_fdopendir
#define dup pxa_esp_dup
#endif
#define GRANTS 4u
#define GRANT_BYTES 304u
#define GRANT_FILE_BYTES (16u+GRANTS*GRANT_BYTES+32u)
typedef struct {uint8_t grants[GRANT_FILE_BYTES],digest[32],old_tag[32],new_tag[32];char pending_digest[65];} authority_t;
static pxa_status_t error(void) {
    if(errno==ENOSPC || errno==EDQUOT)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==ENOMEM || errno==EMFILE || errno==ENFILE)return PXA_STATUS_RESOURCE_LIMIT;
    return PXA_STATUS_IO_ERROR;
}
static pxa_status_t read_exact(int directory,const char *name,uint8_t *out,size_t size) {
    int file=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(file<0)return errno==ENOENT?PXA_STATUS_NOT_FOUND:error();
    struct stat metadata;pxa_status_t status=PXA_STATUS_OK;size_t offset=0;
    if(fstat(file,&metadata)!=0)status=error();
    else if(!S_ISREG(metadata.st_mode) || metadata.st_nlink>1 || metadata.st_size!=(off_t)size)status=PXA_STATUS_DENIED;
    while(status==PXA_STATUS_OK && offset<size) {
        ssize_t n=read(file,out+offset,size-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {status=PXA_STATUS_IO_ERROR;break;}offset+=(size_t)n;
    }
    if(close(file)!=0 && status==PXA_STATUS_OK)status=error();
    return status;
}
static pxa_status_t write_new(int directory,const char *name,const uint8_t *data,size_t size) {
    int file=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(file<0)return error();
    pxa_status_t status=PXA_STATUS_OK;size_t offset=0;
    while(offset<size) {
        ssize_t n=write(file,data+offset,size-offset);if(n<0 && errno==EINTR)continue;
        if(n<=0) {status=error();break;}offset+=(size_t)n;
    }
    if(status==PXA_STATUS_OK && fsync(file)!=0)status=error();
    if(close(file)!=0 && status==PXA_STATUS_OK)status=error();
    return status;
}
static int decode_digest(const char *name,uint8_t *digest) {
    if(strlen(name)!=64)return 0;
    for(unsigned i=0;i<32;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {
            unsigned char c=(unsigned char)name[2*i+j];
            if(c>='0' && c<='9')value=(value<<4)|(c-'0');
            else if(c>='a' && c<='f')value=(value<<4)|(c-'a'+10);
            else return 0;
        }
        digest[i]=(uint8_t)value;
    }
    return 1;
}
static int valid_grants(const uint8_t *data) {
    if(memcmp(data,"PXAGRNT2",8) || pxa_read_u16(data+8)!=GRANTS)return 0;
    for(unsigned i=10;i<16;++i)if(data[i])return 0;
    for(unsigned i=0;i<GRANTS;++i) {
        const uint8_t *p=data+16+i*GRANT_BYTES;uint16_t length=pxa_read_u16(p+46);
        if(p[44]>1 || p[45] || length>=256)return 0;
        if(p[44]) {
            uint8_t id=0,volume=0;
            for(unsigned j=0;j<16;++j) {id|=p[j];volume|=p[16+j];}
            if(!id || !volume || !pxa_read_u64(p+32) || pxa_read_u32(p+40)!=3 ||
                (length && !pxa_fs_scoped_path_is_valid((pxa_bytes_t){p+48,length})))return 0;
            for(unsigned j=0;j<i;++j)if(data[16+j*GRANT_BYTES+44] && !memcmp(data+16+j*GRANT_BYTES,p,16))return 0;
        }
    }
    return 1;
}
static pxa_status_t read_legacy_grants(authority_t *state,int directory,const char *name,uint8_t *present) {
    *present=0;if(directory<0)return PXA_STATUS_OK;
    pxa_status_t status=read_exact(directory,name,state->grants,sizeof(state->grants));
    if(status==PXA_STATUS_NOT_FOUND)return PXA_STATUS_OK;
    if(status!=PXA_STATUS_OK)return status;
    if(memcmp(state->grants,"PXAGRNT1",8))return PXA_STATUS_DENIED;
    status=pxa_openssl_sha256(state->grants,sizeof(state->grants)-32,state->digest);
    if(status!=PXA_STATUS_OK)return status;
    if(memcmp(state->digest,state->grants+sizeof(state->grants)-32,32))return PXA_STATUS_DENIED;
    memcpy(state->grants,"PXAGRNT2",8);
    if(!valid_grants(state->grants))return PXA_STATUS_DENIED;
    *present=1;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_private_authority_validate_legacy(pxa_bytes_t identity,
    const pxa_permission_store_t *source,const pxa_package_permission_t *permissions,size_t count,
    int directory,const char *name) {
    if(!source || !source->load || !identity.data || !identity.size || identity.size>129 ||
        (count && !permissions) || directory < -1 || (directory>=0 &&
        (!name || strlen(name)!=identity.size || memcmp(name,identity.data,identity.size))))return PXA_STATUS_INVALID_ARGUMENT;
    authority_t *state=heap_caps_calloc(1,sizeof(*state),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!state)return PXA_STATUS_RESOURCE_LIMIT;
    uint8_t present;pxa_status_t status=read_legacy_grants(state,directory,name,&present);
    for(size_t i=0;status==PXA_STATUS_OK && i<count;++i) {
        pxa_permission_decision_t decision=PXA_PERMISSION_DENY;
        status=source->load(source->context,identity,permissions[i].name,permissions[i].scope,&decision);
        if(status==PXA_STATUS_NOT_FOUND)status=PXA_STATUS_OK;
        else if(status==PXA_STATUS_OK && decision!=PXA_PERMISSION_ALLOW && decision!=PXA_PERMISSION_DENY)status=PXA_STATUS_DENIED;
    }
    heap_caps_free(state);return status;
}
pxa_status_t pxa_esp_private_authority_import_legacy(pxa_esp_app_storage_t *target,
    pxa_bytes_t identity,const pxa_permission_store_t *source,
    const pxa_package_permission_t *permissions,size_t count,int grants_directory,const char *grant_name) {
    if(!target || !source || !identity.data || !identity.size || identity.size>129 ||
        (count && !permissions) || grants_directory < -1 ||
        (grants_directory>=0 && (!grant_name || strlen(grant_name)!=identity.size ||
         memcmp(grant_name,identity.data,identity.size))))return PXA_STATUS_INVALID_ARGUMENT;
    authority_t *state=heap_caps_calloc(1,sizeof(*state),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!state)return PXA_STATUS_RESOURCE_LIMIT;
    const char *root;int destination=-1;uint8_t have_grants=0;
    pxa_status_t status=pxa_esp_app_storage_existing_root(target,&root);
    if(status==PXA_STATUS_OK && (destination=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC))<0)status=error();
    const char *controls[]={".pxa-policy",".pxa-directory-grants"};
    for(unsigned i=0;status==PXA_STATUS_OK && i<2;++i) {
        struct stat metadata;
        if(fstatat(destination,controls[i],&metadata,AT_SYMLINK_NOFOLLOW)==0)status=PXA_STATUS_BAD_STATE;
        else if(errno!=ENOENT)status=error();
    }
    if(status==PXA_STATUS_OK)status=read_legacy_grants(state,grants_directory,grant_name,&have_grants);
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_import_legacy_permissions(target,identity,source,permissions,count);
    if(status==PXA_STATUS_OK && have_grants) {
        status=pxa_esp_app_storage_seal_grants(target,(pxa_bytes_t){state->grants,sizeof(state->grants)-32},state->new_tag);
        if(status==PXA_STATUS_OK) {
            memcpy(state->grants+sizeof(state->grants)-32,state->new_tag,32);
            status=write_new(destination,".pxa-directory-grants",state->grants,sizeof(state->grants));
        }
    }
    if(destination>=0) {
        if(status==PXA_STATUS_OK && fsync(destination)!=0)status=error();
        if(close(destination)!=0 && status==PXA_STATUS_OK)status=error();
    }
    heap_caps_free(state);return status;
}
pxa_status_t pxa_esp_private_authority_copy(pxa_esp_app_storage_t *source,pxa_esp_app_storage_t *target) {
    const char *root;int from=-1,to=-1,policy=-1,destination=-1;DIR *entries=NULL;
    authority_t *state=heap_caps_calloc(1,sizeof(*state),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!state)return PXA_STATUS_RESOURCE_LIMIT;
    pxa_status_t status=pxa_esp_app_storage_existing_root(source,&root);
    if(status==PXA_STATUS_OK && (from=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC))<0)status=error();
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_existing_root(target,&root);
    if(status==PXA_STATUS_OK && (to=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC))<0)status=error();
    if(status!=PXA_STATUS_OK)goto done;
    policy=openat(from,".pxa-policy",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(policy<0 && errno!=ENOENT) {status=error();goto done;}
    if(policy>=0) {
        int iterator=dup(policy);if(iterator<0) {status=error();goto done;}
        entries=fdopendir(iterator);
        if(!entries) {status=error();close(iterator);goto done;}
        if(mkdirat(to,".pxa-policy",0700)!=0) {status=error();goto done;}
        destination=openat(to,".pxa-policy",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(destination<0) {status=error();goto done;}
        for(;;) {
            errno=0;struct dirent *entry=readdir(entries);
            if(!entry) {if(errno)status=error();break;}
            if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
            if(strlen(entry->d_name)==73 && entry->d_name[0]=='.' && !strcmp(entry->d_name+65,".pending")) {
                memcpy(state->pending_digest,entry->d_name+1,64);state->pending_digest[64]=0;
                /* An interrupted allow never became a committed decision. */
                if(decode_digest(state->pending_digest,state->digest))continue;
            }
            if(!decode_digest(entry->d_name,state->digest)) {status=PXA_STATUS_DENIED;break;}
            status=read_exact(policy,entry->d_name,state->old_tag,32);
            if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_transfer_permission_tag(source,target,
                state->digest,state->old_tag,state->new_tag);
            if(status==PXA_STATUS_OK)status=write_new(destination,entry->d_name,state->new_tag,32);
            if(status!=PXA_STATUS_OK)break;
        }
        if(status==PXA_STATUS_OK && fsync(destination)!=0)status=error();
    }
    if(status!=PXA_STATUS_OK)goto done;
    status=read_exact(from,".pxa-directory-grants",state->grants,sizeof(state->grants));
    if(status==PXA_STATUS_NOT_FOUND) {status=PXA_STATUS_OK;goto done;}
    if(status!=PXA_STATUS_OK)goto done;
    if(!valid_grants(state->grants)) {status=PXA_STATUS_DENIED;goto done;}
    status=pxa_esp_app_storage_transfer_grants(source,target,
        (pxa_bytes_t){state->grants,sizeof(state->grants)-32},state->grants+sizeof(state->grants)-32,state->new_tag);
    if(status==PXA_STATUS_OK) {
        memcpy(state->grants+sizeof(state->grants)-32,state->new_tag,32);
        status=write_new(to,".pxa-directory-grants",state->grants,sizeof(state->grants));
    }
done:
    if(entries && closedir(entries)!=0 && status==PXA_STATUS_OK)status=error();
    if(policy>=0 && close(policy)!=0 && status==PXA_STATUS_OK)status=error();
    if(destination>=0 && close(destination)!=0 && status==PXA_STATUS_OK)status=error();
    if(to>=0) {if(status==PXA_STATUS_OK && fsync(to)!=0)status=error();if(close(to)!=0 && status==PXA_STATUS_OK)status=error();}
    if(from>=0 && close(from)!=0 && status==PXA_STATUS_OK)status=error();
    heap_caps_free(state);return status;
}
