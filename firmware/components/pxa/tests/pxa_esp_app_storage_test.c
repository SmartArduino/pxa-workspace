#define _GNU_SOURCE
#include "pxa_esp_app_storage.h"
#include "pxa_esp_volumes.h"
#include "pxa_esp_record_seal.h"
#include "record_test_auth.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "lfs.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the production lifecycle, directory shim, volume manager and image
 * driver together. Only ESP's VFS registration is replaced; formats, mounts
 * and cold remounts below use the actual LittleFS implementation. */
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
const char *esp_err_to_name(esp_err_t error) {(void)error;return "injected";}
static uint64_t free_bytes=4*1024*1024;
static unsigned random_state=1,formats,mounts;
static int fail_mount,fail_unmount;
static lfs_t fs;
static struct lfs_config fs_config;
static esp_blockdev_handle_t mounted;
static record_test_auth_context_t device_key={{1},1};
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t record,uint8_t tag[32]) {
    return record_test_tag(&device_key,purpose,binding,record,tag);
}
pxa_status_t pxa_esp_record_verify(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t record,const uint8_t tag[32]) {
    uint8_t expected[32];pxa_status_t status=record_test_tag(&device_key,purpose,binding,record,expected);
    return status==PXA_STATUS_OK && memcmp(expected,tag,32)?PXA_STATUS_DENIED:status;
}
static pxa_bytes_t bytes(const char *s) {return (pxa_bytes_t){(const uint8_t *)s,strlen(s)};}
void *heap_caps_malloc(size_t n,unsigned caps) {(void)caps;return malloc(n);}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {(void)caps;return calloc(n,bytes);}
void heap_caps_free(void *p) {free(p);}
uint32_t esp_random(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
void vTaskDelay(TickType_t ticks) {(void)ticks;}
esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *available) {
    (void)path;*total=8*1024*1024;*available=free_bytes;return ESP_OK;
}
static pxa_status_t capacity(void *ctx,const char *path,uint64_t *total,uint64_t *available,uint8_t *known) {
    (void)ctx;(void)path;*total=8*1024*1024;*available=free_bytes;*known=3;return PXA_STATUS_OK;
}
static int fs_read(const struct lfs_config *c,lfs_block_t b,lfs_off_t off,void *out,lfs_size_t n) {
    esp_blockdev_handle_t d=c->context;
    return d->ops->read(d,out,n,(uint64_t)b*c->block_size+off,n)==ESP_OK?0:LFS_ERR_IO;
}
static int fs_write(const struct lfs_config *c,lfs_block_t b,lfs_off_t off,const void *in,lfs_size_t n) {
    esp_blockdev_handle_t d=c->context;
    return d->ops->write(d,in,(uint64_t)b*c->block_size+off,n)==ESP_OK?0:LFS_ERR_IO;
}
static int fs_erase(const struct lfs_config *c,lfs_block_t b) {
    esp_blockdev_handle_t d=c->context;
    return d->ops->erase(d,(uint64_t)b*c->block_size,c->block_size)==ESP_OK?0:LFS_ERR_IO;
}
static int fs_sync(const struct lfs_config *c) {
    esp_blockdev_handle_t d=c->context;return d->ops->sync(d)==ESP_OK?0:LFS_ERR_IO;
}
static struct lfs_config configure(esp_blockdev_handle_t device) {
    struct lfs_config c={0};c.context=device;c.read=fs_read;c.prog=fs_write;c.erase=fs_erase;c.sync=fs_sync;
    c.read_size=256;c.prog_size=256;c.block_size=4096;c.block_count=(lfs_size_t)(device->geometry.disk_size/4096);
    c.cache_size=512;c.lookahead_size=16;c.block_cycles=-1;return c;
}
esp_err_t esp_littlefs_format_blockdev(esp_blockdev_handle_t device) {
    lfs_t temporary;struct lfs_config c=configure(device);++formats;
    return lfs_format(&temporary,&c)?ESP_FAIL:ESP_OK;
}
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config) {
    assert(!mounted && !config->format_if_mount_failed && strlen(config->base_path)<=15);
    ++mounts;if(fail_mount)return fail_mount==2?ESP_ERR_NO_MEM:ESP_FAIL;
    fs_config=configure(config->blockdev);
    if(lfs_mount(&fs,&fs_config))return ESP_FAIL;
    mounted=config->blockdev;return ESP_OK;
}
esp_err_t esp_vfs_littlefs_unregister_blockdev(esp_blockdev_handle_t device) {
    assert(mounted==device);if(fail_unmount)return ESP_FAIL;
    assert(!lfs_unmount(&fs));mounted=NULL;return ESP_OK;
}
bool esp_littlefs_blockdev_mounted(esp_blockdev_handle_t device) {return mounted==device;}
static void write_save(const char *name,const char *value) {
    lfs_file_t file;assert(!lfs_file_open(&fs,&file,name,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_EXCL));
    assert(lfs_file_write(&fs,&file,value,strlen(value))==(lfs_ssize_t)strlen(value));
    assert(!lfs_file_close(&fs,&file));
}
static void read_save(const char *name,const char *expected) {
    char bytes[64]={0};lfs_file_t file;assert(!lfs_file_open(&fs,&file,name,LFS_O_RDONLY));
    assert(lfs_file_read(&fs,&file,bytes,sizeof(bytes))==(lfs_ssize_t)strlen(expected));
    assert(!strcmp(bytes,expected) && !lfs_file_close(&fs,&file));
}
static void remove_tree(const char *root) {
    DIR *dir=opendir(root);assert(dir);struct dirent *entry;char path[1024];struct stat st;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        assert(snprintf(path,sizeof(path),"%s/%s",root,entry->d_name)<(int)sizeof(path));assert(!lstat(path,&st));
        if(S_ISDIR(st.st_mode))remove_tree(path);else assert(!unlink(path));
    }
    assert(!closedir(dir) && !rmdir(root));
}
static void make_home(const char *mount,uint64_t epoch,const char *name,pxa_posix_app_home_t *home) {
    pxa_volume_info_t info;char path[512];memset(home,0,sizeof(*home));
    assert(!pxa_esp_storage_volume(epoch,&info));home->record.app_volume=info.id;
    home->record.flags=PXA_INSTALL_RECORD_ENABLED;strcpy(home->record.app_id,name);
    home->record.install_id[0]=1;home->record.publisher_root[0]=1;
    assert(snprintf(home->locator,sizeof(home->locator),"publisher/%s",name)<(int)sizeof(home->locator));
    assert(snprintf(path,sizeof(path),"%s/PXA/apps",mount)<(int)sizeof(path));assert(!mkdir(path,0700) || errno==EEXIST);
    assert(snprintf(path,sizeof(path),"%s/PXA/apps/publisher",mount)<(int)sizeof(path));assert(!mkdir(path,0700) || errno==EEXIST);
    assert(snprintf(home->home,sizeof(home->home),"%s/PXA/apps/%s",mount,home->locator)<(int)sizeof(home->home));
    assert(!mkdir(home->home,0700));
}
/* The same card can be nested below Flash or become the system root. A new
 * native pathname and mount epoch must preserve its durable authority, while
 * stale paths/epochs cannot reopen the installation. */
static void check_changed_mount_path(bool native) {
    char original[256],relocated[256],saved[768],identity[130];
    const char *base=getenv("TMPDIR")?getenv("TMPDIR"):"/tmp",*root;
    assert(snprintf(original,sizeof(original),"%s/nested-card-XXXXXX",base)<(int)sizeof(original));
    assert(snprintf(relocated,sizeof(relocated),"%s/root-card-XXXXXX",base)<(int)sizeof(relocated));
    assert(mkdtemp(original) && mkdtemp(relocated) && !rmdir(relocated));
    pxa_esp_storage_volume_config_t config={0};
    config.struct_size=sizeof(config);config.mount_path=original;config.name="portable card";
    config.device_identity="card";config.device_identity_bytes=4;config.link_free=1;
    config.allocation_unit=32768;config.max_file_bytes=INT32_MAX;config.query_space=capacity;
    config.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ|
        PXA_VOLUME_INSTALLABLE|(native?PXA_VOLUME_PRIVATE_NATIVE:0);
    uint64_t epoch;pxa_volume_info_t before,after;pxa_posix_app_home_t home;
    pxa_esp_app_storage_t *storage=NULL;
    assert(!pxa_esp_storage_register_volume(&config,&epoch));
    assert(!pxa_esp_storage_volume(epoch,&before));
    make_home(original,epoch,"portable",&home);home.record.data_generation=9;
    const pxa_install_record_t record=home.record;
    const unsigned old_formats=formats;
    assert(!pxa_esp_app_storage_create(&home,epoch,native?0:131072,NULL,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root));
    assert(snprintf(saved,sizeof(saved),"%s/Save.sav",root)<(int)sizeof(saved));
    if(native) {
        int file=open(saved,O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0);
        assert(write(file,"portable",8)==8 && !close(file));
    } else write_save("Save.sav","portable");
    assert(formats==old_formats+(native?0:1));
    uint8_t authority[32];pxa_bytes_t body=bytes("portable directory authority");
    assert(!pxa_esp_app_storage_seal_grants(storage,body,authority));
    memset(identity,'0',64);identity[1]='1';strcpy(identity+64,":portable");
    pxa_bytes_t app=bytes(identity),name=bytes("audio.playback"),scope=bytes("media");
    pxa_permission_store_t permissions;pxa_permission_decision_t decision;
    if(native) {
        assert(!pxa_esp_app_storage_permissions(storage,&permissions));
        assert(!permissions.save(permissions.context,app,name,scope,PXA_PERMISSION_ALLOW));
    }
    assert(!pxa_esp_app_storage_close(&storage));
    assert(!pxa_esp_storage_drain_volume(epoch) && !pxa_esp_storage_unregister_volume(epoch));
    assert(!rename(original,relocated));
    config.mount_path=relocated;uint64_t new_epoch;
    assert(!pxa_esp_storage_register_volume(&config,&new_epoch));
    assert(new_epoch!=epoch && !pxa_esp_storage_volume(new_epoch,&after));
    assert(!memcmp(before.id.bytes,after.id.bytes,16));
    assert(pxa_esp_app_storage_create(&home,epoch,native?0:131072,NULL,&storage)!=PXA_STATUS_OK && !storage);
    assert(pxa_esp_app_storage_create(&home,new_epoch,native?0:131072,NULL,&storage)==PXA_STATUS_DENIED && !storage);
    assert(snprintf(home.home,sizeof(home.home),"%s/PXA/apps/%s",relocated,home.locator)<(int)sizeof(home.home));
    assert(!memcmp(&record,&home.record,sizeof(record)));
    assert(!pxa_esp_app_storage_create(&home,new_epoch,native?0:131072,NULL,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root));
    assert(!pxa_esp_app_storage_verify_grants(storage,body,authority));
    if(native) {
        assert(snprintf(saved,sizeof(saved),"%s/Save.sav",root)<(int)sizeof(saved));
        char content[8];int file=open(saved,O_RDONLY);assert(file>=0);
        assert(read(file,content,sizeof(content))==8 && !memcmp(content,"portable",8) && !close(file));
        assert(!pxa_esp_app_storage_permissions(storage,&permissions));
        assert(!permissions.load(permissions.context,app,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    } else read_save("Save.sav","portable");
    assert(formats==old_formats+(native?0:1));
    assert(!pxa_esp_app_storage_close(&storage));
    assert(!pxa_esp_storage_drain_volume(new_epoch) && !pxa_esp_storage_unregister_volume(new_epoch));
    remove_tree(relocated);
}
int main(void) {
    char temporary[256],image[768],pending[768];uint64_t epoch;const char *root;
    pxa_esp_app_storage_t *storage=NULL,*duplicate=NULL;pxa_posix_app_home_t home,foreign;
    pxa_esp_storage_volume_config_t config={0};pxa_volume_info_t info;struct stat st;
    assert(snprintf(temporary,sizeof(temporary),"%s/app-storage-XXXXXX",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp")<(int)sizeof(temporary));
    assert(mkdtemp(temporary));
    config.struct_size=sizeof(config);config.mount_path=temporary;config.name="SD";
    config.device_identity="card";config.device_identity_bytes=4;config.link_free=1;
    config.allocation_unit=32768;config.max_file_bytes=INT32_MAX;config.query_space=capacity;
    config.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ|PXA_VOLUME_INSTALLABLE|PXA_VOLUME_CASE_INSENSITIVE;
    assert(!pxa_esp_storage_register_volume(&config,&epoch));make_home(temporary,epoch,"quake",&home);
    snprintf(image,sizeof(image),"%s/private/private.lfs",home.home);
    snprintf(pending,sizeof(pending),"%s/private/private.pending.lfs",home.home);
    foreign=home;strcpy(foreign.home,"/different-volume/home");
    assert(pxa_esp_app_storage_create(&foreign,epoch,131072,NULL,&storage)==PXA_STATUS_DENIED);
    assert(!pxa_esp_app_storage_create(&home,epoch,131072,NULL,&storage));
    assert(pxa_esp_app_storage_create(&home,epoch,131072,NULL,&duplicate)==PXA_STATUS_BUSY && !duplicate);
    assert(!pxa_esp_app_storage_bind_media(storage,epoch));
    assert(!pxa_esp_app_storage_bind_media(storage,epoch)); /* Idempotent retain. */
    assert(pxa_esp_app_storage_bind_media(storage,epoch+1)==PXA_STATUS_BAD_STATE);
    free_bytes=1;
    assert(pxa_esp_app_storage_root(storage,&root)==PXA_STATUS_QUOTA_EXCEEDED && !root);
    assert(!formats && stat(image,&st)<0 && stat(pending,&st)<0);
    free_bytes=4*1024*1024;
    fail_mount=1;
    assert(pxa_esp_app_storage_root(storage,&root)==PXA_STATUS_IO_ERROR && !root && !mounted);
    assert(formats==1 && !stat(image,&st) && st.st_size==131072);
    assert(!pxa_esp_storage_volume(epoch,&info) && !info.reserved_bytes);
    fail_mount=2;
    assert(pxa_esp_app_storage_root(storage,&root)==PXA_STATUS_RESOURCE_LIMIT && !root && !mounted && formats==1);
    fail_mount=0;assert(!pxa_esp_app_storage_root(storage,&root) && mounted && formats==1);
    assert(!pxa_esp_app_storage_root(storage,&root) && mounts==3); /* Lazy and shared. */
    write_save("Save.sav","upper");write_save("save.sav","lower");
    fail_unmount=1;assert(pxa_esp_app_storage_close(&storage)==PXA_STATUS_IO_ERROR && storage && mounted);
    fail_unmount=0;assert(!pxa_esp_app_storage_close(&storage) && !storage && !mounted);
    assert(!pxa_esp_app_storage_close(&storage));
    assert(!pxa_esp_app_storage_create(&home,epoch,131072,NULL,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root) && formats==1);
    read_save("Save.sav","upper");read_save("save.sav","lower");
    assert(!pxa_esp_app_storage_close(&storage));
    /* An admitted install uses its existing lease, even after staged files
     * consume the free-space snapshot. It must not reserve the same image twice. */
    make_home(temporary,epoch,"duke",&foreign);
    pxa_install_requirements_t requirements={262144,0,0,0,32768};pxa_install_plan_t plan;
    assert(!pxa_esp_storage_plan(&requirements,epoch,0,&plan) && !pxa_esp_storage_reserve_plan(&plan));
    free_bytes=1;assert(!pxa_esp_app_storage_create(&foreign,epoch,131072,&plan,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root) && formats==2);
    assert(!pxa_esp_storage_volume(epoch,&info) && info.reserved_bytes==294912);
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_storage_release_plan(&plan));
    /* A final corrupt image is preserved, never silently reformatted. */
    int file=open(image,O_RDWR);assert(file>=0);char zeros[8192]={0};
    assert(pwrite(file,zeros,sizeof(zeros),0)==sizeof(zeros) && !close(file));
    assert(!pxa_esp_app_storage_create(&home,epoch,131072,NULL,&storage));
    assert(pxa_esp_app_storage_root(storage,&root)==PXA_STATUS_IO_ERROR && !root && formats==2);
    assert(!pxa_esp_app_storage_close(&storage));
    assert(!pxa_esp_storage_drain_volume(epoch) && !pxa_esp_storage_unregister_volume(epoch));
    /* Native private filesystems do not pay for or format a nested image. */
    config.capabilities|=PXA_VOLUME_PRIVATE_NATIVE;free_bytes=4*1024*1024;
    assert(!pxa_esp_storage_register_volume(&config,&epoch));
    make_home(temporary,epoch,"native",&home);
    assert(!pxa_esp_app_storage_create(&home,epoch,0,NULL,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root) && strstr(root,"/native/private") && formats==2);
    pxa_permission_store_t permissions;pxa_permission_decision_t decision;
    char identity[130];memset(identity,'0',64);identity[1]='1';strcpy(identity+64,":native");
    pxa_bytes_t app=bytes(identity),name=bytes("audio.playback"),scope=bytes("media");
    assert(!pxa_esp_app_storage_permissions(storage,&permissions));
    assert(permissions.load(permissions.context,app,name,scope,&decision)==PXA_STATUS_NOT_FOUND && decision==PXA_PERMISSION_DENY);
    assert(!permissions.save(permissions.context,app,name,scope,PXA_PERMISSION_ALLOW));
    assert(!permissions.load(permissions.context,app,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    assert(permissions.load(permissions.context,app,name,bytes("other"),&decision)==PXA_STATUS_NOT_FOUND && !decision);
    assert(permissions.load(permissions.context,bytes("foreign:native"),name,scope,&decision)==PXA_STATUS_DENIED && !decision);
    device_key.available=0;
    assert(permissions.load(permissions.context,app,name,scope,&decision)==PXA_STATUS_UNAVAILABLE && !decision);
    device_key.available=1;
    assert(!pxa_esp_app_storage_close(&storage));
    foreign=home;foreign.record.install_id[0]=2;
    assert(!pxa_esp_app_storage_create(&foreign,epoch,0,NULL,&storage));
    assert(!pxa_esp_app_storage_permissions(storage,&permissions));
    assert(permissions.load(permissions.context,app,name,scope,&decision)==PXA_STATUS_DENIED && !decision);
    assert(!pxa_esp_app_storage_close(&storage));
    assert(!pxa_esp_app_storage_create(&home,epoch,0,NULL,&storage));
    assert(!pxa_esp_app_storage_permissions(storage,&permissions));
    assert(!permissions.load(permissions.context,app,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    assert(!permissions.save(permissions.context,app,name,scope,PXA_PERMISSION_DENY));
    assert(permissions.load(permissions.context,app,name,scope,&decision)==PXA_STATUS_NOT_FOUND && !decision);
    assert(!pxa_esp_app_storage_close(&storage));
    home.record.data_generation=1;
    assert(!pxa_esp_app_storage_create(&home,epoch,0,NULL,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root) && strstr(root,"/data/0000000000000001"));
    char preserved[768];snprintf(preserved,sizeof(preserved),"%s/Save.sav",root);
    file=open(preserved,O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0);
    assert(write(file,"old-save",8)==8 && !close(file));
    uint8_t authority[32];pxa_bytes_t grant_body=bytes("directory authority");
    assert(!pxa_esp_app_storage_seal_grants(storage,grant_body,authority));
    /* Moving an installation preserves its identity/data generation but must
     * authenticate and reseal declared decisions for the new physical volume. */
    char destination[256];uint64_t destination_epoch;const char *destination_root;
    assert(snprintf(destination,sizeof(destination),"%s/transfer-storage-XXXXXX",
        getenv("TMPDIR")?getenv("TMPDIR"):"/tmp")<(int)sizeof(destination) && mkdtemp(destination));
    config.mount_path=destination;config.device_identity="destination";config.device_identity_bytes=11;
    assert(!pxa_esp_storage_register_volume(&config,&destination_epoch));
    make_home(destination,destination_epoch,"native",&foreign);foreign.record.data_generation=1;
    pxa_esp_app_storage_t *target=NULL;pxa_permission_store_t target_permissions;
    assert(!pxa_esp_app_storage_create(&foreign,destination_epoch,0,NULL,&target));
    pxa_package_permission_t migrated_permission={name,scope,0};
    assert(pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1)==PXA_STATUS_BAD_STATE);
    assert(!pxa_esp_app_storage_root(target,&destination_root));
    assert(pxa_esp_app_storage_verify_grants(target,grant_body,authority)==PXA_STATUS_DENIED);
    uint8_t migrated_authority[32],bad_authority[32];
    assert(!pxa_esp_app_storage_transfer_grants(storage,target,grant_body,authority,migrated_authority));
    assert(memcmp(authority,migrated_authority,32));
    assert(!pxa_esp_app_storage_verify_grants(target,grant_body,migrated_authority));
    assert(pxa_esp_app_storage_verify_grants(storage,grant_body,migrated_authority)==PXA_STATUS_DENIED);
    memcpy(bad_authority,authority,32);bad_authority[0]^=1;
    assert(pxa_esp_app_storage_transfer_grants(storage,target,grant_body,bad_authority,migrated_authority)==PXA_STATUS_DENIED);
    assert(!pxa_esp_app_storage_verify_grants(target,grant_body,migrated_authority));
    assert(!pxa_esp_app_storage_permissions(storage,&permissions));
    assert(!pxa_esp_app_storage_permissions(target,&target_permissions));
    assert(!permissions.save(permissions.context,app,name,scope,PXA_PERMISSION_ALLOW));
    assert(!pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1));
    assert(!target_permissions.load(target_permissions.context,app,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    assert(!permissions.load(permissions.context,app,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    char source_policy[768],target_policy[768],policy_name[65];uint8_t digest[32],source_tag[32],target_tag[32],actual_tag[32];
    assert(snprintf(source_policy,sizeof(source_policy),"%s/.pxa-policy",root)<(int)sizeof(source_policy));
    DIR *policy=opendir(source_policy);assert(policy);struct dirent *item;policy_name[0]=0;
    while((item=readdir(policy)))if(strlen(item->d_name)==64)strcpy(policy_name,item->d_name);
    assert(*policy_name && !closedir(policy));
    for(size_t i=0;i<32;++i) {unsigned value;assert(sscanf(policy_name+2*i,"%2x",&value)==1);digest[i]=(uint8_t)value;}
    assert(snprintf(source_policy,sizeof(source_policy),"%s/.pxa-policy/%s",root,policy_name)<(int)sizeof(source_policy));
    assert(snprintf(target_policy,sizeof(target_policy),"%s/.pxa-policy/%s",destination_root,policy_name)<(int)sizeof(target_policy));
    file=open(source_policy,O_RDONLY);assert(file>=0 && read(file,source_tag,32)==32 && !close(file));
    file=open(target_policy,O_RDONLY);assert(file>=0 && read(file,actual_tag,32)==32 && !close(file));
    assert(!pxa_esp_app_storage_transfer_permission_tag(storage,target,digest,source_tag,target_tag));
    assert(!memcmp(target_tag,actual_tag,32) && memcmp(source_tag,target_tag,32));
    digest[0]^=1;
    assert(pxa_esp_app_storage_transfer_permission_tag(storage,target,digest,source_tag,target_tag)==PXA_STATUS_DENIED);
    assert(!memcmp(target_tag,actual_tag,32));digest[0]^=1;
    file=open(target_policy,O_WRONLY);assert(file>=0 && write(file,source_tag,32)==32 && !close(file));
    assert(target_permissions.load(target_permissions.context,app,name,scope,&decision)==PXA_STATUS_DENIED);
    file=open(target_policy,O_WRONLY);assert(file>=0 && write(file,target_tag,32)==32 && !close(file));
    assert(!target_permissions.load(target_permissions.context,app,name,scope,&decision) && decision==PXA_PERMISSION_ALLOW);
    assert(!permissions.save(permissions.context,app,name,scope,PXA_PERMISSION_DENY));
    assert(!pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1));
    assert(target_permissions.load(target_permissions.context,app,name,scope,&decision)==PXA_STATUS_NOT_FOUND);
    assert(!permissions.save(permissions.context,app,name,scope,PXA_PERMISSION_ALLOW));
    device_key.available=0;
    assert(pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1)==PXA_STATUS_UNAVAILABLE);
    device_key.available=1;
    assert(!pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1));
    assert(!pxa_esp_app_storage_close(&target));
    foreign.record.install_id[0]=2;
    assert(!pxa_esp_app_storage_create(&foreign,destination_epoch,0,NULL,&target));
    assert(pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1)==PXA_STATUS_DENIED);
    assert(pxa_esp_app_storage_transfer_grants(storage,target,grant_body,authority,migrated_authority)==PXA_STATUS_DENIED);
    assert(!pxa_esp_app_storage_close(&target));
    foreign.record.install_id[0]=1;foreign.record.data_generation=2;
    assert(!pxa_esp_app_storage_create(&foreign,destination_epoch,0,NULL,&target));
    assert(pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1)==PXA_STATUS_DENIED);
    assert(!pxa_esp_app_storage_close(&target));
    foreign.record.data_generation=1;
    assert(!pxa_esp_app_storage_create(&foreign,destination_epoch,0,NULL,&target));
    assert(!pxa_esp_app_storage_root(target,&destination_root));
    assert(!pxa_esp_storage_drain_volume(destination_epoch));
    assert(pxa_esp_app_storage_transfer_permissions(storage,target,&migrated_permission,1)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_esp_app_storage_close(&target) && !pxa_esp_storage_unregister_volume(destination_epoch));
    remove_tree(destination);
    assert(!pxa_esp_app_storage_close(&storage));
    home.record.data_generation=2;
    assert(!pxa_esp_app_storage_create(&home,epoch,0,NULL,&storage));
    assert(!pxa_esp_app_storage_root(storage,&root) && strstr(root,"/data/0000000000000002"));
    assert(!stat(preserved,&st) && st.st_size==8); /* Staging a root preserves the old generation. */
    assert(pxa_esp_app_storage_verify_grants(storage,grant_body,authority)==PXA_STATUS_DENIED);
    assert(!pxa_esp_storage_drain_volume(epoch));
    assert(pxa_esp_storage_unregister_volume(epoch)==PXA_STATUS_BUSY);
    assert(pxa_esp_app_storage_root(storage,&root)==PXA_STATUS_UNAVAILABLE && !root);
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_storage_unregister_volume(epoch));
    remove_tree(temporary);check_changed_mount_path(false);check_changed_mount_path(true);
    puts("private application storage lifecycle and changed mount paths passed");return 0;
}
