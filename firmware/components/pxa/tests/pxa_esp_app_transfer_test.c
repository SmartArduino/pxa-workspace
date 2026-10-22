#define _GNU_SOURCE
#include "pxa_esp_app_transfer.h"
#include "pxa_esp_package_limits.h"
#include "pxa_esp_volumes.h"
#include "pxa_esp_record_seal.h"
#include "pxa_esp_package_trust.h"
#include "pxa_esp_transfer_receipt.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#include "pxa/openssl/pxa_openssl.h"
#include "record_test_auth.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pxa_openssl_publisher_key_t publisher;
static record_test_auth_context_t device_key={{1},1};
static uint64_t free_bytes=UINT64_C(128)*1024*1024;
static int fail_after_rename,fail_target_commit,source_renamed;
static unsigned random_state=1;
static int reject_oversized_manifest_allocation;
static const char *expected_save="a real retained save";
void *heap_caps_malloc(size_t bytes,unsigned caps) {
    (void)caps;assert(!reject_oversized_manifest_allocation || bytes!=PXA_ESP_SIGNED_MANIFEST_MAX_BYTES+1u);
    return malloc(bytes);
}
void *heap_caps_calloc(size_t count,size_t bytes,unsigned caps) {(void)caps;return calloc(count,bytes);}
void *heap_caps_aligned_calloc(size_t alignment,size_t count,size_t bytes,unsigned caps) {
    (void)caps;void *out=NULL;assert(alignment==8);
    if(posix_memalign(&out,alignment,count*bytes)!=0)return NULL;
    memset(out,0,count*bytes);return out;
}
void heap_caps_free(void *p) {free(p);}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
void vTaskDelay(TickType_t ticks) {(void)ticks;}
uint32_t esp_random(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
void esp_fill_random(void *out,size_t bytes) {uint8_t *p=out;for(size_t i=0;i<bytes;++i)p[i]=(uint8_t)esp_random();}
const char *esp_err_to_name(esp_err_t code) {(void)code;return "injected";}
esp_err_t esp_littlefs_info(const char *label,size_t *total,size_t *used) {(void)label;(void)total;(void)used;assert(0);return ESP_FAIL;}
esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *available) {(void)path;*total=free_bytes;*available=free_bytes;return ESP_OK;}
esp_err_t esp_littlefs_format_blockdev(esp_blockdev_handle_t device) {(void)device;assert(0);return ESP_FAIL;}
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config) {(void)config;assert(0);return ESP_FAIL;}
esp_err_t esp_vfs_littlefs_unregister_blockdev(esp_blockdev_handle_t device) {(void)device;assert(0);return ESP_FAIL;}
bool esp_littlefs_blockdev_mounted(esp_blockdev_handle_t device) {(void)device;return false;}
static pxa_status_t capacity(void *context,const char *path,uint64_t *total,uint64_t *available,uint8_t *known) {
    (void)context;(void)path;*total=UINT64_C(256)*1024*1024;*available=free_bytes;*known=3;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_installer_profile(pxa_posix_installer_config_t *config,pxa_esp_mbedtls_trust_t *trust) {
    (void)trust;memset(config,0,sizeof(*config));config->struct_size=sizeof(*config);
    config->trust=(pxa_openssl_trust_t){sizeof(config->trust),&publisher,1};config->verify=pxa_openssl_p256_verify;
    config->flags=PXA_POSIX_INSTALLER_FLAG_SKIP_LOCK;pxa_package_limits_init(&config->limits);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,uint8_t tag[32]) {
    if(source_renamed && fail_target_commit && purpose.size==16 && !memcmp(purpose.data,"install-record/1",16)) {
        fail_target_commit=0;return PXA_STATUS_UNAVAILABLE;
    }
    return record_test_tag(&device_key,purpose,binding,body,tag);
}
pxa_status_t pxa_esp_record_verify(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,const uint8_t tag[32]) {
    uint8_t expected[32];pxa_status_t status=record_test_tag(&device_key,purpose,binding,body,expected);
    return status==PXA_STATUS_OK && memcmp(expected,tag,32)?PXA_STATUS_DENIED:status;
}
static int transfer_renameat(int from,const char *old,int to,const char *name) {
    int result=pxa_esp_renameat(from,old,to,name);
    if(!result && !strcmp(name,"source")) {
        source_renamed=1;
        if(fail_after_rename) {fail_after_rename=0;errno=EIO;return -1;}
    }
    return result;
}
#define pxa_esp_renameat transfer_renameat
#include "../src/package/pxa_esp_app_transfer.c"
#undef pxa_esp_renameat
#undef open
#undef openat
#undef fstat
#undef close
#undef fstatat
#undef mkdirat
#undef renameat
#undef unlinkat
#undef fsync
#undef dup
#undef fdopendir

static void path(char *out,size_t capacity_bytes,const char *root,const char *name) {
    assert(snprintf(out,capacity_bytes,"%s/%s",root,name)<(int)capacity_bytes);
}
static void save(const char *root,const char *name,const void *bytes,size_t count) {
    char filename[768];path(filename,sizeof(filename),root,name);
    int file=open(filename,O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0);
    assert(write(file,bytes,count)==(ssize_t)count && !fsync(file) && !close(file));
}
static void assert_file(const char *root,const char *name,const char *expected) {
    char filename[768],buffer[128]={0};path(filename,sizeof(filename),root,name);int file=open(filename,O_RDONLY);assert(file>=0);
    assert(read(file,buffer,sizeof(buffer))==(ssize_t)strlen(expected) && !strcmp(buffer,expected) && !close(file));
}
static int count_home(void *context,const pxa_posix_app_home_t *home) {(void)home;++*(unsigned *)context;return 1;}
typedef struct {const char *source,*target,*locator;unsigned polls,checkpoints;} progress_test_t;
static void progress_test(void *context,uint8_t phase) {
    progress_test_t *progress=context;
    if(!phase) {++progress->polls;return;}
    assert(phase==1 && ++progress->checkpoints==1);
    char home[768],filename[1024],intents[768];struct stat info;
    assert(snprintf(home,sizeof(home),"%s/PXA/apps/%s",progress->target,progress->locator)<(int)sizeof(home));
    for(unsigned slot=0;slot<2;++slot) {
        assert(snprintf(filename,sizeof(filename),"%s/.transfer.%u.pxm",home,slot)<(int)sizeof(filename));
        assert(!lstat(filename,&info) && S_ISREG(info.st_mode) && info.st_size>328);
    }
    path(intents,sizeof(intents),progress->source,"PXA/apps/.transfers");
    DIR *directory=opendir(intents);assert(directory);unsigned found=0;struct dirent *entry;
    while((entry=readdir(directory))!=NULL) {
        if(strlen(entry->d_name)!=32)continue;
        for(unsigned slot=0;slot<2;++slot) {
            assert(snprintf(filename,sizeof(filename),"%s/%s/.transfer.%u.pxm",intents,entry->d_name,slot)<(int)sizeof(filename));
            assert(!lstat(filename,&info) && S_ISREG(info.st_mode) && info.st_size>328);
        }
        ++found;
    }
    assert(found==1 && !closedir(directory));
}
typedef struct {const char *locator;unsigned calls;int keep_going;uint64_t epoch;pxa_status_t status;uint64_t generation;} scan_test_t;
static int visit_pending(void *context,const char *locator) {
    scan_test_t *scan=context;assert(!strcmp(locator,scan->locator));++scan->calls;
    if(scan->epoch) {
        pxa_posix_app_home_t home;
        scan->status=pxa_esp_app_transfer_resume(scan->epoch,locator,NULL,NULL,&home);
        if(scan->status==PXA_STATUS_OK)scan->generation=home.record.generation;
    }
    return scan->keep_going;
}
static pxa_bytes_t bytes(const char *value) {return (pxa_bytes_t){(const uint8_t *)value,strlen(value)};}
typedef struct {const char *identity;unsigned reads;int invalid_decision;} legacy_policy_test_t;
static pxa_status_t legacy_policy_read(void *context,pxa_bytes_t identity,pxa_bytes_t name,
    pxa_bytes_t scope,pxa_permission_decision_t *decision) {
    legacy_policy_test_t *test=context;++test->reads;
    assert(identity.size==strlen(test->identity) && !memcmp(identity.data,test->identity,identity.size));
    if(test->invalid_decision) {*decision=(pxa_permission_decision_t)99;return PXA_STATUS_OK;}
    if(name.size==14 && !memcmp(name.data,"audio.playback",14) && scope.size==5 && !memcmp(scope.data,"media",5)) {
        *decision=PXA_PERMISSION_ALLOW;return PXA_STATUS_OK;
    }
    return PXA_STATUS_NOT_FOUND;
}
static void verify_legacy_authority_import(uint64_t target_epoch,const char *target_path,
    const char *source_path,const pxa_posix_app_home_t *source,const uint8_t *original_grants) {
    pxa_esp_installations_t *owner=NULL;pxa_esp_app_storage_t *storage=NULL;pxa_posix_app_home_t candidate=*source;
    assert(!pxa_esp_installations_open(target_epoch,&owner));
    candidate.record.app_volume=owner->volume.id;candidate.record.install_id[0]=42;
    assert(snprintf(candidate.home,sizeof(candidate.home),"%s/PXA/apps/%s",target_path,candidate.locator)<(int)sizeof(candidate.home));
    int parent=-1,directory=-1;
    assert(!home_directory(owner,candidate.locator,1,&parent,&directory));
    assert(!pxa_esp_close(parent) && !pxa_esp_close(directory));
    const char *root;
    assert(!pxa_esp_app_storage_create(&candidate,target_epoch,0,NULL,&storage) && !pxa_esp_app_storage_root(storage,&root));
    char identity[130],old_root[768],old_file[1024];strcpy(identity,candidate.locator);identity[64]=':';
    path(old_root,sizeof(old_root),source_path,"legacy-grants");assert(!mkdir(old_root,0700));
    uint8_t grant_bytes[1264],readback[1264];memcpy(grant_bytes,original_grants,sizeof(grant_bytes));
    memcpy(grant_bytes,"PXAGRNT1",8);
    assert(!pxa_openssl_sha256(grant_bytes,sizeof(grant_bytes)-32,grant_bytes+sizeof(grant_bytes)-32));
    save(old_root,identity,grant_bytes,sizeof(grant_bytes));
    int old=pxa_esp_open(old_root,O_RDONLY|O_DIRECTORY);assert(old>=0);
    legacy_policy_test_t test={identity,0,0};
    pxa_permission_store_t provider={.struct_size=sizeof(provider),.context=&test,.load=legacy_policy_read};
    const pxa_package_permission_t declared[]={
        {.name={(const uint8_t *)"audio.playback",14},.scope={(const uint8_t *)"media",5}},
        {.name={(const uint8_t *)"network.http",12},.scope={(const uint8_t *)"",0}}};
    assert(pxa_esp_app_storage_import_legacy_permissions(storage,bytes("wrong:app"),&provider,NULL,0)==PXA_STATUS_DENIED);
    path(old_file,sizeof(old_file),old_root,identity);int file=open(old_file,O_RDWR);assert(file>=0);
    grant_bytes[sizeof(grant_bytes)-1]^=1;
    assert(write(file,grant_bytes,sizeof(grant_bytes))==sizeof(grant_bytes) && !close(file));
    assert(pxa_esp_private_authority_import_legacy(storage,bytes(identity),&provider,declared,2,old,identity)==PXA_STATUS_DENIED && !test.reads);
    grant_bytes[sizeof(grant_bytes)-1]^=1;file=open(old_file,O_WRONLY);assert(file>=0);
    assert(write(file,grant_bytes,sizeof(grant_bytes))==sizeof(grant_bytes) && !close(file));
    test.invalid_decision=1;
    assert(pxa_esp_private_authority_import_legacy(storage,bytes(identity),&provider,declared,2,old,identity)==PXA_STATUS_DENIED && test.reads==1);
    test.invalid_decision=0;
    assert(!pxa_esp_private_authority_import_legacy(storage,bytes(identity),&provider,declared,2,old,identity) && test.reads==3);
    pxa_permission_store_t imported;pxa_permission_decision_t decision=PXA_PERMISSION_DENY;
    assert(!pxa_esp_app_storage_permissions(storage,&imported));
    assert(!imported.load(imported.context,bytes(identity),declared[0].name,declared[0].scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    decision=PXA_PERMISSION_ALLOW;
    assert(imported.load(imported.context,bytes(identity),declared[1].name,declared[1].scope,&decision)==PXA_STATUS_NOT_FOUND && decision==PXA_PERMISSION_DENY);
    char imported_file[768];path(imported_file,sizeof(imported_file),root,".pxa-directory-grants");
    file=open(imported_file,O_RDONLY);assert(file>=0 && read(file,readback,sizeof(readback))==sizeof(readback) && !close(file));
    assert(!memcmp(readback,"PXAGRNT2",8) && !memcmp(readback+8,grant_bytes+8,sizeof(grant_bytes)-40));
    assert(!pxa_esp_app_storage_verify_grants(storage,(pxa_bytes_t){readback,sizeof(readback)-32},readback+sizeof(readback)-32));
    assert(pxa_esp_app_storage_verify_grants(storage,(pxa_bytes_t){grant_bytes,sizeof(grant_bytes)-32},grant_bytes+sizeof(grant_bytes)-32)==PXA_STATUS_DENIED);
    file=open(old_file,O_RDONLY);assert(file>=0 && read(file,readback,sizeof(readback))==sizeof(readback) && !close(file));
    assert(!memcmp(grant_bytes,readback,sizeof(readback)));
    assert(pxa_esp_private_authority_import_legacy(storage,bytes(identity),&provider,declared,2,old,identity)==PXA_STATUS_BAD_STATE && test.reads==3);
    assert(!pxa_esp_close(old) && !unlink(old_file) && !rmdir(old_root));
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_installations_close(&owner));
    assert(!pxa_posix_fs_remove_tree(candidate.home));
    char publisher[65],path_name[768];memcpy(publisher,candidate.locator,64);publisher[64]=0;
    assert(snprintf(path_name,sizeof(path_name),"%s/PXA/apps/%s",target_path,publisher)<(int)sizeof(path_name) && !rmdir(path_name));
    path(path_name,sizeof(path_name),target_path,"PXA/apps");assert(!rmdir(path_name));
}
static void verify_data(uint64_t epoch,const char *locator,const uint8_t install_id[16],uint64_t generation) {
    pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home;pxa_esp_app_storage_t *storage=NULL;const char *root;
    assert(!pxa_esp_installations_open(epoch,&owner) && !pxa_esp_installations_lookup(owner,locator,&home));
    assert(home.record.generation==generation && home.record.data_generation==1 && !memcmp(home.record.install_id,install_id,16));
    assert(!pxa_esp_app_storage_create(&home,epoch,0,NULL,&storage) && !pxa_esp_app_storage_existing_root(storage,&root));
    assert_file(root,"Save.sav",expected_save);assert_file(root,".pxa-kv-0","retained KV");
    pxa_permission_store_t permissions;pxa_permission_decision_t decision;char identity[130];
    strcpy(identity,locator);identity[64]=':';
    assert(!pxa_esp_app_storage_permissions(storage,&permissions));
    assert(!permissions.load(permissions.context,bytes(identity),bytes("audio"),bytes(""),&decision) && decision==PXA_PERMISSION_ALLOW);
    char filename[768];uint8_t grants[1264];path(filename,sizeof(filename),root,".pxa-directory-grants");
    int file=open(filename,O_RDONLY);assert(file>=0 && read(file,grants,sizeof(grants))==sizeof(grants) && !close(file));
    assert(!pxa_esp_app_storage_verify_grants(storage,(pxa_bytes_t){grants,sizeof(grants)-32},grants+sizeof(grants)-32));
    path(filename,sizeof(filename),root,".pxa-policy");DIR *policy=opendir(filename);assert(policy);struct dirent *entry;
    while((entry=readdir(policy)))assert(!strstr(entry->d_name,".pending"));
    assert(!closedir(policy));
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_installations_close(&owner));
}
static void verify_archive(uint64_t epoch,const char *locator,const uint8_t install_id[16],uint64_t generation) {
    pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home;char package[768];struct stat metadata;
    assert(!pxa_esp_installations_open(epoch,&owner) && !pxa_esp_installations_lookup(owner,locator,&home));
    assert(home.record.flags&PXA_INSTALL_RECORD_ARCHIVED);
    path(package,sizeof(package),home.home,"package");assert(lstat(package,&metadata)<0 && errno==ENOENT);
    assert(!pxa_esp_installations_close(&owner));verify_data(epoch,locator,install_id,generation);
}
static void cancel_checkpoint(void *context,uint8_t phase) {if(phase)*(int *)context=1;}
static pxa_status_t cancel_copy(void *context) {return *(int *)context?PXA_STATUS_CANCELLED:PXA_STATUS_OK;}
int main(int argc,char **argv) {
    assert(argc==2);char source_path[512],target_path[512],package[512],locator[130],filename[768];uint8_t der[256],encoded[8192],digest[32],install_id[16];
    path(source_path,sizeof(source_path),argv[1],"transfer-source");path(target_path,sizeof(target_path),argv[1],"transfer-target");
    assert(!mkdir(source_path,0700) && !mkdir(target_path,0700));
    path(package,sizeof(package),argv[1],"publisher.der");int file=open(package,O_RDONLY);assert(file>=0);
    ssize_t count=read(file,der,sizeof(der));assert(count>0 && !close(file));publisher=(pxa_openssl_publisher_key_t){der,(size_t)count};
    pxa_esp_storage_volume_config_t config={0};uint64_t source_epoch,target_epoch;pxa_volume_info_t volume;
    config.struct_size=sizeof(config);config.mount_path=source_path;config.name="source";config.device_identity="source";config.device_identity_bytes=6;
    config.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ|PXA_VOLUME_INSTALLABLE|PXA_VOLUME_PRIVATE_NATIVE;
    config.allocation_unit=4096;config.max_file_bytes=INT32_MAX;config.link_free=1;config.query_space=capacity;
    assert(!pxa_esp_storage_register_volume(&config,&source_epoch));
    config.mount_path=target_path;config.name="target";config.device_identity="target";
    assert(!pxa_esp_storage_register_volume(&config,&target_epoch));
    pxa_package_limits_t limits;pxa_package_limits_init(&limits);size_t size=pxa_package_manifest_workspace_size(&limits);void *workspace=malloc(size);assert(workspace);
    pxa_package_manifest_t *manifest=NULL;pxa_posix_installer_result_t result={0};result.struct_size=sizeof(result);
    result.manifest_workspace=workspace;result.manifest_workspace_size=size;result.encoded=encoded;result.encoded_capacity=sizeof(encoded);
    result.manifest=&manifest;result.root=package;result.root_capacity=sizeof(package);
    pxa_esp_installations_t *owner=NULL;pxa_install_record_t approved={0};pxa_posix_app_home_t home;pxa_install_plan_t plan;
    path(filename,sizeof(filename),argv[1],"source1");assert(!pxa_esp_installations_open(source_epoch,&owner));
    assert(!pxa_esp_installations_inspect(owner,filename,&result));
    assert(!pxa_openssl_sha256(manifest->encoded.data,manifest->encoded.size,digest));
    approved.flags=PXA_INSTALL_RECORD_ENABLED;approved.install_id[0]=1;approved.data_generation=1;strcpy(approved.app_id,"volume-test");
    assert(!pxa_openssl_sha256(der,(size_t)count,approved.publisher_root));assert(!pxa_esp_storage_volume(source_epoch,&volume));approved.app_volume=volume.id;
    assert(!pxa_esp_installations_locator(approved.publisher_root,approved.app_id,locator,sizeof(locator)));
    assert(!pxa_esp_installations_plan(owner,manifest,0,1,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    assert(!pxa_esp_installations_install(owner,filename,locator,&approved,0,digest,&plan,NULL,NULL,&result,&home));
    assert(!pxa_esp_storage_release_plan(&plan));memcpy(install_id,home.record.install_id,16);
    pxa_esp_app_storage_t *storage=NULL;const char *root;char private_path[512];
    assert(!pxa_esp_app_storage_create(&home,source_epoch,0,NULL,&storage));
    assert(!pxa_esp_app_storage_existing_root(storage,&root));save(root,"Save.sav","a real retained save",20);save(root,".pxa-kv-0","retained KV",11);
    pxa_permission_store_t permissions;char identity[130];strcpy(identity,locator);identity[64]=':';
    assert(!pxa_esp_app_storage_permissions(storage,&permissions));
    assert(!permissions.save(permissions.context,bytes(identity),bytes("audio"),bytes(""),PXA_PERMISSION_ALLOW));
    char policy_path[768];path(policy_path,sizeof(policy_path),root,".pxa-policy");
    save(policy_path,".0000000000000000000000000000000000000000000000000000000000000000.pending","uncommitted",11);
    uint8_t grants[1264]={0};memcpy(grants,"PXAGRNT2",8);pxa_write_u16(grants+8,4);
    grants[16]=1;memcpy(grants+32,volume.id.bytes,16);pxa_write_u64(grants+48,7);pxa_write_u32(grants+56,3);grants[60]=1;
    assert(!pxa_esp_app_storage_seal_grants(storage,(pxa_bytes_t){grants,sizeof(grants)-32},grants+sizeof(grants)-32));
    save(root,".pxa-directory-grants",grants,sizeof(grants));strcpy(private_path,root);root=private_path;
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_installations_close(&owner));
    verify_legacy_authority_import(target_epoch,target_path,source_path,&home,grants);
    /* Placement is an estimate, with no target namespace or capacity lease.
     * UI approval binds every source field, including reinstall identity. */
    pxa_install_record_t snapshot;pxa_install_plan_t estimate;
    assert(!pxa_esp_app_transfer_plan(source_epoch,target_epoch,locator,1,&snapshot,&estimate));
    assert(snapshot.generation==1 && !estimate.reserved && estimate.volume_count==2);
    char untouched[768];assert(snprintf(untouched,sizeof(untouched),"%s/PXA/apps/%s",target_path,locator)<(int)sizeof(untouched));
    struct stat untouched_info;assert(lstat(untouched,&untouched_info)<0 && errno==ENOENT);
    path(untouched,sizeof(untouched),source_path,"PXA/apps/.transfers");
    assert(lstat(untouched,&untouched_info)<0 && errno==ENOENT);
    /* A replaced, oversized disk manifest is rejected before its payload
     * allocation, without consuming a destination or mutating the source. */
    char disk_manifest[768];
    assert(snprintf(disk_manifest,sizeof(disk_manifest),"%s/PXA/apps/%s/package/manifest.pxm",source_path,locator)<(int)sizeof(disk_manifest));
    int disk=open(disk_manifest,O_RDWR);assert(disk>=0);
    ssize_t original=read(disk,encoded,sizeof(encoded));assert(original>0 && (size_t)original<sizeof(encoded));
    assert(!ftruncate(disk,PXA_ESP_SIGNED_MANIFEST_MAX_BYTES+1u));
    reject_oversized_manifest_allocation=1;
    assert(pxa_esp_app_transfer_plan(source_epoch,target_epoch,locator,1,&approved,&estimate)==PXA_STATUS_LIMIT_EXCEEDED);
    reject_oversized_manifest_allocation=0;
    assert(!ftruncate(disk,original) && lseek(disk,0,SEEK_SET)==0 && write(disk,encoded,(size_t)original)==original && !fsync(disk) && !close(disk));
    char media_base[768],media_publisher[768],manual_media[768],prefix[65];
    memcpy(prefix,locator,64);prefix[64]=0;
    path(media_base,sizeof(media_base),source_path,"PXA/media");assert(!mkdir(media_base,0700));
    path(media_publisher,sizeof(media_publisher),media_base,prefix);assert(!mkdir(media_publisher,0700));
    path(manual_media,sizeof(manual_media),media_base,locator);assert(!mkdir(manual_media,0700));
    assert(pxa_esp_app_transfer_plan(source_epoch,target_epoch,locator,1,&approved,&estimate)==PXA_STATUS_UNSUPPORTED);
    assert(!rmdir(manual_media) && !rmdir(media_publisher) && !rmdir(media_base));
    pxa_install_record_t stale=snapshot;stale.install_id[0]^=0x40;
    assert(pxa_esp_app_transfer_begin_approved(source_epoch,target_epoch,locator,&stale,NULL,NULL,&home)==PXA_STATUS_BAD_STATE);
    stale=snapshot;stale.data_generation++;
    assert(pxa_esp_app_transfer_begin_approved(source_epoch,target_epoch,locator,&stale,NULL,NULL,&home)==PXA_STATUS_BAD_STATE);
    stale=snapshot;stale.manifest_sha256[0]^=0x40;
    assert(pxa_esp_app_transfer_begin_approved(source_epoch,target_epoch,locator,&stale,NULL,NULL,&home)==PXA_STATUS_BAD_STATE);
    assert(pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,2,NULL,&result,&home)==PXA_STATUS_BAD_STATE);
    free_bytes=4096;
    assert(pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,1,NULL,&result,&home)==PXA_STATUS_QUOTA_EXCEEDED);
    free_bytes=UINT64_C(128)*1024*1024;
    /* Admission must charge the actual bounded transfer journal, rather than
     * dozens of extra clusters that exclude an otherwise fitting target. */
    free_bytes=UINT64_C(128)*1024; /* Exact fixture peak; headroom charged once. */
    --free_bytes;
    assert(pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,1,NULL,&result,&home)==PXA_STATUS_QUOTA_EXCEEDED);
    ++free_bytes;
    /* Fail after a partial private copy, then let the source change its save.
     * A retry must rebuild that unpublished snapshot, never merge old data. */
    grants[sizeof(grants)-1]^=1;
    path(filename,sizeof(filename),root,".pxa-directory-grants");file=open(filename,O_WRONLY);assert(file>=0);
    assert(write(file,grants,sizeof(grants))==sizeof(grants) && !close(file));
    assert(pxa_esp_app_transfer_begin_approved(source_epoch,target_epoch,locator,&snapshot,NULL,&result,&home)==PXA_STATUS_DENIED);
    scan_test_t scan={locator,0,1,0,0,0};size_t found=0;
    /* A symlink to a real pending home must not become a second candidate. */
    char pending_home[768],link_path[768],publisher_path[768];
    path(publisher_path,sizeof(publisher_path),target_path,"PXA/apps");
    char publisher_hex[65];memcpy(publisher_hex,locator,64);publisher_hex[64]=0;
    path(pending_home,sizeof(pending_home),publisher_path,locator);
    path(publisher_path,sizeof(publisher_path),target_path,"PXA/apps");
    path(link_path,sizeof(link_path),publisher_path,publisher_hex);
    path(publisher_path,sizeof(publisher_path),link_path,"link-app");assert(!symlink(pending_home,publisher_path));
    assert(!pxa_esp_app_transfer_visit(target_epoch,visit_pending,&scan,&found) && found==1 && scan.calls==1);
    scan.calls=0;scan.keep_going=0;
    assert(!pxa_esp_app_transfer_visit(target_epoch,visit_pending,&scan,&found) && found==1 && scan.calls==1);
    assert(!unlink(publisher_path));scan.calls=0;scan.keep_going=1;
    assert(!pxa_esp_app_transfer_visit(source_epoch,visit_pending,&scan,&found) && !found && !scan.calls);
    assert(!pxa_esp_installations_open(target_epoch,&owner));
    assert(pxa_esp_installations_lookup(owner,locator,&home)==PXA_STATUS_BUSY);
    assert(!pxa_esp_installations_close(&owner));
    assert(!pxa_esp_storage_drain_volume(source_epoch) && !pxa_esp_storage_unregister_volume(source_epoch));
    assert(pxa_esp_app_transfer_resume(target_epoch,locator,NULL,&result,&home)==PXA_STATUS_UNAVAILABLE);
    config.mount_path=source_path;config.name="source";config.device_identity="source";
    assert(!pxa_esp_storage_register_volume(&config,&source_epoch));
    grants[sizeof(grants)-1]^=1;
    file=open(filename,O_WRONLY);assert(file>=0 && write(file,grants,sizeof(grants))==sizeof(grants) && !close(file));
    expected_save="a new retained save!";
    path(filename,sizeof(filename),root,"Save.sav");file=open(filename,O_WRONLY|O_TRUNC);assert(file>=0);
    assert(write(file,expected_save,strlen(expected_save))==(ssize_t)strlen(expected_save) && !close(file));
    pxa_status_t status=pxa_esp_app_transfer_resume(target_epoch,locator,NULL,&result,&home);
    if(status)fprintf(stderr,"initial transfer failed %d\n",status);
    assert(!status);verify_data(target_epoch,locator,install_id,2);
    free_bytes=UINT64_C(128)*1024*1024;
    assert(!pxa_esp_installations_open(source_epoch,&owner));unsigned discovered=0;size_t invalid=0;
    assert(!pxa_esp_installations_visit(owner,count_home,&discovered,&invalid) && !discovered && !invalid);
    assert(!pxa_esp_installations_close(&owner));
    fail_after_rename=1;source_renamed=0;
    assert(pxa_esp_app_transfer_begin(target_epoch,source_epoch,locator,2,NULL,&result,&home)==PXA_STATUS_IO_ERROR);
    assert(!pxa_esp_installations_open(source_epoch,&owner));
    assert(pxa_esp_installations_lookup(owner,locator,&home)==PXA_STATUS_BUSY);
    discovered=0;assert(!pxa_esp_installations_visit(owner,count_home,&discovered,&invalid) && !discovered);
    assert(!pxa_esp_installations_close(&owner));
    scan=(scan_test_t){locator,0,1,source_epoch,0,0};
    assert(!pxa_esp_app_transfer_visit(source_epoch,visit_pending,&scan,&found) && found==1 && scan.calls==1);
    status=scan.status;assert(!status && scan.generation==3);
    if(status)fprintf(stderr,"rename recovery failed %d\n",status);
    assert(!status);verify_data(source_epoch,locator,install_id,3);
    fail_target_commit=1;source_renamed=0;
    progress_test_t progress={source_path,target_path,locator,0,0};
    pxa_esp_app_transfer_control_t control={&progress,NULL,progress_test};
    status=pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,3,&control,NULL,&home);
    assert(status==PXA_STATUS_UNAVAILABLE);
    assert(progress.polls && progress.checkpoints==1);
    assert(!pxa_esp_storage_drain_volume(source_epoch) && !pxa_esp_storage_unregister_volume(source_epoch));
    scan=(scan_test_t){locator,0,1,target_epoch,0,0};
    assert(!pxa_esp_app_transfer_visit(target_epoch,visit_pending,&scan,&found) && found==1 && scan.calls==1);
    status=scan.status;assert(!status && scan.generation==4);
    if(status)fprintf(stderr,"offline source recovery failed %d\n",status);
    assert(!status);verify_data(target_epoch,locator,install_id,4);
    /* An explicitly bound manual directory survives whole-app relocation.
     * This is the data-plane needed by legacy adoption: original game media
     * need not be packaged, copied or silently redirected with its code. */
    config.mount_path=source_path;config.name="source";config.device_identity="source";
    assert(!pxa_esp_storage_register_volume(&config,&source_epoch));
    assert(!mkdir(media_base,0700) && !mkdir(media_publisher,0700) && !mkdir(manual_media,0700));
    save(manual_media,"assets.bin","manual media preserved",22);
    assert(!pxa_esp_storage_volume(source_epoch,&volume));
    assert(!pxa_esp_installations_open(target_epoch,&owner) && !pxa_esp_installations_lookup(owner,locator,&home));
    approved=home.record;approved.generation=5;approved.flags|=PXA_INSTALL_RECORD_HAS_MEDIA;
    approved.media_volume=volume.id;
    int parent=-1,directory=-1;assert(!home_directory(owner,locator,0,&parent,&directory));
    pxa_posix_install_record_auth_t auth={owner->config.record_auth_context,bytes(locator),owner->config.record_tag};
    assert(!pxa_posix_install_record_commit_with_auth(directory,4,&approved,&auth) &&
        !pxa_posix_install_record_mirror(directory,&approved,&auth));
    assert(!pxa_esp_close(directory) && !pxa_esp_close(parent));
    assert(!pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home));
    assert(!pxa_esp_installations_storage(owner,&home,&storage) && pxa_esp_app_storage_media_epoch(storage)==source_epoch);
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_installations_close(&owner));
    assert(!pxa_esp_app_transfer_begin(target_epoch,source_epoch,locator,5,NULL,&result,&home));
    assert(home.record.generation==6 && !memcmp(home.record.media_volume.bytes,volume.id.bytes,16));
    verify_data(source_epoch,locator,install_id,6);assert_file(manual_media,"assets.bin","manual media preserved");
    assert(!pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,6,NULL,&result,&home));
    verify_data(target_epoch,locator,install_id,7);assert_file(manual_media,"assets.bin","manual media preserved");
    /* Archives have no signed code directory. They still move the complete
     * private identity and authority, including after a cold handoff. */
    assert(!pxa_esp_installations_open(target_epoch,&owner));
    assert(!pxa_esp_installations_remove(owner,locator,7,1,&home) && home.record.generation==8);
    assert(!pxa_esp_installations_close(&owner));verify_archive(target_epoch,locator,install_id,8);
    assert(!pxa_esp_app_transfer_plan(target_epoch,source_epoch,locator,8,&snapshot,&estimate));
    assert(!estimate.reserved && (snapshot.flags&PXA_INSTALL_RECORD_ARCHIVED));
    approved=snapshot;approved.install_id[0]^=1;
    assert(pxa_esp_app_transfer_begin_approved(target_epoch,source_epoch,locator,&approved,NULL,NULL,&home)==PXA_STATUS_BAD_STATE);
    int checkpoint=0;control=(pxa_esp_app_transfer_control_t){&checkpoint,cancel_copy,cancel_checkpoint};
    assert(pxa_esp_app_transfer_begin_approved(target_epoch,source_epoch,locator,&snapshot,&control,NULL,&home)==PXA_STATUS_CANCELLED);
    assert(checkpoint);verify_archive(target_epoch,locator,install_id,8);
    assert(!pxa_esp_installations_open(source_epoch,&owner));
    assert(pxa_esp_installations_lookup(owner,locator,&home)==PXA_STATUS_BUSY);
    assert(!pxa_esp_installations_close(&owner));
    assert(!pxa_esp_app_transfer_resume(source_epoch,locator,NULL,NULL,&home));
    verify_archive(source_epoch,locator,install_id,9);
    fail_after_rename=1;source_renamed=0;
    assert(pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,9,NULL,NULL,&home)==PXA_STATUS_IO_ERROR);
    assert(!pxa_esp_storage_drain_volume(source_epoch) && !pxa_esp_storage_unregister_volume(source_epoch));
    assert(pxa_esp_app_transfer_resume(target_epoch,locator,NULL,NULL,&home)==PXA_STATUS_UNAVAILABLE);
    config.mount_path=source_path;config.name="source";config.device_identity="source";
    assert(!pxa_esp_storage_register_volume(&config,&source_epoch));
    assert(!pxa_esp_app_transfer_resume(target_epoch,locator,NULL,NULL,&home));
    verify_archive(target_epoch,locator,install_id,10);
    fail_target_commit=1;source_renamed=0;
    assert(pxa_esp_app_transfer_begin(target_epoch,source_epoch,locator,10,NULL,NULL,&home)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_esp_storage_drain_volume(target_epoch) && !pxa_esp_storage_unregister_volume(target_epoch));
    assert(!pxa_esp_app_transfer_resume(source_epoch,locator,NULL,NULL,&home));
    verify_archive(source_epoch,locator,install_id,11);
    config.mount_path=target_path;config.name="target";config.device_identity="target";
    assert(!pxa_esp_storage_register_volume(&config,&target_epoch));
    assert(!pxa_esp_app_transfer_begin(source_epoch,target_epoch,locator,11,NULL,NULL,&home));
    verify_archive(target_epoch,locator,install_id,12);assert_file(manual_media,"assets.bin","manual media preserved");
    assert(!pxa_esp_installations_open(target_epoch,&owner));approved=home.record;
    approved.flags&=~PXA_INSTALL_RECORD_ARCHIVED;
    path(filename,sizeof(filename),argv[1],"source1");
    assert(!pxa_esp_installations_inspect(owner,filename,&result));
    assert(!pxa_esp_installations_plan(owner,manifest,0,0,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    assert(!pxa_esp_installations_install(owner,filename,locator,&approved,12,digest,&plan,NULL,NULL,&result,&home));
    assert(!(home.record.flags&PXA_INSTALL_RECORD_ARCHIVED));
    assert(!pxa_esp_storage_release_plan(&plan) && !pxa_esp_installations_close(&owner));
    verify_data(target_epoch,locator,install_id,13);
    assert(!pxa_esp_storage_drain_volume(source_epoch) && !pxa_esp_storage_unregister_volume(source_epoch));
    assert(!pxa_esp_installations_open(target_epoch,&owner));
    assert(pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_esp_installations_close(&owner));
    assert(!pxa_esp_storage_drain_volume(target_epoch) && !pxa_esp_storage_unregister_volume(target_epoch));
    assert(!pending_close);free(workspace);puts("whole application transfer and cold recovery passed");return 0;
}
