#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pxa_esp_package_store.h"
#define PXA_ESP_PACKAGE_MAX_ID 65
#define PXA_ESP_PACKAGE_MAX_PATH 160
#define PXA_ESP_PACKAGE_MAX_ASSET_PATH 256
#define PXA_ESP_SIGNED_MANIFEST_MAX_BYTES (128u * 1024u)
#define PXA_ESP_HOST_MAX_PATH 160
typedef struct {char name[128],description[512],icon_path[256];} pxa_host_package_metadata_t;
static struct {int initialized;} g_host={1};
#define ESP_LOGE(...) ((void)0)
typedef struct {
    char id[65],root[160]; uint8_t publisher_key_id[32];
    int installed,has_publisher_key_id,ambiguous;
} pxa_esp_package_entry_t;
static pxa_esp_package_entry_t entry;
static struct {void *metadata_lock,*transaction_lock;pxa_package_limits_t limits;} store,*g_store=&store;
static int metadata,transaction,allocations,fail_allocation,live,reads,short_read;
static int update_before_transaction;
static void *failed_lock;
static bool store_is_initialized(void) {return true;}
static bool take_lock(void *lock) {
    int *held=lock;assert(!*held);
    if(lock==failed_lock)return false;
    if(lock==store.transaction_lock && update_before_transaction) {
        /* An update can finish while the loader waits for its transaction
         * lease. The catalog then points at the new generation only. */
        assert(!metadata);assert(rename("package","package-next")==0);
        strcpy(entry.root,"package-next");update_before_transaction=0;
    }
    *held=1;return true;
}
static void release_lock(void *lock) {int *held=lock;assert(*held);*held=0;}
static pxa_esp_package_entry_t *find_entry(const char *id) {return !strcmp(id,"test")?&entry:NULL;}
static void *esp_alloc(size_t n) {
    if (++allocations==fail_allocation)return NULL;
    void *p=malloc(n);if(p)++live;return p;
}
static void release_buffer(void *p) {if(p) {assert(live>0);--live;}free(p);}
static FILE *open_manifest(const char *path,const char *mode) {
    assert(transaction);if(!strcmp(mode,"rb"))++reads;return fopen(path,mode);
}
static size_t read_manifest(void *p,size_t s,size_t n,FILE *f) {
    size_t got=fread(p,s,n,f);return short_read && got?got-1:got;
}
#define fopen open_manifest
#define fread read_manifest
#define free release_buffer
// PRODUCTION
#undef fopen
#undef fread
#undef free
static void discard(pxa_posix_installer_result_t *r) {
    release_buffer(r->manifest_workspace);release_buffer(r->encoded);
    r->manifest_workspace=NULL;r->encoded=NULL;*r->manifest=NULL;
}
static void rejected(pxa_posix_installer_result_t *r) {
    assert(!pxa_esp_package_store_prepare_launch("test",r));
    assert(!*r->manifest && !r->manifest_workspace && !r->encoded && !live);
    assert(!metadata && !transaction);
}
int main(void) {
    store.metadata_lock=&metadata;store.transaction_lock=&transaction;
    pxa_package_limits_init(&store.limits);
    strcpy(entry.root,"package");strcpy(entry.id,"launch-fixture");
    entry.installed=entry.has_publisher_key_id=1;
    FILE *pub=fopen("publisher.bin","rb");assert(pub);
    assert(fread(entry.publisher_key_id,1,32,pub)==32);fclose(pub);
    char root[160];pxa_package_manifest_t *manifest=NULL;
    pxa_posix_installer_result_t r={.struct_size=sizeof(r),.root=root,.root_capacity=sizeof(root),.manifest=&manifest};
    for(int i=0;i<3;++i) {
        reads=allocations=0;
        assert(pxa_esp_package_store_prepare_launch("test",&r));
        assert(reads==1 && live==2 && manifest && !strcmp(root,entry.root));
        assert(manifest->app_id.size==strlen(entry.id));discard(&r);assert(!live);
    }
    update_before_transaction=1;
    assert(pxa_esp_package_store_prepare_launch("test",&r));
    assert(!strcmp(root,"package-next"));discard(&r);
    assert(rename("package-next","package")==0);strcpy(entry.root,"package");
    failed_lock=store.transaction_lock;rejected(&r);
    failed_lock=store.metadata_lock;rejected(&r);failed_lock=NULL;
    for(int fail=1;fail<=2;++fail) {allocations=0;fail_allocation=fail;rejected(&r);}
    fail_allocation=0;
    strcpy(entry.id,"wrong-app");rejected(&r);strcpy(entry.id,"launch-fixture");
    entry.publisher_key_id[0]^=1;rejected(&r);entry.publisher_key_id[0]^=1;
    short_read=1;rejected(&r);short_read=0;
    entry.ambiguous=1;reads=0;rejected(&r);assert(!reads);entry.ambiguous=0;
    /* Desktop localization shares the current-generation snapshot loader:
     * one manifest read per app, with ownership released on all failures. */
    pxa_host_package_metadata_t localized;
    reads=0;
    assert(pxa_esp_host_resolve_package_metadata("test","zh-CN",&localized));
    assert(reads==1 && localized.name[0] && !live);
    for(int fail=1;fail<=2;++fail) {
        allocations=0;fail_allocation=fail;
        assert(!pxa_esp_host_resolve_package_metadata("test","zh-CN",&localized));
        assert(!live && !metadata && !transaction);
    }
    fail_allocation=0;entry.ambiguous=1;reads=0;
    assert(!pxa_esp_host_resolve_package_metadata("test","zh-CN",&localized));
    assert(!reads && !live);entry.ambiguous=0;
    r.root_capacity=1;rejected(&r);r.root_capacity=sizeof(root);
    FILE *file=fopen("package/manifest.pxm","r+b");assert(file);
    int first=fgetc(file);rewind(file);fputc(first^255,file);fclose(file);rejected(&r);
    file=fopen("package/manifest.pxm","r+b");assert(file);fputc(first,file);fclose(file);
    assert(pxa_esp_package_store_prepare_launch("test",&r));discard(&r);
    assert(truncate("package/manifest.pxm",PXA_ESP_SIGNED_MANIFEST_MAX_BYTES+1)==0);
    reads=0;rejected(&r);assert(!reads);
    assert(truncate("package/manifest.pxm",0)==0);rejected(&r);
    assert(unlink("package/manifest.pxm")==0);rejected(&r);
    puts("PASS: current launch generation, lock ordering, one-read manifest and failure cleanup");
}
