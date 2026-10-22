/* Reuse the real volume/crypto/installer platform harness. The ordinary move
 * regression remains a separate executable; this one drives legacy adoption. */
#define main ordinary_transfer_regression_main
#define test_log harness_test_log
#include "pxa_esp_app_transfer_test.c"
#undef test_log
#undef main
#include "pxa_esp_legacy_adoption.h"
#include "pxa/posix/pxa_posix_storage.h"
#include <stdarg.h>
void test_log(const char *tag,const char *format,...) {
    va_list args;va_start(args,format);fprintf(stderr,"%s: ",tag);vfprintf(stderr,format,args);fputc('\n',stderr);va_end(args);
}

static char legacy_identity[130];
static unsigned legacy_reads;
static int legacy_invalid;
static int legacy_retired,legacy_cleanup_failure;
static unsigned legacy_cleanup_calls;
pxa_status_t pxa_esp_permission_store_peek(void *context,pxa_bytes_t identity,pxa_bytes_t name,
    pxa_bytes_t scope,pxa_permission_decision_t *decision) {
    (void)context;++legacy_reads;
    assert(identity.size==strlen(legacy_identity) && !memcmp(identity.data,legacy_identity,identity.size));
    if(legacy_invalid) {*decision=(pxa_permission_decision_t)99;return PXA_STATUS_OK;}
    if(legacy_retired) {*decision=PXA_PERMISSION_DENY;return PXA_STATUS_NOT_FOUND;}
    if(name.size==14 && !memcmp(name.data,"audio.playback",14)) {
        assert(scope.size==5 && !memcmp(scope.data,"media",5));*decision=PXA_PERMISSION_ALLOW;return PXA_STATUS_OK;
    }
    *decision=PXA_PERMISSION_DENY;return PXA_STATUS_NOT_FOUND;
}
pxa_status_t pxa_esp_permission_store_clear_app(pxa_bytes_t identity,const pxa_package_permission_t *permissions,size_t count) {
    assert(identity.size==strlen(legacy_identity) && !memcmp(identity.data,legacy_identity,identity.size));
    assert(permissions && count==2);++legacy_cleanup_calls;
    if(legacy_cleanup_failure)return PXA_STATUS_IO_ERROR;
    legacy_retired=1;return PXA_STATUS_OK;
}
static void copy_fixture(const char *from,const char *to) {
    assert(!mkdir(to,0700));DIR *dir=opendir(from);assert(dir);struct dirent *entry;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char source[1024],target[1024];path(source,sizeof(source),from,entry->d_name);path(target,sizeof(target),to,entry->d_name);
        struct stat st;assert(!lstat(source,&st));
        if(S_ISDIR(st.st_mode))copy_fixture(source,target);
        else {
            assert(S_ISREG(st.st_mode));int input=open(source,O_RDONLY),output=open(target,O_WRONLY|O_CREAT|O_EXCL,0600);
            assert(input>=0 && output>=0);uint8_t buffer[4096];ssize_t n;
            while((n=read(input,buffer,sizeof(buffer)))>0)assert(write(output,buffer,(size_t)n)==n);
            assert(!n && !fsync(output) && !close(output) && !close(input));
        }
    }
    assert(!closedir(dir));
}
static void kv(const char *root,uint8_t create) {
    char directory[768];path(directory,sizeof(directory),root,".pxa-storage");
    if(create)assert(!mkdir(directory,0700));
    pxa_posix_storage_config_t config={sizeof(config),directory,8,128,1024};
    size_t size=pxa_posix_storage_workspace_size(&config);void *memory=malloc(size);assert(memory);
    pxa_posix_storage_t *storage=NULL;pxa_storage_backend_t backend;
    assert(!pxa_posix_storage_init(memory,size,&config,&storage,&backend));
    if(create)assert(!backend.set(backend.context,bytes("progress"),bytes("level-7")));
    else {uint8_t result[32];size_t count=0;assert(!backend.get(backend.context,bytes("progress"),result,sizeof(result),&count) &&
        count==7 && !memcmp(result,"level-7",7));}
    pxa_posix_storage_deinit(storage);free(memory);
}
static void old_grants(const char *root,const pxa_volume_info_t *volume) {
    uint8_t data[1264]={0};memcpy(data,"PXAGRNT1",8);pxa_write_u16(data+8,4);
    data[16]=1;memcpy(data+32,volume->id.bytes,16);pxa_write_u64(data+48,7);pxa_write_u32(data+56,3);data[60]=1;
    assert(!pxa_openssl_sha256(data,sizeof(data)-32,data+sizeof(data)-32));save(root,legacy_identity,data,sizeof(data));
}
static void imported(uint64_t epoch,const char *locator,const pxa_install_record_t *approved,uint8_t have_private) {
    pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home;
    assert(!pxa_esp_installations_open(epoch,&owner));
    size_t capacity_bytes=8192;uint8_t encoded[8192];char result_root[512];pxa_package_limits_t limits;
    pxa_package_limits_init(&limits);size_t size=pxa_package_manifest_workspace_size(&limits);void *memory=malloc(size);assert(memory);
    pxa_package_manifest_t *manifest=NULL;pxa_posix_installer_result_t result={.struct_size=sizeof(result),
        .manifest=&manifest,.encoded=encoded,.encoded_capacity=capacity_bytes,.manifest_workspace=memory,
        .manifest_workspace_size=size,.root=result_root,.root_capacity=sizeof(result_root)};
    pxa_status_t status=pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home);
    if(status)fprintf(stderr,"adopted activation verification failed: %d\n",status);
    assert(!status && home.record.generation>=1 && !memcmp(home.record.install_id,approved->install_id,16) &&
        home.record.data_generation==1 && !memcmp(home.record.media_volume.bytes,approved->media_volume.bytes,16));
    pxa_esp_app_storage_t *storage=NULL;const char *root;
    assert(!pxa_esp_installations_storage(owner,&home,&storage));
    assert(!pxa_esp_app_storage_existing_root(storage,&root));
    if(have_private) {assert_file(root,"Save.sav","legacy save");kv(root,0);}
    pxa_permission_store_t provider;pxa_permission_decision_t decision=PXA_PERMISSION_DENY;
    assert(!pxa_esp_app_storage_permissions(storage,&provider));
    assert(!provider.load(provider.context,bytes(legacy_identity),bytes("audio.playback"),bytes("media"),&decision) && decision==PXA_PERMISSION_ALLOW);
    pxa_status_t denied=provider.load(provider.context,bytes(legacy_identity),bytes("network.http"),bytes(""),&decision);
    assert((denied==PXA_STATUS_OK || denied==PXA_STATUS_NOT_FOUND) && decision==PXA_PERMISSION_DENY);
    char file_path[768];path(file_path,sizeof(file_path),root,".pxa-directory-grants");int file=open(file_path,O_RDONLY);assert(file>=0);
    uint8_t grants[1264];assert(read(file,grants,sizeof(grants))==sizeof(grants) && !close(file) && !memcmp(grants,"PXAGRNT2",8));
    assert(!pxa_esp_app_storage_verify_grants(storage,(pxa_bytes_t){grants,sizeof(grants)-32},grants+sizeof(grants)-32));
    assert(!pxa_esp_app_storage_close(&storage) && !pxa_esp_installations_close(&owner));free(memory);
}
typedef struct {uint64_t epoch;unsigned cut,seen;int drained;} adoption_fault_t;
static void interrupt_adoption(void *context,uint8_t phase) {
    adoption_fault_t *f=context;if(!phase)return;
    f->seen|=1u<<phase;
    if(phase==f->cut && !f->drained) {assert(!pxa_esp_storage_drain_volume(f->epoch));f->drained=1;}
}
int main(int argc,char **argv) {
    assert(argc==2);char template[768],key_path[768];uint8_t der[256];
    path(key_path,sizeof(key_path),argv[1],"publisher.der");int key_fd=open(key_path,O_RDONLY);assert(key_fd>=0);
    ssize_t key_bytes=read(key_fd,der,sizeof(der));assert(key_bytes>0 && !close(key_fd));publisher=(pxa_openssl_publisher_key_t){der,(size_t)key_bytes};
    path(template,sizeof(template),argv[1],"legacy-template");
    for(unsigned cut=0;cut<=12;++cut) {
        legacy_retired=0;legacy_cleanup_calls=0;legacy_cleanup_failure=cut==11;
        char source_path[512],media_path[512],name[64],old_code[768],old_private[768],old_media[768],grants_root[768];
        snprintf(name,sizeof(name),"legacy-source-%u",cut);path(source_path,sizeof(source_path),argv[1],name);assert(!mkdir(source_path,0700));
        snprintf(name,sizeof(name),"legacy-media-%u",cut);path(media_path,sizeof(media_path),argv[1],name);assert(!mkdir(media_path,0700));
        pxa_esp_storage_volume_config_t config={.struct_size=sizeof(config),.mount_path=source_path,.name="source",
            .device_identity="source",.device_identity_bytes=6,.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ|
            PXA_VOLUME_INSTALLABLE|PXA_VOLUME_PRIVATE_NATIVE,.allocation_unit=4096,.max_file_bytes=INT32_MAX,.link_free=1,.query_space=capacity};
        uint64_t source_epoch,media_epoch;assert(!pxa_esp_storage_register_volume(&config,&source_epoch));
        config.mount_path=media_path;config.name="media";config.device_identity="media!";
        assert(!pxa_esp_storage_register_volume(&config,&media_epoch));
        pxa_volume_info_t source_volume,media_volume;assert(!pxa_esp_storage_volume(source_epoch,&source_volume) && !pxa_esp_storage_volume(media_epoch,&media_volume));
        char directory_path[768];path(directory_path,sizeof(directory_path),source_path,"pxa-state");assert(!mkdir(directory_path,0700));
        path(directory_path,sizeof(directory_path),source_path,"pxa-state/packages");assert(!mkdir(directory_path,0700));
        path(directory_path,sizeof(directory_path),source_path,"pxa-state/packages/legacy");assert(!mkdir(directory_path,0700));
        path(old_code,sizeof(old_code),directory_path,"volume-test");copy_fixture(template,old_code);
        path(directory_path,sizeof(directory_path),source_path,"pxa-state/data");assert(!mkdir(directory_path,0700));
        path(directory_path,sizeof(directory_path),source_path,"pxa-state/data/legacy");assert(!mkdir(directory_path,0700));
        path(old_private,sizeof(old_private),directory_path,"volume-test");
        const uint8_t have_private=cut!=11;
        struct stat save_before={0},media_before={0},code_before;
        if(have_private) {
            assert(!mkdir(old_private,0700));save(old_private,"Save.sav","legacy save",11);kv(old_private,1);
            path(directory_path,sizeof(directory_path),old_private,"Save.sav");assert(!lstat(directory_path,&save_before));
        }
        path(directory_path,sizeof(directory_path),old_code,"artifacts/main.wasm");assert(!lstat(directory_path,&code_before));
        path(directory_path,sizeof(directory_path),media_path,"PXA/media");assert(!mkdir(directory_path,0700));
        path(directory_path,sizeof(directory_path),media_path,"PXA/media/legacy");assert(!mkdir(directory_path,0700));
        path(old_media,sizeof(old_media),directory_path,"volume-test");assert(!mkdir(old_media,0700));
        save(old_media,"pak0.pak","legacy media",12);path(directory_path,sizeof(directory_path),old_media,"pak0.pak");assert(!lstat(directory_path,&media_before));
        path(grants_root,sizeof(grants_root),source_path,"pxa-state/grants");assert(!mkdir(grants_root,0700));
        uint8_t publisher_root[32];char locator[130];assert(!pxa_openssl_sha256(der,(size_t)key_bytes,publisher_root));
        assert(!pxa_esp_installations_locator(publisher_root,"volume-test",locator,sizeof(locator)));
        strcpy(legacy_identity,locator);legacy_identity[64]=':';old_grants(grants_root,&media_volume);
        char owner_root[768],owner_name[130];path(owner_root,sizeof(owner_root),source_path,"pxa-state/owners");
        assert(!mkdir(owner_root,0700));strcpy(owner_name,legacy_identity);owner_name[64]='~';
        uint8_t owner_record[40]={0};memcpy(owner_record,"PXAO\1\0\0\0",8);memcpy(owner_record+8,publisher_root,32);
        if(cut==12)owner_record[8]^=1; /* Foreign owner metadata must survive. */
        save(owner_root,owner_name,owner_record,sizeof(owner_record));
        char packages_root[768],lock_name[136];path(packages_root,sizeof(packages_root),source_path,"pxa-state/packages");
        snprintf(lock_name,sizeof(lock_name),"%s.lock",owner_name);save(packages_root,lock_name,"",0);
        pxa_esp_legacy_source_t source={.app_epoch=source_epoch,.media_epoch=cut==12?0:media_epoch};
        strcpy(source.package,"pxa-state/packages/legacy/volume-test");strcpy(source.private_root,"pxa-state/data/legacy/volume-test");
        if(source.media_epoch)strcpy(source.media,"PXA/media/legacy/volume-test");
        pxa_install_record_t approved;pxa_install_plan_t plan;
        pxa_status_t status=pxa_esp_legacy_adoption_prepare(&source,1,&approved,&plan);
        if(status)fprintf(stderr,"legacy prepare cut=%u status=%d\n",cut,status);
        assert(!status && source.private_present==have_private && approved.generation==1 && !plan.reserved);
        path(directory_path,sizeof(directory_path),source_path,"PXA/apps");struct stat metadata;
        assert(lstat(directory_path,&metadata)<0 && errno==ENOENT); /* Planning is read-only. */
        if(!cut) {
            pxa_install_record_t stale=approved;stale.manifest_sha256[0]^=1;pxa_posix_app_home_t unused;
            assert(pxa_esp_legacy_adoption_begin(&source,&stale,NULL,&unused)==PXA_STATUS_BAD_STATE);
            free_bytes=1;assert(pxa_esp_legacy_adoption_begin(&source,&approved,NULL,&unused)==PXA_STATUS_QUOTA_EXCEEDED);
            free_bytes=UINT64_C(128)*1024*1024;
            legacy_invalid=1;
            assert(pxa_esp_legacy_adoption_begin(&source,&approved,NULL,&unused)==PXA_STATUS_DENIED);legacy_invalid=0;
            char grants_file[1024];path(grants_file,sizeof(grants_file),grants_root,legacy_identity);
            int fd=open(grants_file,O_RDWR);assert(fd>=0);uint8_t value;assert(pread(fd,&value,1,1263)==1);value^=1;assert(pwrite(fd,&value,1,1263)==1 && !close(fd));
            unsigned before=legacy_reads;
            assert(pxa_esp_legacy_adoption_begin(&source,&approved,NULL,&unused)==PXA_STATUS_DENIED && legacy_reads==before);
            fd=open(grants_file,O_RDWR);value^=1;assert(fd>=0 && pwrite(fd,&value,1,1263)==1 && !close(fd));
            assert(!lstat(old_code,&metadata) && !lstat(old_private,&metadata));
        }
        adoption_fault_t fault={source_epoch,cut<=10?cut:0,0,0};
        uint8_t ready_receipt[2][1024];size_t ready_size[2]={0};
        const char *receipt_names[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1};
        pxa_esp_app_transfer_control_t control={&fault,NULL,interrupt_adoption};pxa_posix_app_home_t home;
        status=pxa_esp_legacy_adoption_begin(&source,&approved,&control,&home);
        if(cut>=1 && cut<=10) {
            if(status!=PXA_STATUS_UNAVAILABLE)fprintf(stderr,"legacy interrupt cut=%u status=%d seen=%x\n",cut,status,fault.seen);
            assert(status==PXA_STATUS_UNAVAILABLE && fault.drained && !home.record.generation);
            if(cut>=2)assert(lstat(old_code,&metadata)<0 && errno==ENOENT);
            else assert(!lstat(old_code,&metadata));
            if(cut>=3)assert(lstat(old_private,&metadata)<0 && errno==ENOENT);
            assert(!pxa_esp_storage_unregister_volume(source_epoch));
            config.mount_path=source_path;config.name="source";config.device_identity="source";
            assert(!pxa_esp_storage_register_volume(&config,&source_epoch));
            pxa_esp_installations_t *owner=NULL;assert(!pxa_esp_installations_open(source_epoch,&owner));
            assert(pxa_esp_installations_lookup(owner,locator,&home)==PXA_STATUS_BUSY && !pxa_esp_installations_close(&owner));
            assert(pxa_esp_app_transfer_resume(source_epoch,locator,NULL,NULL,&home)==PXA_STATUS_DENIED);
            if(cut==4)for(unsigned i=0;i<2;++i) {
                char filename[1024];assert(snprintf(filename,sizeof(filename),"%s/PXA/apps/%s/%s",source_path,locator,receipt_names[i])<(int)sizeof(filename));
                int fd=open(filename,O_RDONLY);assert(fd>=0);
                ssize_t size=read(fd,ready_receipt[i],sizeof(ready_receipt[i]));
                assert(size>0 && size<(ssize_t)sizeof(ready_receipt[i]) && !close(fd));ready_size[i]=(size_t)size;
            }
            if(cut==10)free_bytes=24*1024; /* Only replacement peak, not all old allocations again. */
            status=pxa_esp_legacy_adoption_resume(source_epoch,locator,NULL,&home);
            if(status)fprintf(stderr,"legacy resume cut=%u status=%d\n",cut,status);
        }
        assert(!status && home.record.generation==1 && !memcmp(home.record.install_id,approved.install_id,16));
        assert(legacy_cleanup_calls==1 && legacy_retired==!legacy_cleanup_failure);
        path(directory_path,sizeof(directory_path),grants_root,legacy_identity);assert(lstat(directory_path,&metadata)<0 && errno==ENOENT);
        path(directory_path,sizeof(directory_path),owner_root,owner_name);
        if(cut==12)assert(!lstat(directory_path,&metadata));
        else assert(lstat(directory_path,&metadata)<0 && errno==ENOENT);
        path(directory_path,sizeof(directory_path),packages_root,lock_name);
        if(cut==12)assert(!lstat(directory_path,&metadata));
        else assert(lstat(directory_path,&metadata)<0 && errno==ENOENT);
        free_bytes=UINT64_C(128)*1024*1024;
        assert(lstat(old_code,&metadata)<0 && errno==ENOENT && lstat(old_private,&metadata)<0 && errno==ENOENT);
        char location[768];path(location,sizeof(location),home.home,"package/artifacts/main.wasm");assert(!lstat(location,&metadata) && metadata.st_ino==code_before.st_ino);
        if(have_private) {path(location,sizeof(location),home.home,"data/0000000000000001/Save.sav");assert(!lstat(location,&metadata) && metadata.st_ino==save_before.st_ino);}
        if(cut!=12) {
            assert(lstat(old_media,&metadata)<0 && errno==ENOENT);
            assert(snprintf(location,sizeof(location),"%s/PXA/media/%s/pak0.pak",media_path,locator)<(int)sizeof(location));
            assert(!lstat(location,&metadata) && metadata.st_ino==media_before.st_ino);
        }
        imported(source_epoch,locator,&approved,have_private);
        if(cut==4) {
            /* Replaying an authenticated pre-publication journal must not
             * replace the now-authoritative private permission/grant files. */
            for(unsigned i=0;i<2;++i)save(home.home,receipt_names[i],ready_receipt[i],ready_size[i]);
            unsigned reads=legacy_reads;pxa_posix_app_home_t ignored;
            assert(pxa_esp_legacy_adoption_resume(source_epoch,locator,NULL,&ignored)==PXA_STATUS_DENIED && legacy_reads==reads);
            for(unsigned i=0;i<2;++i) {
                char filename[1024];path(filename,sizeof(filename),home.home,receipt_names[i]);assert(!unlink(filename));
            }
            imported(source_epoch,locator,&approved,have_private);
        }
        /* A converted application now uses the ordinary whole-app mover;
         * private FS, actual KV snapshots and authority survive together. */
        if(!cut) {
            assert(!pxa_esp_app_transfer_begin(source_epoch,media_epoch,locator,1,NULL,NULL,&home));
            imported(media_epoch,locator,&approved,1);
        }
        assert(!pxa_esp_storage_drain_volume(source_epoch) && !pxa_esp_storage_unregister_volume(source_epoch));
        assert(!pxa_esp_storage_drain_volume(media_epoch) && !pxa_esp_storage_unregister_volume(media_epoch));
    }
    puts("Legacy adoption: signed code, inode-preserving private/media rename, real KV/grants, read-only admission, ten remount recovery checkpoints and subsequent whole-app move passed");
    return 0;
}
