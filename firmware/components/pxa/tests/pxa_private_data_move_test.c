#define _POSIX_C_SOURCE 200809L
#include "pxa_private_data_move.h"
#include <assert.h>
#include <stdlib.h>

static void put(const char *path,const char *bytes) {
    int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);
    size_t n=strlen(bytes);assert(write(fd,bytes,n)==(ssize_t)n);assert(close(fd)==0);
}
int main(void) {
    char directory[]="/tmp/pxa-data-move-XXXXXX";assert(mkdtemp(directory));
    char old[180],next[180],path[220],dest[220];
    snprintf(old,sizeof(old),"%s/old",directory);assert(mkdir(old,0700)==0);
    snprintf(next,sizeof(next),"%s/new/publisher/app",directory);
    snprintf(path,sizeof(path),"%s/data",old);assert(mkdir(path,0700)==0);
    snprintf(path,sizeof(path),"%s/data/save",old);put(path,"saved game");
    snprintf(path,sizeof(path),"%s/.pxa-storage",old);put(path,"old grants");
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_OK);
    snprintf(dest,sizeof(dest),"%s/data/save",next);assert(access(dest,F_OK)==0);
    assert(access(path,F_OK)==0); /* Grants do not cross identities. */
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_OK); /* Lost ACK. */
    assert(pxa_private_data_move(old,next,"publisher:other")==PXA_STATUS_DENIED);
    snprintf(path,sizeof(path),"%s/data",old);assert(mkdir(path,0700)==0);
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_DENIED);
    assert(rmdir(path)==0);
    /* Resume after durable intent, before rename. */
    assert(unlink(dest)==0);snprintf(dest,sizeof(dest),"%s/data",next);assert(rmdir(dest)==0);
    assert(mkdir(path,0700)==0);
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_OK);
    assert(rmdir(dest)==0);
    /* A destination without a receipt must never be adopted. */
    snprintf(dest,sizeof(dest),"%s/.pxa-data-move",next);assert(unlink(dest)==0);
    snprintf(dest,sizeof(dest),"%s/data",next);assert(mkdir(dest,0700)==0);
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_DENIED);
    assert(rmdir(dest)==0);
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_NOT_FOUND);
    assert(symlink("/tmp",path)==0);
    assert(pxa_private_data_move(old,next,"publisher:old")==PXA_STATUS_DENIED);
    assert(unlink(path)==0);
    /* Clean only this test's exact files and empty directories. */
    snprintf(path,sizeof(path),"%s/.pxa-storage",old);assert(unlink(path)==0);assert(rmdir(old)==0);
    assert(rmdir(next)==0);*strrchr(next,'/')=0;assert(rmdir(next)==0);
    *strrchr(next,'/')=0;assert(rmdir(next)==0);assert(rmdir(directory)==0);
    puts("private data migration: passed");return 0;
}
