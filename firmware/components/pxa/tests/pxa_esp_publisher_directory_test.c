#define _GNU_SOURCE
#include "pxa_esp_publisher_directory.h"
#include "pxa_esp_record_seal.h"
#include "pxa/pxa_esp_storage.h"
#include "record_test_auth.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static record_test_auth_context_t key={{1},1};
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,uint8_t tag[32]) {
    return record_test_tag(&key,purpose,binding,body,tag);
}
pxa_status_t pxa_esp_storage_volume(uint64_t epoch,pxa_volume_info_t *out) {(void)epoch;(void)out;return PXA_STATUS_UNAVAILABLE;}
pxa_status_t pxa_esp_storage_volume_path(uint64_t epoch,char *out,size_t size) {(void)epoch;(void)out;(void)size;return PXA_STATUS_UNAVAILABLE;}
static void path(char *out,const char *base,const char *name) {assert(snprintf(out,512,"%s/%s",base,name)<512);}
int main(void) {
    char root[]="/tmp/pxa-publisher-XXXXXX",logical[130],other[130],physical[130],reverse[130],old[512],alias[512],marker[512];
    assert(mkdtemp(root));pxa_volume_id_t volume={{1}},foreign={{2}};uint8_t first[32]={1},second[32]={2};
    memset(logical,'0',64);logical[1]='1';strcpy(logical+64,"/game");strcpy(other,logical);other[1]='2';
    assert(!pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical)) && !strcmp(physical,logical));
    assert(!pxa_esp_publisher_bind(root,&volume,"apps",first,"lucinhu"));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical)) && !strcmp(physical,"lucinhu/game"));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",physical,1,reverse,sizeof(reverse)) && !strcmp(reverse,logical));
    /* First verified reservation is stable; subsequent profile renames cannot
     * move existing saves or steal another publisher's directory. */
    assert(!pxa_esp_publisher_bind(root,&volume,"apps",first,"another"));
    assert(!pxa_esp_publisher_bind(root,&volume,"apps",second,"lucinhu"));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",other,0,physical,sizeof(physical)) && !strcmp(physical,"lucinhu-02000000/game"));
    /* Public removable storage may contain ordinary notes alongside publisher
     * directories. They must neither hide existing apps nor block a new owner. */
    char note[512],broken[512],broken_marker[512],unclaimed[130];uint8_t third[32]={3};
    strcpy(unclaimed,logical);unclaimed[1]='3';path(note,root,"apps/readme.txt");
    int note_fd=open(note,O_WRONLY|O_CREAT|O_EXCL,0600);assert(note_fd>=0);
    assert(write(note_fd,"notes",5)==5 && !close(note_fd));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical)) && !strcmp(physical,"lucinhu/game"));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",unclaimed,0,physical,sizeof(physical)) && !strcmp(physical,unclaimed));
    /* An unreadable unrelated marker cannot invalidate a verified owner.
     * Its unknown ownership still forbids inventing a fresh empty home. */
    path(broken,root,"apps/broken");assert(!mkdir(broken,0700));path(broken_marker,broken,".publisher.pxi");
    note_fd=open(broken_marker,O_WRONLY|O_CREAT|O_EXCL,0600);assert(note_fd>=0);
    assert(write(note_fd,"bad",3)==3 && !close(note_fd));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical)) && !strcmp(physical,"lucinhu/game"));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",other,0,physical,sizeof(physical)) && !strcmp(physical,"lucinhu-02000000/game"));
    assert(pxa_esp_publisher_map(root,&volume,"apps",unclaimed,0,physical,sizeof(physical))==PXA_STATUS_DENIED);
    assert(pxa_esp_publisher_bind(root,&volume,"apps",third,"new-owner")==PXA_STATUS_DENIED);
    assert(!unlink(broken_marker) && !rmdir(broken) && !unlink(note));
    assert(pxa_esp_publisher_map(root,&foreign,"apps",logical,0,physical,sizeof(physical))==PXA_STATUS_DENIED);
    /* A committed marker at the old full-root directory is the recoverable
     * state immediately before rename. Reverse discovery and retry both work. */
    path(alias,root,"apps/lucinhu");char relative[80];snprintf(relative,sizeof(relative),"apps/%.64s",logical);path(old,root,relative);
    assert(!rename(alias,old));
    assert(pxa_esp_publisher_reserve(root,&volume,"apps",first,"lucinhu",0)==PXA_STATUS_BUSY);
    assert(access(alias,F_OK)<0 && errno==ENOENT && !access(old,F_OK));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical)) && !strcmp(physical,logical));
    assert(!pxa_esp_publisher_map(root,&volume,"apps",physical,1,reverse,sizeof(reverse)) && !strcmp(reverse,logical));
    assert(!pxa_esp_publisher_bind(root,&volume,"apps",first,"lucinhu") && access(old,F_OK)<0 && errno==ENOENT);
    /* Existing canonical plus alias is ambiguous, never a second save. */
    assert(!mkdir(old,0700));
    assert(pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical))==PXA_STATUS_DENIED);
    assert(!rmdir(old));
    /* Media is independent: an interrupted apps/media migration needs no
     * installation-record rewrite and keeps each tree individually valid. */
    assert(!pxa_esp_publisher_map(root,&volume,"media",logical,0,physical,sizeof(physical)) && !strcmp(physical,logical));
    assert(!pxa_esp_publisher_bind(root,&volume,"media",first,"lucinhu"));
    assert(!pxa_esp_publisher_map(root,&volume,"media",logical,0,physical,sizeof(physical)) && !strcmp(physical,"lucinhu/game"));
    /* Runtime calls may reserve a new name, but must not rename shared media
     * paths that another application can already hold open. */
    char reserved[512],reserved_marker[512],canonical_media[512];
    snprintf(relative,sizeof(relative),"media/%.64s",unclaimed);path(canonical_media,root,relative);
    assert(!mkdir(canonical_media,0700));
    assert(pxa_esp_publisher_reserve(root,&volume,"media",third,"new-owner",0)==PXA_STATUS_BUSY);
    assert(!pxa_esp_publisher_map(root,&volume,"media",unclaimed,0,physical,sizeof(physical)) && !strcmp(physical,unclaimed));
    assert(!rmdir(canonical_media));
    assert(!pxa_esp_publisher_reserve(root,&volume,"media",third,"new-owner",0));
    assert(!pxa_esp_publisher_map(root,&volume,"media",unclaimed,0,physical,sizeof(physical)) && !strcmp(physical,"new-owner/game"));
    assert(!pxa_esp_publisher_reserve(root,&volume,"media",third,"renamed",0));
    path(reserved,root,"media/new-owner");path(reserved_marker,reserved,".publisher.pxi");
    assert(!unlink(reserved_marker) && !rmdir(reserved));
    key.available=0;assert(pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical))==PXA_STATUS_UNAVAILABLE);key.available=1;
    path(marker,alias,".publisher.pxi");int fd=open(marker,O_RDWR);assert(fd>=0);uint8_t changed=99;
    assert(pwrite(fd,&changed,1,15)==1 && !close(fd));
    assert(pxa_esp_publisher_map(root,&volume,"apps",logical,0,physical,sizeof(physical))==PXA_STATUS_DENIED);
    assert(pxa_esp_publisher_map(root,&volume,"apps","../game",0,physical,sizeof(physical))==PXA_STATUS_INVALID_ARGUMENT);
    assert(pxa_esp_publisher_bind(root,&volume,"apps",first,"../escape")==PXA_STATUS_INVALID_ARGUMENT);
    assert(!unlink(marker) && !rmdir(alias));
    path(alias,root,"apps/lucinhu-02000000");path(marker,alias,".publisher.pxi");assert(!unlink(marker) && !rmdir(alias));
    path(alias,root,"apps");assert(!rmdir(alias));
    path(alias,root,"media/lucinhu");path(marker,alias,".publisher.pxi");assert(!unlink(marker) && !rmdir(alias));
    path(alias,root,"media");assert(!rmdir(alias) && !rmdir(root));
    puts("Publisher directories: stable names, collision isolation, old/new recovery, media independence, ordinary files/unrelated broken markers, ambiguous/tampered/foreign bindings and unavailable key passed");
    return 0;
}
