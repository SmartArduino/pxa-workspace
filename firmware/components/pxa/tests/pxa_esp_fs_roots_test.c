#define _GNU_SOURCE
#include "pxa_esp_fs_roots.h"
#include "pxa_esp_ui_shell.h"
#include "pxa/pxa_esp_storage.h"
#include "esp_vfs_fat.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/hmac.h>
#include "../src/services/pxa_esp_fs_roots.c"

static unsigned random_state=1,allocation_count;
static void *allocations[8];
struct pxa_esp_app_storage {const char *root;uint8_t key[32];uint64_t media_epoch;};
uint64_t pxa_esp_app_storage_media_epoch(const pxa_esp_app_storage_t *storage) {return storage?storage->media_epoch:0;}
pxa_status_t pxa_esp_app_storage_root(pxa_esp_app_storage_t *storage,const char **root) {
    assert(storage);*root=storage->root;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_app_storage_seal_grants(pxa_esp_app_storage_t *storage,pxa_bytes_t body,uint8_t tag[32]) {
    unsigned size=0;
    assert(HMAC(EVP_sha256(),storage->key,32,body.data,body.size,tag,&size) && size==32);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_app_storage_verify_grants(pxa_esp_app_storage_t *storage,pxa_bytes_t body,const uint8_t tag[32]) {
    uint8_t expected[32];assert(!pxa_esp_app_storage_seal_grants(storage,body,expected));
    return memcmp(expected,tag,32)?PXA_STATUS_DENIED:PXA_STATUS_OK;
}
void *heap_caps_malloc(size_t bytes,unsigned caps) {(void)caps;return malloc(bytes);}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {(void)caps;return calloc(n,bytes);}
void heap_caps_free(void *memory) {free(memory);}
uint32_t esp_random(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
void esp_fill_random(void *output,size_t bytes) {
    uint8_t *p=output;for(size_t i=0;i<bytes;++i)p[i]=(uint8_t)esp_random();
}
esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *free_bytes) {
    (void)path;*total=4000000;*free_bytes=3000000;return ESP_OK;
}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
int pxa_esp_ui_shell_post_directory_prompt(const pxa_esp_ui_directory_prompt_t *p) {(void)p;assert(0);return 0;}
void pxa_esp_ui_shell_dismiss_directory_prompt(uint32_t id) {(void)id;assert(0);}
void pxa_esp_surface_runtime_modal_enter(void) {assert(0);}
void pxa_esp_surface_runtime_modal_leave(void) {assert(0);}
static void *allocate(void *context,size_t bytes) {
    (void)context;assert(allocation_count<8);
    void *p=calloc(1,bytes);assert(p);allocations[allocation_count++]=p;return p;
}
static pxa_bytes_t bytes(const char *p) {return (pxa_bytes_t){(const uint8_t *)p,strlen(p)};}
int main(void) {
    pxa_esp_fs_roots_t *roots;pxa_fs_roots_backend_t *backend;
    void *directory,*file;uint32_t rights;pxa_fs_entry_t entry;uint64_t features;
    size_t count;uint8_t data[8]={0},id[16]={0};pxa_volume_info_t info;
    assert(!mkdir(CONFIG_PXA_MOUNT_POINT,0700));
    assert(!pxa_esp_storage_register_fat(CONFIG_PXA_MOUNT_POINT,"test card","card",4,4000000,3000000,3,4096));
    assert(!pxa_esp_storage_select_app_media(CONFIG_PXA_MOUNT_POINT));
    char selected[256],temporary[512];
    assert(!pxa_esp_storage_media_mount(selected,sizeof(selected)) && !selected[0]);
    assert(pxa_esp_storage_debug_path("PXA/media/test/quake/pak0.pak",false,temporary,sizeof(temporary)) &&
        !strcmp(temporary,CONFIG_PXA_MOUNT_POINT "/.pxa-pxadb-upload"));
    assert(!pxa_esp_storage_debug_path("PXA/system/fonts/font.ttf",false,NULL,0));
    assert(pxa_esp_storage_debug_path("PXA/system/fonts/font.ttf",true,NULL,0));
    assert(!pxa_esp_storage_debug_path("PXA/system",true,NULL,0));
    pxa_esp_fs_roots_config_t config={.allocate=allocate,.identity="test:quake",.name="Quake",.media_relative="PXA/media/test/quake"};
    assert(!pxa_esp_fs_roots_create(&config,&roots,&backend));
    assert(!backend->open_root(backend->context,PXA_FS_ROOT_APP_MEDIA,id,&directory,&rights));
    int native=open(CONFIG_PXA_MOUNT_POINT "/PXA/media/test/quake/pak0.pak",O_CREAT|O_WRONLY|O_EXCL,0600);
    assert(native>=0 && write(native,"PACK0123456789",14)==14 && !close(native));
    assert(!backend->open_at(backend->context,directory,bytes("pak0.pak"),PXA_FS_OPEN_READ,&file,&entry,&features));
    /* Root callbacks use the wrapper. File operations, including READ_AT,
     * use files.context as required by the public FS provider contract. */
    assert(backend->context!=backend->files.context);
    assert(!backend->read_at(backend->files.context,file,4,data,4,&count));
    assert(count==4 && !memcmp(data,"0123",4));
    assert(!backend->files.read(backend->files.context,file,data,4,&count));
    assert(count==4 && !memcmp(data,"PACK",4));
    assert(!backend->read_at(backend->files.context,file,100,data,4,&count) && !count);
    backend->files.close(backend->files.context,file,PXA_FS_KIND_REGULAR);
    backend->files.close(backend->files.context,directory,PXA_FS_KIND_DIRECTORY);
    pxa_esp_fs_roots_destroy(roots);
    while(allocation_count)free(allocations[--allocation_count]);
    assert(pxa_esp_storage_volumes(&info,1)==1);
    /* An install binds its own media volume even when the product's legacy
     * default points elsewhere. Late registration does not recreate Guests. */
    assert(!mkdir(CONFIG_PXA_MOUNT_POINT "/second",0700));
    assert(!pxa_esp_storage_register_fat(CONFIG_PXA_MOUNT_POINT "/second","second card","card2",5,4000000,3000000,3,4096));
    pxa_volume_info_t volumes[2];assert(pxa_esp_storage_volumes(volumes,2)==2);
    uint64_t second_epoch=volumes[0].mount_epoch==info.mount_epoch?volumes[1].mount_epoch:volumes[0].mount_epoch;
    assert(pxa_esp_storage_debug_path("second/PXA/media/test/quake/pak0.pak",false,temporary,sizeof(temporary)) &&
        !strcmp(temporary,CONFIG_PXA_MOUNT_POINT "/second/.pxa-pxadb-upload"));
    config.media_mount_epoch=second_epoch;
    assert(!pxa_esp_fs_roots_create(&config,&roots,&backend));
    assert(!backend->open_root(backend->context,PXA_FS_ROOT_APP_MEDIA,id,&directory,&rights));
    native=open(CONFIG_PXA_MOUNT_POINT "/second/PXA/media/test/quake/pak0.pak",O_CREAT|O_WRONLY|O_EXCL,0600);
    assert(native>=0 && write(native,"SECOND",6)==6 && !close(native));
    assert(!backend->open_at(backend->context,directory,bytes("pak0.pak"),PXA_FS_OPEN_READ,&file,&entry,&features));
    assert(!backend->read_at(backend->files.context,file,0,data,6,&count) && count==6 && !memcmp(data,"SECOND",6));
    backend->files.close(backend->files.context,file,PXA_FS_KIND_REGULAR);
    backend->files.close(backend->files.context,directory,PXA_FS_KIND_DIRECTORY);
    pxa_esp_fs_roots_destroy(roots);while(allocation_count)free(allocations[--allocation_count]);
    char managed[256]="PXA/media/",absolute[768];
    memset(managed+10,'a',64);strcpy(managed+74,"/quake/");
    memset(managed+81,'b',64);strcpy(managed+145,"/package");
    config.media_relative=managed;
    assert(!pxa_esp_fs_roots_create(&config,&roots,&backend));
    assert(backend->open_root(backend->context,PXA_FS_ROOT_APP_MEDIA,id,&directory,&rights)==PXA_STATUS_UNAVAILABLE);
    assert(snprintf(absolute,sizeof(absolute),"%s/second/%s",CONFIG_PXA_MOUNT_POINT,managed)<(int)sizeof(absolute));
    assert(access(absolute,F_OK)!=0); /* No empty signed generation is made. */
    for(char *part=absolute+strlen(CONFIG_PXA_MOUNT_POINT)+1;*part;++part)if(*part=='/') {
        *part=0;assert(!mkdir(absolute,0700) || errno==EEXIST);*part='/';
    }
    assert(!mkdir(absolute,0700));
    assert(!backend->open_root(backend->context,PXA_FS_ROOT_APP_MEDIA,id,&directory,&rights));
    assert(rights==(PXA_FS_RIGHT_READ|PXA_FS_RIGHT_ENUMERATE));
    backend->files.close(backend->files.context,directory,PXA_FS_KIND_DIRECTORY);
    assert(!pxa_esp_storage_drain_volume(second_epoch));
    assert(!pxa_esp_storage_debug_path("second/PXA/media/test/quake/pak0.pak",false,NULL,0));
    assert(pxa_esp_storage_unregister_volume(second_epoch)==PXA_STATUS_BUSY);
    pxa_esp_fs_roots_destroy(roots);while(allocation_count)free(allocations[--allocation_count]);
    assert(!pxa_esp_storage_unregister_volume(second_epoch));
    for(unsigned i=0;i<4;++i) {assert(!rmdir(absolute));*strrchr(absolute,'/')=0;}
    assert(!unlink(CONFIG_PXA_MOUNT_POINT "/second/PXA/media/test/quake/pak0.pak"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/second/PXA/media/test/quake"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/second/PXA/media/test"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/second/PXA/media"));
    assert(!unlink(CONFIG_PXA_MOUNT_POINT "/second/PXA/volume.0.pxv"));
    assert(!unlink(CONFIG_PXA_MOUNT_POINT "/second/PXA/volume.1.pxv"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/second/PXA") && !rmdir(CONFIG_PXA_MOUNT_POINT "/second"));
    /* Persisted directory authority follows the activation's private root.
     * Recomputing the old unkeyed digest cannot forge a new grant. */
    assert(!mkdir(CONFIG_PXA_MOUNT_POINT "/PXA/apps",0700));
    pxa_esp_app_storage_t storage={.root=CONFIG_PXA_MOUNT_POINT "/PXA/apps",.key={1}};
    config.media_mount_epoch=info.mount_epoch;config.app_storage=&storage;config.user_directories=1;
    config.media_relative="PXA/media/test/quake";
    assert(!pxa_esp_fs_roots_create(&config,&roots,&backend));
    storage.media_epoch=info.mount_epoch;
    strcpy(roots->media_relative,"PXA/media/test/missing");
    assert(app_media_grant(roots)==PXA_STATUS_UNAVAILABLE);
    struct stat absent;assert(stat(CONFIG_PXA_MOUNT_POINT "/PXA/media/test/missing",&absent)<0);
    strcpy(roots->media_relative,"PXA/media/test/quake");
    assert(!app_media_grant(roots));storage.media_epoch=0;
    roots->persisted_mask=1;roots->grants[0].active=1;roots->grants[0].id[0]=1;
    roots->grants[0].volume_id=info.id;roots->grants[0].revision=1;roots->grants[0].rights=3;
    strcpy(roots->grants[0].relative_path,"documents");
    assert(!strncmp(roots->store_path,storage.root,strlen(storage.root)) && save_grants(roots));
    pxa_scoped_fs_grant_t saved=roots->grants[0];
    memset(roots->grants,0,sizeof(roots->grants));roots->persisted_mask=0;load_grants(roots);
    assert(roots->persisted_mask==1 && !strcmp(roots->grants[0].relative_path,"documents"));
    uint8_t encoded[GRANT_FILE_BYTES];native=open(roots->store_path,O_RDWR);assert(native>=0);
    assert(read(native,encoded,sizeof(encoded))==sizeof(encoded));encoded[16]^=8;
    assert(digest(encoded,sizeof(encoded)-32,encoded+sizeof(encoded)-32));
    assert(pwrite(native,encoded,sizeof(encoded),0)==sizeof(encoded) && !close(native));
    memset(roots->grants,0,sizeof(roots->grants));roots->persisted_mask=0;load_grants(roots);assert(!roots->persisted_mask);
    roots->grants[0]=saved;roots->persisted_mask=1;assert(save_grants(roots));
    storage.key[0]=2;memset(roots->grants,0,sizeof(roots->grants));roots->persisted_mask=0;load_grants(roots);
    assert(!roots->persisted_mask);storage.key[0]=1;load_grants(roots);assert(roots->persisted_mask==1);
    assert(!unlink(roots->store_path));
    pxa_esp_fs_roots_destroy(roots);while(allocation_count)free(allocations[--allocation_count]);
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/PXA/apps"));
    assert(!pxa_esp_storage_drain_volume(info.mount_epoch) && !pxa_esp_storage_unregister_volume(info.mount_epoch));
    assert(!unlink(CONFIG_PXA_MOUNT_POINT "/PXA/media/test/quake/pak0.pak"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/PXA/media/test/quake"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/PXA/media/test"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/PXA/media"));
    assert(!unlink(CONFIG_PXA_MOUNT_POINT "/PXA/volume.0.pxv"));
    assert(!unlink(CONFIG_PXA_MOUNT_POINT "/PXA/volume.1.pxv"));
    assert(!rmdir(CONFIG_PXA_MOUNT_POINT "/PXA") && !rmdir(CONFIG_PXA_MOUNT_POINT));
    puts("ESP roots: READ_AT contexts/cursors, per-install media mounts and authenticated private directory grants passed");
    return 0;
}
