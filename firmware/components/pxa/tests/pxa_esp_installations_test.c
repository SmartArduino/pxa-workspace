#define _GNU_SOURCE
#include "pxa_esp_installations.h"
#include "pxa_esp_installations_internal.h"
#include "pxa_esp_resource_transfer.h"
#include "pxa_esp_package_trust.h"
#include "pxa_esp_record_seal.h"
#include "pxa_esp_publisher_directory.h"
#include "pxa_esp_volumes.h"
#include "esp_vfs_fat.h"
#include "pxa/openssl/pxa_openssl.h"
#include "record_test_auth.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pxa_openssl_publisher_key_t publisher;
static record_test_auth_context_t device_key={{1},1};
static uint64_t selected_epoch;
static unsigned random_state=1,prepare_calls;
static int fail_external,fail_prepare,fail_close;
struct pxa_esp_app_storage {char root[PXA_POSIX_APP_PATH_BYTES];uint64_t data_generation;};
void *heap_caps_malloc(size_t n,unsigned caps) {if(fail_external && (caps&MALLOC_CAP_SPIRAM))return NULL;return malloc(n);}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {if(fail_external && (caps&MALLOC_CAP_SPIRAM))return NULL;return calloc(n,bytes);}
void *heap_caps_aligned_calloc(size_t alignment,size_t n,size_t bytes,unsigned caps) {
    if(fail_external && (caps&MALLOC_CAP_SPIRAM))return NULL;
    void *out=NULL;if(posix_memalign(&out,alignment,n*bytes)!=0)return NULL;
    memset(out,0,n*bytes);return out;
}
void heap_caps_free(void *p) {free(p);}
uint32_t esp_random(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
void vTaskDelay(TickType_t ticks) {(void)ticks;}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
esp_err_t esp_littlefs_info(const char *label,size_t *total,size_t *used) {
    (void)label;(void)total;(void)used;assert(!"external installation must not query primary Flash");return ESP_FAIL;
}
esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *free_bytes) {(void)path;*total=16000000;*free_bytes=15000000;return ESP_OK;}
static pxa_status_t capacity(void *ctx,const char *path,uint64_t *total,uint64_t *free_bytes,uint8_t *known) {
    (void)path;*total=16000000;*free_bytes=ctx?15000000:0;*known=3;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_installer_profile(pxa_posix_installer_config_t *config,pxa_esp_mbedtls_trust_t *trust) {
    (void)trust;memset(config,0,sizeof(*config));config->struct_size=sizeof(*config);
    config->trust=(pxa_openssl_trust_t){sizeof(config->trust),&publisher,1};
    config->verify=pxa_openssl_p256_verify;config->flags=PXA_POSIX_INSTALLER_FLAG_SKIP_LOCK;
    pxa_package_limits_init(&config->limits);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,uint8_t tag[32]) {
    return record_test_tag(&device_key,purpose,binding,body,tag);
}
/* The private-image implementation has its own actual-LittleFS fault suite.
 * Here inject its admission/teardown outcomes into real signed transactions. */
pxa_status_t pxa_esp_app_storage_create(const pxa_posix_app_home_t *home,uint64_t epoch,uint64_t bytes,
    const pxa_install_plan_t *plan,pxa_esp_app_storage_t **out) {
    assert(home && epoch==selected_epoch && bytes>=131072);*out=NULL;
    if(plan)assert(plan->reserved && plan->app_mount_epoch==epoch);
    *out=calloc(1,sizeof(**out));assert(*out);
    assert(!pxa_posix_app_home_path(home,PXA_POSIX_APP_HOME_PRIVATE,(*out)->root,sizeof((*out)->root)));
    (*out)->data_generation=home->record.data_generation;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_app_storage_root(pxa_esp_app_storage_t *storage,const char **root) {
    assert(storage);++prepare_calls;*root=NULL;
    if(fail_prepare)return PXA_STATUS_QUOTA_EXCEEDED;
    if(storage->data_generation) {
        char *separator=strrchr(storage->root,'/');*separator=0;
        assert(!mkdir(storage->root,0700) || errno==EEXIST);*separator='/';
    }
    assert(!mkdir(storage->root,0700) || errno==EEXIST);
    *root=storage->root;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_app_storage_close(pxa_esp_app_storage_t **owner) {
    assert(owner);if(!*owner)return PXA_STATUS_OK;
    if(fail_close)return PXA_STATUS_IO_ERROR;
    free(*owner);*owner=NULL;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_app_storage_permissions(pxa_esp_app_storage_t *storage,pxa_permission_store_t *provider) {
    (void)storage;(void)provider;return PXA_STATUS_UNSUPPORTED;
}
pxa_status_t pxa_esp_app_storage_bind_media(pxa_esp_app_storage_t *storage,uint64_t epoch) {
    assert(storage && epoch);return PXA_STATUS_OK;
}
static int count_home(void *ctx,const pxa_posix_app_home_t *home) {
    unsigned *count=ctx;assert(!strcmp(home->record.app_id,"volume-test"));++*count;return 1;
}
static void path(char *out,size_t size,const char *root,const char *child) {
    assert(snprintf(out,size,"%s/%s",root,child)<(int)size);
}
int main(int argc,char **argv) {
    assert(argc==2);char flash[512],sd[512],source1[512],source2[512],package[512],locator[130];uint8_t der[256],encoded[8192],digest[32];
    path(flash,sizeof(flash),argv[1],"flash");path(sd,sizeof(sd),argv[1],"sd");assert(!mkdir(flash,0700) && !mkdir(sd,0700));
    path(source1,sizeof(source1),argv[1],"source1");path(source2,sizeof(source2),argv[1],"source2");
    path(package,sizeof(package),argv[1],"publisher.der");int file=open(package,O_RDONLY);assert(file>=0);
    ssize_t count=read(file,der,sizeof(der));assert(count>0 && !close(file));publisher=(pxa_openssl_publisher_key_t){der,(size_t)count};
    pxa_esp_storage_volume_config_t volume={0};uint64_t flash_epoch;pxa_volume_info_t info;
    volume.struct_size=sizeof(volume);volume.mount_path=flash;volume.name="full flash";volume.device_identity="flash";volume.device_identity_bytes=5;
    volume.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE|PXA_VOLUME_RANDOM_READ;
    volume.allocation_unit=4096;volume.max_file_bytes=INT32_MAX;volume.query_space=capacity;volume.link_free=1;
    assert(!pxa_esp_storage_register_volume(&volume,&flash_epoch));
    volume.mount_path=sd;volume.name="SD";volume.device_identity="card";volume.device_identity_bytes=4;volume.context=&volume;
    volume.allocation_unit=32768;volume.capabilities|=PXA_VOLUME_REMOVABLE|PXA_VOLUME_CASE_INSENSITIVE;
    assert(!pxa_esp_storage_register_volume(&volume,&selected_epoch));
    pxa_esp_installations_t *owner=NULL;fail_external=1;
    assert(pxa_esp_installations_open(selected_epoch,&owner)==PXA_STATUS_RESOURCE_LIMIT && !owner);
    fail_external=0;assert(!pxa_esp_installations_open(selected_epoch,&owner));
    pxa_package_limits_t limits;pxa_package_limits_init(&limits);
    size_t bytes=pxa_package_manifest_workspace_size(&limits);void *workspace=malloc(bytes);assert(workspace);
    pxa_package_manifest_t *manifest=NULL;pxa_posix_installer_result_t result={0};
    result.struct_size=sizeof(result);result.manifest_workspace=workspace;result.manifest_workspace_size=bytes;
    result.encoded=encoded;result.encoded_capacity=sizeof(encoded);result.manifest=&manifest;result.root=package;result.root_capacity=sizeof(package);
    assert(!pxa_esp_installations_inspect(owner,source1,&result) && manifest);
    pxa_install_record_t approved={0};pxa_posix_app_home_t home;
    approved.flags=PXA_INSTALL_RECORD_ENABLED;approved.install_id[0]=1;strcpy(approved.app_id,"volume-test");
    approved.data_generation=1;
    assert(!pxa_openssl_sha256(der,(size_t)count,approved.publisher_root));
    assert(!pxa_esp_storage_volume(selected_epoch,&info));approved.app_volume=info.id;
    assert(!pxa_esp_installations_locator(approved.publisher_root,approved.app_id,locator,sizeof(locator)));
    assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,digest));
    pxa_install_plan_t plan;
    assert(!pxa_esp_installations_plan(owner,manifest,0,1,&plan) && plan.app_mount_epoch==selected_epoch);
    pxa_install_plan_t prepared,existing;
    assert(!pxa_esp_installations_plan_prepared(owner,manifest,1,&prepared));
    assert(!pxa_esp_installations_plan_prepared(owner,manifest,0,&existing));
    assert(prepared.app_mount_epoch==selected_epoch && prepared.volume_count==1);
    assert(prepared.volumes[0].bytes<plan.volumes[0].bytes);
    assert(prepared.volumes[0].bytes>=existing.volumes[0].bytes+pxa_esp_installations_private_initial_bytes());
    assert(!pxa_esp_storage_reserve_plan(&plan));
    assert(pxa_esp_installations_install(owner,source2,locator,&approved,0,digest,&plan,NULL,NULL,&result,&home)==PXA_STATUS_DENIED);
    assert(!prepare_calls); /* Same identity, different signed permissions/release. */
    assert(!pxa_esp_installations_install(owner,source1,locator,&approved,0,digest,&plan,NULL,NULL,&result,&home));
    assert(home.record.generation==1 && !strncmp(home.home,sd,strlen(sd)) && prepare_calls==1);
    assert(!pxa_esp_storage_release_plan(&plan));
    char namespace_root[768];path(namespace_root,sizeof(namespace_root),sd,"PXA");
    assert(!pxa_esp_publisher_bind(namespace_root,&info.id,"apps",approved.publisher_root,"lucinhu"));
    assert(!pxa_esp_installations_lookup(owner,locator,&home));
    assert(strstr(home.home,"/apps/lucinhu/volume-test") && !strcmp(home.locator,locator));
    unsigned discovered=0;size_t invalid;
    assert(!pxa_esp_installations_visit(owner,count_home,&discovered,&invalid) && discovered==1 && !invalid);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && prepare_calls==1);
    assert(!pxa_esp_installations_inspect(owner,source2,&result));
    assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,digest));
    assert(!pxa_esp_installations_plan(owner,manifest,0,0,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    approved=home.record;fail_prepare=1;
    assert(pxa_esp_installations_install(owner,source2,locator,&approved,1,digest,&plan,NULL,NULL,&result,&home)==PXA_STATUS_QUOTA_EXCEEDED);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home));
    assert(home.record.generation==1 && !memcmp(manifest->version.data,"1.0.0",5));
    fail_prepare=0;fail_close=1;
    assert(pxa_esp_installations_install(owner,source2,locator,&approved,1,digest,&plan,NULL,NULL,&result,&home)==PXA_STATUS_IO_ERROR);
    assert(pxa_esp_installations_close(&owner)==PXA_STATUS_IO_ERROR && owner);
    fail_close=0;assert(!pxa_esp_installations_close(&owner) && !owner);
    assert(!pxa_esp_installations_open(selected_epoch,&owner));
    assert(!pxa_esp_installations_install(owner,source2,locator,&approved,1,digest,&plan,NULL,NULL,&result,&home));
    assert(home.record.generation==2 && !memcmp(manifest->version.data,"2.0.0",5));
    assert(!pxa_esp_storage_release_plan(&plan));
    char private_root[PXA_POSIX_APP_PATH_BYTES],saved_file[768];
    assert(!pxa_posix_app_home_path(&home,PXA_POSIX_APP_HOME_PRIVATE,private_root,sizeof(private_root)));
    path(saved_file,sizeof(saved_file),private_root,"save.bin");file=open(saved_file,O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0);
    assert(write(file,"old-save",8)==8 && !close(file));
    fail_prepare=1;
    assert(pxa_esp_installations_reset_data(owner,locator,2,1,&home)==PXA_STATUS_QUOTA_EXCEEDED);
    struct stat st;assert(!stat(saved_file,&st) && st.st_size==8);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && home.record.data_generation==1);
    fail_prepare=0;
    assert(!pxa_esp_installations_reset_data(owner,locator,2,1,&home));
    assert(home.record.generation==3 && home.record.data_generation==2 && stat(saved_file,&st)<0 && errno==ENOENT);
    assert(!pxa_posix_app_home_path(&home,PXA_POSIX_APP_HOME_PRIVATE,private_root,sizeof(private_root)) && !stat(private_root,&st));
    assert(pxa_esp_installations_reset_data(owner,locator,3,1,&home)==PXA_STATUS_BAD_STATE);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home));
    path(saved_file,sizeof(saved_file),private_root,"new-save.bin");
    file=open(saved_file,O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0 && write(file,"keep",4)==4 && !close(file));
    assert(!pxa_esp_installations_remove(owner,locator,3,1,&home));
    assert(home.record.generation==4 && (home.record.flags&PXA_INSTALL_RECORD_ARCHIVED));
    assert(!stat(saved_file,&st) && st.st_size==4);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && home.record.generation==4);
    approved=home.record;approved.flags&=~PXA_INSTALL_RECORD_ARCHIVED;
    assert(!pxa_esp_installations_inspect(owner,source2,&result));
    assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,digest));
    assert(!pxa_esp_installations_plan(owner,manifest,0,0,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    assert(!pxa_esp_installations_install(owner,source2,locator,&approved,4,digest,&plan,NULL,NULL,&result,&home));
    assert(!pxa_esp_storage_release_plan(&plan) && home.record.generation==5 && home.record.data_generation==2);
    assert(!memcmp(home.record.install_id,approved.install_id,16) && !stat(saved_file,&st));
    assert(!pxa_esp_installations_remove(owner,locator,5,0,&home));
    assert(stat(home.home,&st)<0 && errno==ENOENT);
    discovered=0;assert(!pxa_esp_installations_visit(owner,count_home,&discovered,&invalid) && !discovered && !invalid);
    /* A signed media generation lives on a third volume. Code/private state
     * stays on SD, and no per-app Flash record is needed to join the two. */
    char usb[512],resources[512],with_resources[512];uint64_t usb_epoch;
    path(usb,sizeof(usb),argv[1],"usb");assert(!mkdir(usb,0700));
    volume.mount_path=usb;volume.name="USB";volume.device_identity="usb";volume.device_identity_bytes=3;
    assert(!pxa_esp_storage_register_volume(&volume,&usb_epoch));
    pxa_esp_installations_t *media_owner=NULL;assert(!pxa_esp_installations_open(usb_epoch,&media_owner));
    path(resources,sizeof(resources),argv[1],"resources");path(with_resources,sizeof(with_resources),argv[1],"with-resources");
    assert(!pxa_esp_installations_inspect(media_owner,resources,&result) && manifest->package_kind==PXA_PACKAGE_KIND_RESOURCES);
    approved.flags=PXA_INSTALL_RECORD_ENABLED|PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
    approved.install_id[0]=2;approved.data_generation=1;
    assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,approved.media_set_sha256));
    assert(!pxa_esp_storage_volume(usb_epoch,&info));approved.media_volume=info.id;
    /* Each volume reserves names independently: USB owns the plain handle,
     * while SD already has a different publisher using that same handle. */
    char source_media[256],target_media[256],media_payload[768];struct stat source_payload;
    path(namespace_root,sizeof(namespace_root),usb,"PXA");
    assert(!pxa_esp_publisher_bind(namespace_root,&info.id,"media",approved.publisher_root,"lucinhu"));
    assert(!pxa_esp_installations_media_path(&approved,source_media,sizeof(source_media)));
    assert(!strncmp(source_media,"PXA/media/lucinhu/volume-test/",sizeof("PXA/media/lucinhu/volume-test/")-1));
    assert(!pxa_esp_storage_volume(selected_epoch,&info));
    uint8_t other_publisher[32];memcpy(other_publisher,approved.publisher_root,32);other_publisher[0]^=1;
    path(namespace_root,sizeof(namespace_root),sd,"PXA");
    assert(!pxa_esp_publisher_bind(namespace_root,&info.id,"media",other_publisher,"lucinhu"));
    assert(!pxa_esp_publisher_bind(namespace_root,&info.id,"media",approved.publisher_root,"lucinhu"));
    pxa_install_record_t target_record=approved;target_record.media_volume=info.id;
    assert(!pxa_esp_installations_media_path(&target_record,target_media,sizeof(target_media)));
    assert(!strncmp(target_media,"PXA/media/lucinhu-",sizeof("PXA/media/lucinhu-")-1) && strcmp(source_media,target_media));
    assert(!pxa_esp_installations_plan(media_owner,manifest,0,0,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    char small_root[1];result.root=small_root;result.root_capacity=sizeof(small_root);
    assert(pxa_esp_installations_install_resources(media_owner,&approved,resources,&plan,&result)==PXA_STATUS_RESOURCE_LIMIT);
    result.root=package;result.root_capacity=sizeof(package);
    assert(!pxa_esp_installations_install_resources(media_owner,&approved,resources,&plan,&result));
    assert(strstr(package,"/usb/PXA/media/") && strstr(package,"/package"));
    assert(!pxa_esp_installations_install_resources(media_owner,&approved,resources,&plan,&result));
    assert(!pxa_esp_storage_release_plan(&plan));
    assert(!pxa_esp_installations_verify_resources(media_owner,&approved,&result));
    assert(snprintf(media_payload,sizeof(media_payload),"%s/%s/pak0.pak",usb,source_media)<(int)sizeof(media_payload));
    assert(!stat(media_payload,&source_payload));
    pxa_install_record_t wrong=approved;wrong.media_set_sha256[0]^=1;
    assert(pxa_esp_installations_verify_resources(media_owner,&wrong,&result)!=PXA_STATUS_OK);
    assert(!pxa_esp_installations_inspect(owner,with_resources,&result) && manifest->resource_manifest_sha256);
    assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,digest));
    assert(!pxa_esp_installations_plan(owner,manifest,0,1,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    assert(!pxa_esp_installations_install(owner,with_resources,locator,&approved,0,digest,&plan,NULL,NULL,&result,&home));
    assert(!pxa_esp_storage_release_plan(&plan) && home.record.generation==1);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home));
    pxa_esp_app_storage_t *active_storage=NULL;
    assert(!pxa_esp_installations_storage(owner,&home,&active_storage) && !pxa_esp_app_storage_close(&active_storage));
    /* Activation loads signed resource metadata without reading payloads.
     * That lightweight load must never authorize later full verification. */
    assert(!pxa_esp_installations_close(&owner->media_owner));
    file=open(media_payload,O_RDWR);assert(file>=0);char saved_resource_byte=0;
    assert(read(file,&saved_resource_byte,1)==1 && lseek(file,0,SEEK_SET)==0);
    char damaged_resource_byte=saved_resource_byte^1;
    assert(write(file,&damaged_resource_byte,1)==1 && !close(file));
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home));
    assert(owner->media_owner->resource_loaded && !owner->media_owner->resource_verified);
    assert(pxa_esp_installations_verify_resources(media_owner,&home.record,&result)==PXA_STATUS_DENIED);
    assert(pxa_esp_installations_verify_media(owner,&home.record)==PXA_STATUS_DENIED);
    file=open(media_payload,O_WRONLY);assert(file>=0 && write(file,&saved_resource_byte,1)==1 && !close(file));
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home));
    /* Resource relocation copies to a third volume, changes only the exact
     * approved local record, and preserves private bytes/code/permissions. */
    pxa_install_record_t before_move=home.record,move_approval;
    uint64_t source_media_epoch=0;
    assert(!pxa_posix_app_home_path(&home,PXA_POSIX_APP_HOME_PRIVATE,private_root,sizeof(private_root)));
    path(saved_file,sizeof(saved_file),private_root,"relocate-save.bin");
    file=open(saved_file,O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0 && write(file,"retained",8)==8 && !close(file));
    /* Full Flash has no capacity: no source or target path may be created. */
    assert(pxa_esp_resource_transfer_plan(selected_epoch,flash_epoch,locator,1,&move_approval,&source_media_epoch,&plan)==PXA_STATUS_QUOTA_EXCEEDED);
    assert(!pxa_esp_resource_transfer_plan(selected_epoch,selected_epoch,locator,1,&move_approval,&source_media_epoch,&plan));
    assert(source_media_epoch==usb_epoch && !plan.reserved && plan.volume_count==1);
    assert(!memcmp(&move_approval,&before_move,sizeof(before_move)));
    pxa_install_record_t stale=move_approval;stale.install_id[0]^=1;
    assert(pxa_esp_resource_transfer_begin_approved(selected_epoch,usb_epoch,selected_epoch,locator,&stale,&home)==PXA_STATUS_BAD_STATE);
    assert(pxa_esp_resource_transfer_begin_approved(selected_epoch,usb_epoch+100,selected_epoch,locator,&move_approval,&home)==PXA_STATUS_BAD_STATE);
    fail_external=1;
    assert(pxa_esp_resource_transfer_begin_approved(selected_epoch,usb_epoch,selected_epoch,locator,&move_approval,&home)==PXA_STATUS_RESOURCE_LIMIT);
    fail_external=0;
    unsigned prepared_before=prepare_calls;
    assert(!pxa_esp_resource_transfer_begin_approved(selected_epoch,usb_epoch,selected_epoch,locator,&move_approval,&home));
    assert(home.record.generation==2 && home.record.data_generation==1 && prepare_calls==prepared_before);
    assert(!memcmp(home.record.install_id,before_move.install_id,16) && !memcmp(home.record.manifest_sha256,before_move.manifest_sha256,32));
    assert(!memcmp(home.record.media_set_sha256,before_move.media_set_sha256,32) && !stat(saved_file,&st) && st.st_size==8);
    assert(!pxa_esp_storage_volume(selected_epoch,&info) && !memcmp(home.record.media_volume.bytes,info.id.bytes,16));
    assert(snprintf(media_payload,sizeof(media_payload),"%s/%s/pak0.pak",sd,target_media)<(int)sizeof(media_payload));
    assert(!stat(media_payload,&st) && st.st_size==source_payload.st_size);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && home.record.generation==2);
    assert(pxa_esp_resource_transfer_begin_approved(selected_epoch,usb_epoch,selected_epoch,locator,&move_approval,&home)==PXA_STATUS_BAD_STATE);
    /* Returning reuses the previously verified immutable generation; neither
     * a fresh code copy nor a private-image reservation is required. */
    assert(!pxa_esp_resource_transfer_plan(selected_epoch,usb_epoch,locator,2,&move_approval,&source_media_epoch,&plan));
    assert(source_media_epoch==selected_epoch && plan.volume_count==1 &&
        plan.volumes[0].bytes<UINT64_C(128)*1024);
    assert(!pxa_esp_resource_transfer_begin_approved(selected_epoch,source_media_epoch,usb_epoch,locator,&move_approval,&home));
    assert(home.record.generation==3 && home.record.data_generation==1 && !stat(saved_file,&st));
    assert(snprintf(media_payload,sizeof(media_payload),"%s/%s/pak0.pak",usb,source_media)<(int)sizeof(media_payload));
    assert(!stat(media_payload,&st) && st.st_ino==source_payload.st_ino && st.st_mtime==source_payload.st_mtime);
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && home.record.generation==3);
    /* A present but damaged destination never becomes a successful reuse. */
    char media_relative[256],damaged_file[768];
    target_record=home.record;target_record.media_volume=info.id;
    assert(!pxa_esp_installations_media_path(&target_record,media_relative,sizeof(media_relative)));
    assert(!strcmp(media_relative,target_media));
    assert(snprintf(damaged_file,sizeof(damaged_file),"%s/%s/pak0.pak",sd,media_relative)<(int)sizeof(damaged_file));
    file=open(damaged_file,O_RDWR);assert(file>=0);char initial=0;
    assert(read(file,&initial,1)==1 && lseek(file,0,SEEK_SET)==0 && write(file,"?",1)==1 && !fsync(file) && !close(file));
    assert(!pxa_esp_resource_transfer_plan(selected_epoch,selected_epoch,locator,3,&move_approval,&source_media_epoch,&plan));
    assert(pxa_esp_resource_transfer_begin_approved(selected_epoch,source_media_epoch,selected_epoch,locator,&move_approval,&home)!=PXA_STATUS_OK);
    assert(!pxa_esp_installations_lookup(owner,locator,&home) && home.record.generation==3);
    assert(!memcmp(home.record.media_volume.bytes,move_approval.media_volume.bytes,16) && !stat(saved_file,&st));
    file=open(damaged_file,O_WRONLY);assert(file>=0 && write(file,&initial,1)==1 && !fsync(file) && !close(file));
    /* Native SDK maintenance cannot skip resource readiness authentication. */
    pxa_posix_app_store_config_t unavailable={0};pxa_posix_app_home_t rejected;
    assert(pxa_posix_app_store_rebind_media(&unavailable,locator,&home.record,&info.id,&rejected)==PXA_STATUS_UNSUPPORTED);
    assert(!pxa_esp_resource_transfer_plan(selected_epoch,selected_epoch,locator,3,&move_approval,&source_media_epoch,&plan));
    assert(!pxa_esp_installations_close(&media_owner) && !pxa_esp_installations_close(&owner));
    assert(!pxa_esp_storage_drain_volume(usb_epoch) && !pxa_esp_storage_unregister_volume(usb_epoch));
    assert(!pxa_esp_storage_register_volume(&volume,&usb_epoch) && usb_epoch!=source_media_epoch);
    assert(pxa_esp_resource_transfer_begin_approved(selected_epoch,source_media_epoch,selected_epoch,locator,&move_approval,&home)==PXA_STATUS_BAD_STATE);
    assert(!pxa_esp_installations_open(selected_epoch,&owner));
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && home.record.generation==3);
    /* Legacy conversion binds mutable manual media without pretending it is
     * a publisher-signed bundle. Modern code updates keep that same binding. */
    approved=home.record;approved.flags&=~PXA_INSTALL_RECORD_MANAGED_MEDIA;
    memset(approved.media_set_sha256,0,sizeof(approved.media_set_sha256));
    char manual_code[512],manual_root[768],hidden_manual[768];
    assert(!pxa_esp_installations_media_path(&approved,media_relative,sizeof(media_relative)));
    assert(snprintf(manual_root,sizeof(manual_root),"%s/%s",usb,media_relative)<(int)sizeof(manual_root));
    assert(snprintf(hidden_manual,sizeof(hidden_manual),"%s.hidden",manual_root)<(int)sizeof(hidden_manual));
    for(unsigned sequence=4;sequence<=5;++sequence) {
        char name[32];snprintf(name,sizeof(name),"manual-update%u",sequence);path(manual_code,sizeof(manual_code),argv[1],name);
        assert(!pxa_esp_installations_inspect(owner,manual_code,&result) && manifest->format_minor>=8 && !manifest->resource_manifest_sha256);
        assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,digest));
        assert(!pxa_esp_installations_plan(owner,manifest,0,0,&plan) && !pxa_esp_storage_reserve_plan(&plan));
        assert(!pxa_esp_installations_install(owner,manual_code,locator,&approved,sequence-1,digest,&plan,NULL,NULL,&result,&home));
        assert(!pxa_esp_storage_release_plan(&plan) && home.record.generation==sequence && home.record.data_generation==1);
        assert(home.record.flags&PXA_INSTALL_RECORD_HAS_MEDIA);
        assert(!(home.record.flags&PXA_INSTALL_RECORD_MANAGED_MEDIA) && !memcmp(home.record.media_volume.bytes,approved.media_volume.bytes,16));
        assert(!stat(saved_file,&st) && st.st_size==8);approved=home.record;
    }
    assert(!pxa_esp_installations_open(usb_epoch,&media_owner));
    assert(pxa_esp_installations_verify_resources(media_owner,&approved,&result)==PXA_STATUS_DENIED);
    wrong=approved;wrong.media_set_sha256[0]=1;
    assert(pxa_esp_installations_media_path(&wrong,media_relative,sizeof(media_relative))==PXA_STATUS_DENIED);
    assert(!rename(manual_root,hidden_manual));
    assert(pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home)==PXA_STATUS_UNAVAILABLE);
    assert(stat(manual_root,&st)<0 && errno==ENOENT);
    assert(!symlink(hidden_manual,manual_root));
    assert(pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home)==PXA_STATUS_DENIED);
    assert(!unlink(manual_root) && !rename(hidden_manual,manual_root));
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home) && home.record.generation==5);
    assert(!pxa_esp_storage_drain_volume(usb_epoch));
    assert(pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home)==PXA_STATUS_UNAVAILABLE);
    assert(pxa_esp_storage_unregister_volume(usb_epoch)==PXA_STATUS_BUSY);
    assert(!pxa_esp_installations_close(&media_owner));
    assert(!pxa_esp_installations_close(&owner));
    assert(!pxa_esp_storage_unregister_volume(usb_epoch));
    assert(!pxa_esp_installations_open(selected_epoch,&owner));
    assert(!pxa_esp_storage_drain_volume(selected_epoch));
    assert(pxa_esp_storage_unregister_volume(selected_epoch)==PXA_STATUS_BUSY);
    assert(pxa_esp_installations_visit(owner,count_home,&discovered,&invalid)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_esp_installations_close(&owner) && !pxa_esp_storage_unregister_volume(selected_epoch));
    assert(!pxa_esp_storage_drain_volume(flash_epoch) && !pxa_esp_storage_unregister_volume(flash_epoch));
    free(workspace);puts("ESP signed installations: full Flash / SD, exact approval, local MAC records, rollback and teardown passed");return 0;
}
