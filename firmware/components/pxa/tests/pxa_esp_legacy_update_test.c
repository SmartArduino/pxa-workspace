/* Production snapshot/commit bodies are included by test_legacy_update.py.
 * Native filesystem and actual signed installer exercise the transaction. */
#define _GNU_SOURCE
#include "pxa_esp_install_approval.h"
#include "pxa_esp_legacy_update.h"
#include "pxa_esp_installations.h"
#include "pxa/posix/pxa_posix_installer.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/openssl/pxa_openssl.h"
#include <openssl/sha.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define PXA_ESP_PACKAGE_MAX_ASSET_PATH 512
#define esp_alloc(n) calloc(1,n)
#define heap_caps_free free
static char test_mount[512];
#define CONFIG_PXA_MOUNT_POINT test_mount
typedef struct {
    int installed,volume_local,ambiguous,built_in;
    char root[160],id[65],identity[131];uint8_t publisher_key_id[32];
} pxa_esp_package_entry_t;
static struct {int legacy_ready;pxa_posix_installer_t *installer;pxa_posix_installer_config_t installer_config;} store,*g_store=&store;
static pxa_esp_package_entry_t entry;
static uint8_t encoded[65536],workspace[262144];
static pxa_volume_registry_t *registry;
static pxa_volume_info_t volume;
static uint64_t epoch;
static char adopted[512];
static unsigned layout_writes;
static int path_is_within(const char *p,const char *parent) {
    size_t n=strlen(parent);return !strncmp(p,parent,n)&&(!p[n]||p[n]=='/');
}
struct pxa_esp_installations {pxa_volume_info_t volume;};
static struct pxa_esp_installations planning_owner;
static pxa_status_t pxa_esp_storage_volume(uint64_t e,pxa_volume_info_t *out) {return pxa_volume_get(registry,e,out);}
static pxa_status_t pxa_esp_storage_refresh_volume(uint64_t e) {return e==epoch?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;}
static pxa_status_t pxa_esp_storage_plan(const pxa_install_requirements_t *r,uint64_t a,uint64_t m,pxa_install_plan_t *out) {
    return pxa_install_plan_evaluate(registry,r,a,m,out);
}
static pxa_status_t pxa_esp_storage_reserve_plan(pxa_install_plan_t *p) {return pxa_install_plan_reserve(registry,p);}
static pxa_status_t pxa_esp_storage_release_plan(pxa_install_plan_t *p) {return pxa_install_plan_release(registry,p);}
pxa_status_t pxa_esp_installations_open(uint64_t e,pxa_esp_installations_t **out) {
    pxa_status_t s=pxa_volume_get(registry,e,&planning_owner.volume);*out=s?NULL:&planning_owner;return s;
}
static pxa_status_t close_volume_operation(pxa_esp_installations_t **owner,pxa_status_t status) {*owner=NULL;return status;}
static pxa_status_t available(pxa_esp_installations_t *owner) {return owner==&planning_owner?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;}
uint64_t pxa_esp_installations_private_initial_bytes(void) {assert(!"Legacy update never creates a new private image");return 0;}
static pxa_esp_package_entry_t *find_entry(const char *id) {(void)id;return entry.installed?&entry:NULL;}
static int is_directory(const char *p) {struct stat s;return !lstat(p,&s)&&S_ISDIR(s.st_mode);}
static int pxa_host_container_stream_volume_path(uint64_t e,const char *p) {return e==epoch&&!strcmp(p,adopted);}
static pxa_status_t pxa_esp_storage_resolve_volume(const char *p,pxa_volume_info_t *out) {(void)p;*out=volume;return PXA_STATUS_OK;}
static void pxa_esp_volumes_lock(void) {}
static void pxa_esp_volumes_unlock(void) {}
static pxa_volume_registry_t *pxa_esp_volumes_registry(void) {return registry;}
static pxa_status_t pxa_esp_mbedtls_sha256(const uint8_t *p,size_t n,uint8_t out[32]) {return SHA256(p,n,out)?PXA_STATUS_OK:PXA_STATUS_INTERNAL;}
static int manifest_workspace_for_encoded(const uint8_t *p,size_t n,pxa_package_limits_t *limits,size_t *bytes) {
    pxa_package_limits_t maximum;pxa_package_limits_init(&maximum);
    if(pxa_package_manifest_measure((pxa_bytes_t){p,n},&maximum,limits))return 0;
    *bytes=pxa_package_manifest_workspace_size(limits);return *bytes!=0;
}
static pxa_status_t read_source_into_store(const char *source,pxa_posix_installer_result_t *result,int only) {
    assert(only);result->encoded=encoded;result->encoded_capacity=sizeof(encoded);
    result->manifest_workspace=workspace;result->manifest_workspace_size=sizeof(workspace);
    return pxa_posix_installer_read_signed_manifest(store.installer,source,result);
}
static int parse_manifest_file(const char *path,pxa_package_manifest_t **out) {
    FILE *f=fopen(path,"rb");if(!f)return 0;
    size_t n=fread(encoded,1,sizeof(encoded),f);fclose(f);
    pxa_package_limits_t limits;size_t needed;
    if(!manifest_workspace_for_encoded(encoded,n,&limits,&needed) || needed>sizeof(workspace))return 0;
    return !pxa_package_manifest_parse(workspace,sizeof(workspace),(pxa_bytes_t){encoded,n},&limits,out);
}
static pxa_status_t verified_incoming_layout(void *ctx,const pxa_posix_installer_identity_t *id,const char *root) {
    (void)ctx;(void)id;assert(is_directory(root));++layout_writes;return PXA_STATUS_OK;
}
static int refresh_entry_from_manifest(const char *app,const char *root,int builtin,const pxa_package_manifest_t *m) {
    assert(m && !strcmp(app,"volume-test") && strlen(root)<sizeof(entry.root));
    strcpy(entry.root,root);entry.built_in=builtin;return 1;
}
// PRODUCTION
static pxa_bytes_t file_bytes(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
    uint8_t *b=malloc(n);assert(b && fread(b,1,n,f)==(size_t)n);fclose(f);return (pxa_bytes_t){b,(size_t)n};
}
static void text_file(const char *path,const char *text) {FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void saved(const char *path) {pxa_bytes_t b=file_bytes(path);assert(b.size==12&&!memcmp(b.data,"private-save",12));free((void *)b.data);}
static void copy_stream_fixture(const char *source) {
    assert(!mkdir("stream",0700)&&!mkdir("stream/artifacts",0700));
    const char *files[]={"manifest.pxm","signature.pxs","artifacts/main.wasm"};
    for(unsigned i=0;i<3;++i) {
        char path[560];snprintf(path,sizeof(path),"%s/%s",source,files[i]);pxa_bytes_t bytes=file_bytes(path);
        snprintf(path,sizeof(path),"stream/%s",files[i]);FILE *f=fopen(path,"wb");assert(f);
        assert(fwrite(bytes.data,1,bytes.size,f)==bytes.size&&!fclose(f));free((void *)bytes.data);
    }
    assert(realpath("stream",adopted));
}
int main(int argc,char **argv) {
    assert(argc==2);assert(realpath(argv[1],test_mount));assert(!chdir(argv[1]));
    pxa_bytes_t der=file_bytes("publisher.der");pxa_openssl_publisher_key_t key={der.data,der.size};
    uint8_t publisher[32];SHA256(der.data,der.size,publisher);
    void *reg=calloc(1,pxa_volume_registry_workspace_size(1));assert(reg);
    assert(!pxa_volume_registry_init(reg,pxa_volume_registry_workspace_size(1),1,100,&registry));
    volume.id.bytes[0]=1;volume.device_id.bytes[0]=1;volume.state=PXA_VOLUME_READY;
    volume.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ|PXA_VOLUME_INSTALLABLE|PXA_VOLUME_PRIVATE_NATIVE;
    volume.total_bytes=volume.free_bytes=16000000;volume.space_known=3;volume.allocation_unit=4096;
    assert(!pxa_volume_register(registry,&volume,&epoch));volume.mount_epoch=epoch;
    for(unsigned scenario=0;scenario<4;++scenario) {
        unsigned builtin=scenario%2,direct=scenario>=2;
        char state[512],state_name[32],root[160],save[640],source1[512],source2[512];
        snprintf(state_name,sizeof(state_name),"state%u",scenario);assert(!mkdir(state_name,0700));
        assert(realpath(state_name,state));store.legacy_ready=1;
        assert(realpath("source1",source1)&&realpath("source2",source2));
        pxa_posix_installer_config_t *config=&store.installer_config;memset(config,0,sizeof(*config));
        config->struct_size=sizeof(*config);config->storage_root=state;
        config->flags=PXA_POSIX_INSTALLER_FLAG_COMPOSITE_IDENTITY|PXA_POSIX_INSTALLER_FLAG_SKIP_LOCK;
        config->trust=(pxa_openssl_trust_t){sizeof(config->trust),&key,1};pxa_package_limits_init(&config->limits);
        size_t size=pxa_posix_installer_workspace_size(config);void *memory=calloc(1,size);assert(memory);
        assert(!pxa_posix_installer_init(memory,size,config,&store.installer));
        pxa_package_manifest_t *manifest=NULL;pxa_posix_installer_result_t result={.struct_size=sizeof(result),
            .root=root,.root_capacity=sizeof(root),.manifest=&manifest};
        assert(!read_source_into_store(source1,&result,1));
        pxa_posix_install_disposition_t disposition;
        /* Provision an owner/private directory, as the old runtime does. */
        assert(!pxa_posix_installer_install(store.installer,source1,&result,&disposition));
        entry=(pxa_esp_package_entry_t){.installed=1,.built_in=builtin};strcpy(entry.root,root);
        strcpy(entry.id,"volume-test");memcpy(entry.publisher_key_id,publisher,32);
        for(unsigned i=0;i<32;++i)snprintf(entry.identity+2*i,3,"%02x",publisher[i]);
        strcat(entry.identity,":volume-test");
        snprintf(save,sizeof(save),"%s/data",state);assert(!mkdir(save,0700));
        const char *leaf=strrchr(root,'/');assert(leaf);
        strcat(save,leaf);assert(!mkdir(save,0700));strcat(save,"/save");text_file(save,"private-save");
        if(builtin) {
            char owner[640];snprintf(owner,sizeof(owner),"%s/owners%s",state,leaf);assert(!unlink(owner));
            assert(!pxa_posix_fs_remove_tree(root));strcpy(entry.root,source1);
        }
        if(direct) {
            unsigned before=layout_writes;
            assert(!read_source_into_store(source2,&result,1));
            assert(install_legacy_on_volume_locked(source2,epoch+1,&entry,&result)==PXA_STATUS_BAD_STATE);
            assert(layout_writes==before);saved(save);
            const char *transfer=source2;
            if(!builtin) {copy_stream_fixture(source2);transfer=adopted;}
            assert(!read_source_into_store(transfer,&result,1));
            assert(!pxa_volume_update(registry,epoch,PXA_VOLUME_READY,16000000,100000,3));
            assert(install_legacy_on_volume_locked(transfer,epoch,&entry,&result)==PXA_STATUS_QUOTA_EXCEEDED);
            assert(!pxa_volume_get(registry,epoch,&volume)&&!volume.reserved_bytes);
            assert(layout_writes==before);saved(save);
            assert(!pxa_volume_update(registry,epoch,PXA_VOLUME_READY,16000000,16000000,3));
            assert(!read_source_into_store(transfer,&result,1));
            assert(!install_legacy_on_volume_locked(transfer,epoch,&entry,&result));
            assert(!entry.built_in);saved(save);
            if(!builtin)assert(!is_directory(adopted));
            assert(!pxa_volume_get(registry,epoch,&volume)&&!volume.reserved_bytes);
            assert(!read_source_into_store(source2,&result,1));before=layout_writes;
            assert(!install_legacy_on_volume_locked(source2,0,&entry,&result)); /* Same signed version. */
            assert(layout_writes==before);saved(save);
            assert(!read_source_into_store(source1,&result,1));
            assert(install_legacy_on_volume_locked(source1,epoch,&entry,&result)==PXA_STATUS_DENIED);
            assert(!read_source_into_store(source2,&result,1));
            copy_stream_fixture(source2);
            FILE *f=fopen("stream/artifacts/main.wasm","r+b");assert(f);assert(fputc(1,f)!=EOF&&!fclose(f));
            assert(!read_source_into_store(adopted,&result,1));
            assert(install_legacy_on_volume_locked(adopted,epoch,&entry,&result)!=PXA_STATUS_OK);
            assert(!pxa_volume_get(registry,epoch,&volume)&&!volume.reserved_bytes);saved(save);
            assert(!pxa_posix_fs_remove_tree("stream"));
            pxa_posix_installer_deinit(store.installer);free(memory);continue;
        }
        pxa_esp_install_approval_t approval={.legacy_update=1,.legacy_builtin=builtin};
        strcpy(approval.legacy_root,entry.root);strcpy(approval.record.app_id,"volume-test");
        memcpy(approval.record.publisher_root,publisher,32);approval.record.app_volume=volume.id;
        char manifest_path[560];snprintf(manifest_path,sizeof(manifest_path),"%s/manifest.pxm",source2);
        pxa_bytes_t incoming=file_bytes(manifest_path);SHA256(incoming.data,incoming.size,approval.manifest_sha256);
        assert(!snapshot_legacy_update(&approval,incoming));
        pxa_install_requirements_t required={.app_bytes=1000000,.volume_headroom_bytes=131072};
        assert(!pxa_install_plan_evaluate(registry,&required,epoch,0,&approval.plan));
        assert(!pxa_install_plan_reserve(registry,&approval.plan));
        assert(commit_legacy_update(source1,&approval,&result)==PXA_STATUS_DENIED);saved(save);
        entry.ambiguous=1;assert(commit_legacy_update(source2,&approval,&result)==PXA_STATUS_BAD_STATE);entry.ambiguous=0;
        uint8_t old=approval.legacy_manifest_sha256[0];approval.legacy_manifest_sha256[0]^=1;
        assert(commit_legacy_update(source2,&approval,&result)==PXA_STATUS_BAD_STATE);approval.legacy_manifest_sha256[0]=old;
        approval.record.publisher_root[0]^=1;assert(commit_legacy_update(source2,&approval,&result)==PXA_STATUS_DENIED);
        approval.record.publisher_root[0]^=1;saved(save);
        const char *transfer=source2;
        if(!builtin) {
            copy_stream_fixture(source2);transfer=adopted;
            FILE *f=fopen("stream/artifacts/main.wasm","r+b");assert(f);assert(fputc(1,f)!=EOF&&!fclose(f));
            assert(commit_legacy_update(transfer,&approval,&result)!=PXA_STATUS_OK);saved(save);
            /* Restaging is required after a failed ownership transfer. */
            assert(!pxa_posix_fs_remove_tree("stream") || errno==ENOENT);
            copy_stream_fixture(source2);
        }
        assert(!commit_legacy_update(transfer,&approval,&result));assert(!entry.built_in);saved(save);
        if(!builtin)assert(!is_directory(adopted));
        assert(!read_source_into_store(entry.root,&result,1)&&manifest->release_sequence==2);
        assert(commit_legacy_update(source2,&approval,&result)==PXA_STATUS_BAD_STATE); /* stale approval */
        assert(!pxa_install_plan_release(registry,&approval.plan));
        strcpy(approval.legacy_root,entry.root);approval.legacy_builtin=0;
        assert(!snapshot_legacy_update(&approval,incoming));
        assert(commit_legacy_update(source2,&approval,&result)!=PXA_STATUS_OK); /* released lease */
        snprintf(manifest_path,sizeof(manifest_path),"%s/manifest.pxm",source1);pxa_bytes_t older=file_bytes(manifest_path);
        assert(snapshot_legacy_update(&approval,older)==PXA_STATUS_DENIED);saved(save);
        free((void *)older.data);free((void *)incoming.data);
        pxa_posix_installer_deinit(store.installer);free(memory);
    }
    assert(layout_writes>=2);free((void *)der.data);free(reg);
    puts("Legacy updates: signed snapshots, private-data retention, built-in overlay, direct volume/stream/same-version updates, wrong volume, quota, tampered payload, stale approval, downgrade and released lease passed");
}
