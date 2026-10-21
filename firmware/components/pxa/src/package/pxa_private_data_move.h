#ifndef PXA_PRIVATE_DATA_MOVE_H
#define PXA_PRIVATE_DATA_MOVE_H

#include "pxa/status.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(ESP_PLATFORM) && !defined(__linux__) && !defined(__APPLE__)
#define pxa_data_lstat stat /* Board mounts are link-free. */
#else
#define pxa_data_lstat lstat
#endif

/* -1: error or symlink; 0: missing; 1: directory. */
static int pxa_data_directory(const char *path) {
    struct stat st;
    if(pxa_data_lstat(path,&st)!=0)return errno==ENOENT?0:-1;
    return S_ISDIR(st.st_mode)?1:-1;
}
static int pxa_data_mkdirs(char *path) {
    for(char *p=path+1;;++p) {
        if(*p && *p!='/')continue;
        char saved=*p;*p=0;
        int state=pxa_data_directory(path);
        if(state<0 || (!state && mkdir(path,0700)!=0)){*p=saved;return 0;}
        *p=saved;if(!saved)return 1;
    }
}

/* The caller serializes app launch and package operations. A durable intent
 * makes a retry after lost acknowledgement/power resumable. Never merge or
 * replace a destination; rename fails across filesystems. Retain the receipt
 * outside data/ so the app cannot accidentally overwrite it with a save file. */
static pxa_status_t pxa_private_data_move(const char *from_root,const char *to_root,
                                         const char *source_identity) {
    char from[192],to[192],receipt[208],parent[192],owner[130];
    size_t length=strlen(source_identity);
    if(!length || length>=sizeof(owner) || strlen(from_root)>180 || strlen(to_root)>180)
        return PXA_STATUS_INVALID_ARGUMENT;
    snprintf(from,sizeof(from),"%s/data",from_root);
    snprintf(to,sizeof(to),"%s/data",to_root);
    snprintf(receipt,sizeof(receipt),"%s/.pxa-data-move",to_root);
    struct stat st;int recorded=0;
    if(pxa_data_lstat(receipt,&st)==0) {
        if(!S_ISREG(st.st_mode) || st.st_size!=(off_t)length)return PXA_STATUS_DENIED;
        int fd=open(receipt,O_RDONLY);if(fd<0)return PXA_STATUS_IO_ERROR;
        ssize_t n=read(fd,owner,sizeof(owner));close(fd);
        if(n!=(ssize_t)length || memcmp(owner,source_identity,length))return PXA_STATUS_DENIED;
        recorded=1;
    } else if(errno!=ENOENT)return PXA_STATUS_IO_ERROR;
    int source=pxa_data_directory(from),target=pxa_data_directory(to);
    if(source<0 || target<0)return PXA_STATUS_DENIED;
    if(target)return recorded && !source?PXA_STATUS_OK:PXA_STATUS_DENIED;
    if(!source)return PXA_STATUS_NOT_FOUND;
    snprintf(parent,sizeof(parent),"%s",to_root);
    if(!pxa_data_mkdirs(parent))return PXA_STATUS_IO_ERROR;
    if(!recorded) {
        int fd=open(receipt,O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0)return PXA_STATUS_IO_ERROR;
        int written=write(fd,source_identity,length)==(ssize_t)length;
        int synced=fsync(fd)==0;int closed=close(fd)==0;
        if(!written || !synced || !closed)return PXA_STATUS_IO_ERROR;
    }
#ifndef ESP_PLATFORM
    /* Persist the new receipt's directory entry before the move is possible. */
    int intent_dir=open(to_root,O_RDONLY|O_DIRECTORY);
    if(intent_dir<0)return PXA_STATUS_IO_ERROR;
    int intent_synced=fsync(intent_dir);close(intent_dir);
    if(intent_synced)return PXA_STATUS_IO_ERROR;
#endif
    if(rename(from,to)!=0)return PXA_STATUS_IO_ERROR;
#ifndef ESP_PLATFORM
    int fd=open(to_root,O_RDONLY|O_DIRECTORY);
    if(fd<0)return PXA_STATUS_IO_ERROR;
    int synced=fsync(fd);close(fd);if(synced)return PXA_STATUS_IO_ERROR;
    fd=open(from_root,O_RDONLY|O_DIRECTORY);if(fd<0)return PXA_STATUS_IO_ERROR;
    synced=fsync(fd);close(fd);if(synced)return PXA_STATUS_IO_ERROR;
#endif
    return PXA_STATUS_OK;
}
#undef pxa_data_lstat
#endif
