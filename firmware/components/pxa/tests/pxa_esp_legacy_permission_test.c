#define _GNU_SOURCE
#include "../src/package/pxa_esp_permission_store.c"
#include <assert.h>

static unsigned readwrite,sets,erases,commits,reads;
static int present=1,fail_read;
static uint8_t old_decision=PXA_PERMISSION_ALLOW;
static char old_key[32];
void *heap_caps_malloc(size_t n,unsigned caps) {(void)caps;return malloc(n);}
void *heap_caps_realloc(void *p,size_t n,unsigned caps) {(void)caps;return realloc(p,n);}
void heap_caps_free(void *p) {free(p);}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle) {
    assert(!strcmp(name,"pxa_apps"));*handle=1;
    if(mode==NVS_READWRITE) {++readwrite;return ESP_FAIL;}
    assert(mode==NVS_READONLY);++reads;return fail_read?ESP_FAIL:ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *bytes) {
    assert(handle==1);
    if(!present || strcmp(key,old_key))return ESP_ERR_NVS_NOT_FOUND;
    if(out) {assert(*bytes>=1);*(uint8_t *)out=old_decision;}
    *bytes=1;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t n) {
    (void)h;(void)key;(void)in;(void)n;++sets;return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {(void)h;(void)key;++erases;return ESP_FAIL;}
esp_err_t nvs_commit(nvs_handle_t h) {(void)h;++commits;return ESP_FAIL;}
void nvs_close(nvs_handle_t handle) {assert(handle==1);}
static pxa_bytes_t text(const char *s) {return (pxa_bytes_t){(const uint8_t *)s,strlen(s)};}
int main(void) {
    const pxa_bytes_t identity=text("publisher:legacy"),name=text("audio.playback"),scope=text("media");
    pxa_permission_decision_t decision=PXA_PERMISSION_DENY;struct stat info;
    assert(!mkdir(CONFIG_PXA_MOUNT_POINT,0700));
    assert(derive_legacy_key(identity,name,scope,old_key));
    assert(!pxa_esp_permission_store_peek(NULL,identity,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    assert(present && reads && !readwrite && !sets && !erases && !commits);
    assert(stat(PXA_ESP_PERMISSION_STATE_ROOT,&info)<0 && errno==ENOENT);
    old_decision=PXA_PERMISSION_DENY;decision=PXA_PERMISSION_ALLOW;
    assert(!pxa_esp_permission_store_peek(NULL,identity,name,scope,&decision) && decision==PXA_PERMISSION_DENY);
    assert(present && !readwrite && !sets && !erases && !commits);
    present=0;decision=PXA_PERMISSION_ALLOW;
    assert(pxa_esp_permission_store_peek(NULL,identity,name,scope,&decision)==PXA_STATUS_NOT_FOUND && decision==PXA_PERMISSION_DENY);
    present=1;fail_read=1;decision=PXA_PERMISSION_ALLOW;
    assert(pxa_esp_permission_store_peek(NULL,identity,name,scope,&decision)==PXA_STATUS_INTERNAL && decision==PXA_PERMISSION_DENY);
    fail_read=0;old_decision=PXA_PERMISSION_DENY;
    /* Construct a real checksummed file policy through production persistence.
     * It takes priority over the old NVS row and remains read-only on peek. */
    assert(policy_save(identity,name,scope,PXA_PERMISSION_ALLOW)==PXA_STATUS_OK);
    unsigned before=reads;decision=PXA_PERMISSION_DENY;
    assert(!pxa_esp_permission_store_peek(NULL,identity,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW && reads==before);
    pxa_esp_permission_store_release_cache();assert(!g_permission_cache);
    pxa_esp_permission_store_release_cache();decision=PXA_PERMISSION_DENY;
    assert(!pxa_esp_permission_store_peek(NULL,identity,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW && reads==before);
    assert(!readwrite && !sets && !erases && !commits);
    char path[PXA_ESP_PERMISSION_PATH_BYTES];
    for(int slot=0;slot<2;++slot) {
        assert(build_slot_path(identity,slot,path,sizeof(path)));
        assert(!unlink(path) || errno==ENOENT);
    }
    pxa_esp_permission_store_release_cache();assert(!g_permission_cache);
    assert(!rmdir(PXA_ESP_PERMISSION_POLICY_ROOT) && !rmdir(PXA_ESP_PERMISSION_STATE_ROOT) && !rmdir(CONFIG_PXA_MOUNT_POINT));
    puts("Legacy permission snapshot: real A/B policy and NVS fallback, warm/cold reads, deny/errors, no filesystem/NVS writes or erasure passed");
    return 0;
}
