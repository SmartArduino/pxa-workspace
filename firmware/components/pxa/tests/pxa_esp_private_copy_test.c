#define _GNU_SOURCE
#include "pxa_esp_private_copy.h"
#include "pxa/fs.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned descriptors(void) {
    DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;struct dirent *entry;
    while((entry=readdir(d)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++n;
    assert(!closedir(d));return n;
}
typedef struct {unsigned calls,fail_at,max_fds;pxa_status_t failure;char corrupt[1024];int changed;} check_state_t;
static pxa_status_t check(void *context) {
    check_state_t *state=context;unsigned n=descriptors();if(n>state->max_fds)state->max_fds=n;
    ++state->calls;
    if(state->fail_at && state->calls>=state->fail_at)return state->failure;
    if(*state->corrupt && !state->changed) {
        struct stat info;
        if(!stat(state->corrupt,&info) && info.st_size==9000) {
            int fd=open(state->corrupt,O_WRONLY);assert(fd>=0);
            assert(pwrite(fd,"X",1,1)==1 && !fsync(fd) && !close(fd));state->changed=1;
        }
    }
    return PXA_STATUS_OK;
}
static void join(char *out,size_t bytes,const char *root,const char *relative) {
    assert(snprintf(out,bytes,"%s/%s",root,relative)<(int)bytes);
}
static void folder(const char *root,const char *relative) {char path[2048];join(path,sizeof(path),root,relative);assert(!mkdir(path,0700));}
static void write_file(const char *root,const char *relative,const void *bytes,size_t n) {
    char path[1024];join(path,sizeof(path),root,relative);int fd=open(path,O_CREAT|O_WRONLY|O_EXCL,0600);assert(fd>=0);
    assert(write(fd,bytes?bytes:"",n)==(ssize_t)n && !close(fd));
}
static void read_file(const char *root,const char *relative,const void *expected,size_t n) {
    char path[2048],bytes[10000];assert(n<sizeof(bytes));join(path,sizeof(path),root,relative);
    int fd=open(path,O_RDONLY);assert(fd>=0);assert(read(fd,bytes,sizeof(bytes))==(ssize_t)n);
    if(n)assert(!memcmp(bytes,expected,n));
    assert(!close(fd));
}
static void remove_tree(const char *root) {
    DIR *d=opendir(root);assert(d);struct dirent *entry;char path[2048];struct stat info;
    while((entry=readdir(d))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        join(path,sizeof(path),root,entry->d_name);assert(!lstat(path,&info));
        if(S_ISDIR(info.st_mode))remove_tree(path);else assert(!unlink(path));
    }
    assert(!closedir(d) && !rmdir(root));
}
static void fresh(const char *target) {remove_tree(target);assert(!mkdir(target,0700));}
static void empty_result(const pxa_esp_private_copy_info_t *info) {
    assert(!info->files && !info->directories && !info->logical_bytes && !info->allocated_bytes);
}
int main(void) {
    char root[256],source[512],target[512],path[2048],deep[PXA_FS_SCOPED_MAX_PATH_BYTES+1]={0};
    assert(snprintf(root,sizeof(root),"%s/private-copy-XXXXXX",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp")<(int)sizeof(root));
    assert(mkdtemp(root));folder(root,"source");folder(root,"target");
    join(source,sizeof(source),root,"source");join(target,sizeof(target),root,"target");
    write_file(source,"Save.sav","upper",5);write_file(source,"save.sav","lower",5);
    folder(source,".pxa-kv");uint8_t binary[9000];for(size_t i=0;i<sizeof(binary);++i)binary[i]=(uint8_t)i;
    write_file(source,".pxa-kv/slot.bin",binary,sizeof(binary));write_file(source,"empty",NULL,0);
    folder(source,"data");write_file(source,"data/.pxa-policy","user data",9);
    folder(source,"z");write_file(source,"z/file","sibling!",8);
    for(unsigned i=0;i<12;++i) {strcat(deep,i?"/d":"d");folder(source,deep);}
    char leaf[PXA_FS_SCOPED_MAX_PATH_BYTES+1];assert(snprintf(leaf,sizeof(leaf),"%s/save",deep)<(int)sizeof(leaf));write_file(source,leaf,"deep savefile",13);
    join(path,sizeof(path),source,".pxa-policy");assert(!symlink("/untrusted/outside",path));
    write_file(source,".pxa-directory-grants","old volume authority",20);
    write_file(source,".pxa-directory-grants.tmp","unfinished",10);
    size_t size=pxa_esp_private_copy_workspace_size();void *workspace=calloc(1,size);assert(workspace);
    check_state_t checked={0};unsigned original=descriptors();
    pxa_esp_private_copy_config_t config={4096,0,0,0,&checked,check};
    pxa_esp_private_copy_info_t info,copied;
    assert(pxa_esp_private_copy_measure(source,&config,workspace,size-1,&info)==PXA_STATUS_RESOURCE_LIMIT);
    assert(!pxa_esp_private_copy_measure(source,&config,workspace,size,&info));
    assert(info.files==7 && info.directories==16 && info.logical_bytes==9040 && info.allocated_bytes==102400);
    assert(!pxa_esp_private_copy_tree(source,target,&config,workspace,size,&copied));assert(!memcmp(&copied,&info,sizeof(info)));
    assert(checked.max_fds<=original+8 && descriptors()==original);
    read_file(target,"Save.sav","upper",5);read_file(target,"save.sav","lower",5);
    read_file(target,".pxa-kv/slot.bin",binary,sizeof(binary));read_file(target,leaf,"deep savefile",13);
    read_file(target,"data/.pxa-policy","user data",9);read_file(target,"z/file","sibling!",8);read_file(target,"empty",NULL,0);
    join(path,sizeof(path),target,".pxa-policy");assert(access(path,F_OK)<0);
    join(path,sizeof(path),target,".pxa-directory-grants");assert(access(path,F_OK)<0);
    assert(pxa_esp_private_copy_tree(source,target,&config,workspace,size,&copied)==PXA_STATUS_BAD_STATE);empty_result(&copied);
    config.max_logical_bytes=9039;
    assert(pxa_esp_private_copy_measure(source,&config,workspace,size,&copied)==PXA_STATUS_QUOTA_EXCEEDED);empty_result(&copied);
    config.max_logical_bytes=0;config.max_allocated_bytes=102399;
    assert(pxa_esp_private_copy_measure(source,&config,workspace,size,&copied)==PXA_STATUS_QUOTA_EXCEEDED);empty_result(&copied);
    config.max_allocated_bytes=0;config.max_file_bytes=8999;
    assert(pxa_esp_private_copy_measure(source,&config,workspace,size,&copied)==PXA_STATUS_LIMIT_EXCEEDED);empty_result(&copied);
    config.max_file_bytes=0;fresh(target);checked.fail_at=checked.calls+8;checked.failure=PXA_STATUS_CANCELLED;
    assert(pxa_esp_private_copy_tree(source,target,&config,workspace,size,&copied)==PXA_STATUS_CANCELLED);empty_result(&copied);
    assert(descriptors()==original);read_file(source,".pxa-kv/slot.bin",binary,sizeof(binary));
    fresh(target);checked.fail_at=checked.calls+8;checked.failure=PXA_STATUS_UNAVAILABLE;
    assert(pxa_esp_private_copy_tree(source,target,&config,workspace,size,&copied)==PXA_STATUS_UNAVAILABLE);empty_result(&copied);
    checked.fail_at=0;fresh(target);join(checked.corrupt,sizeof(checked.corrupt),target,".pxa-kv/slot.bin");
    assert(pxa_esp_private_copy_tree(source,target,&config,workspace,size,&copied)==PXA_STATUS_DENIED && checked.changed);empty_result(&copied);
    read_file(source,".pxa-kv/slot.bin",binary,sizeof(binary));checked.corrupt[0]=0;
    join(path,sizeof(path),source,"escape");assert(!symlink("/tmp",path));
    assert(pxa_esp_private_copy_measure(source,&config,workspace,size,&copied)==PXA_STATUS_DENIED);assert(!unlink(path));
    char original_file[1024];join(original_file,sizeof(original_file),source,"Save.sav");join(path,sizeof(path),source,"hardlink");
    assert(!link(original_file,path));assert(pxa_esp_private_copy_measure(source,&config,workspace,size,&copied)==PXA_STATUS_DENIED);assert(!unlink(path));
    join(path,sizeof(path),source,"fifo");assert(!mkfifo(path,0600));
    assert(pxa_esp_private_copy_measure(source,&config,workspace,size,&copied)==PXA_STATUS_DENIED);assert(!unlink(path));
    assert(pxa_esp_private_copy_tree(source,source,&config,workspace,size,&copied)==PXA_STATUS_INVALID_ARGUMENT);
    /* One-byte components reach the current scoped path limit,
     * while the four-directory cache keeps the FD peak independent of depth. */
    fresh(target);remove_tree(source);assert(!mkdir(source,0700));deep[0]=0;
    unsigned maximum_depth=(PXA_FS_SCOPED_MAX_PATH_BYTES+1)/2;
    for(unsigned i=0;i<maximum_depth;++i) {strcat(deep,i?"/d":"d");folder(source,deep);}
    checked.max_fds=0;
    assert(!pxa_esp_private_copy_tree(source,target,&config,workspace,size,&copied));
    assert(copied.directories==maximum_depth+1 && !copied.files && checked.max_fds<=original+8 && descriptors()==original);
    join(path,sizeof(path),target,deep);struct stat stat;assert(!lstat(path,&stat) && S_ISDIR(stat.st_mode));
    free(workspace);remove_tree(root);
    printf("Private transfer: case/KV/data, authority exclusion, SHA reread, admission/cancel/outage, no links/special files, depth=%u with bounded FDs; workspace=%zu bytes\n",maximum_depth,size);
    return 0;
}
