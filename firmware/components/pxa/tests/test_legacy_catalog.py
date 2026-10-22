#!/usr/bin/env python3
"""Exercise production legacy path/collection bodies with the real registry."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT/'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/sha.h>
#include "pxa_app_locators.h"
#include "pxa_esp_legacy_adoption.h"
#include "pxa/posix/pxa_posix_app_store.h"
#define PXA_ESP_PACKAGE_MAX_PATH 160
#define PXA_ESP_PACKAGE_ID_BYTES 130
#define ESP_LOGW(...) ((void)0)
typedef struct {
    char id[65],identity[130],storage_key[130];uint8_t publisher_key_id[32];
    int installed,volume_local,has_publisher_key_id,staged,archived,enabled;
} pxa_esp_package_entry_t;
typedef enum {PXA_ESP_PACKAGE_VIEW_RUNNABLE,PXA_ESP_PACKAGE_VIEW_MANAGED} pxa_esp_package_view_t;
static struct {
    int legacy_ready;void *metadata_lock;
    char packages_root[160],data_root[160];pxa_app_locators_t locators;
} store,*g_store=&store;
static pxa_esp_package_entry_t entry;
static unsigned commits,policy_writes;
static bool fail_commit,fail_stat,disabled;
static bool media_offline;
static unsigned media_resolutions;
static char test_mount[256];
#define CONFIG_PXA_MOUNT_POINT test_mount
typedef struct {char absolute[768],media_mount[256],media_relative[160];pxa_volume_info_t volume;} legacy_paths_t;
static bool store_is_initialized(void) {return true;}
static bool take_lock(void *lock) {(void)lock;return true;}
static void release_lock(void *lock) {(void)lock;}
static pxa_esp_package_entry_t *find_entry(const char *identity) {return !strcmp(identity,entry.identity)?&entry:NULL;}
static pxa_status_t pxa_esp_mbedtls_sha256(const uint8_t *data,size_t size,uint8_t result[32]) {
    return SHA256(data,size,result)?PXA_STATUS_OK:PXA_STATUS_INTERNAL;
}
static pxa_status_t pxa_esp_storage_media_mount(char *output,size_t capacity) {
    assert(capacity>2);if(media_offline)return PXA_STATUS_UNAVAILABLE;strcpy(output,"sd");return PXA_STATUS_OK;
}
static pxa_status_t pxa_esp_storage_resolve_volume(const char *path,pxa_volume_info_t *volume) {
    assert(!strncmp(path,test_mount,strlen(test_mount)));++media_resolutions;volume->mount_epoch=77;return PXA_STATUS_OK;
}
static bool pxa_esp_package_policy_is_disabled(const char *identity) {assert(!strcmp(identity,entry.identity));return disabled;}
static bool pxa_esp_package_policy_set_enabled(const char *identity,bool enabled) {
    assert(!strcmp(identity,entry.identity) && enabled);++policy_writes;disabled=false;return true;
}
static pxa_status_t persist(void *context,uint64_t generation,const pxa_locator_publisher_t *pubs,size_t np,
    const pxa_locator_app_t *apps,size_t na) {
    (void)context;(void)generation;(void)pubs;(void)np;(void)apps;(void)na;++commits;
    return fail_commit?PXA_STATUS_IO_ERROR:PXA_STATUS_OK;
}
static int test_lstat(const char *path,struct stat *st) {if(fail_stat) {errno=EIO;return -1;}return lstat(path,st);}
static void mkdirs(const char *path) {
    char copy[768];assert(strlen(path)<sizeof(copy));strcpy(copy,path);
    for(char *p=copy+1;;++p) {
        if(*p && *p!='/')continue;
        char saved=*p;*p=0;assert(!mkdir(copy,0700) || errno==EEXIST);*p=saved;if(!saved)break;
    }
}
#define lstat test_lstat
// PRODUCTION
int main(int argc,char **argv) {
    assert(argc==2);uint8_t root[32]={1},other[32]={2},app_key[32]={3};char private_root[256],media[256],relative[130];
    pxa_esp_package_entry_t archived={.volume_local=1,.archived=1};
    assert(package_entry_has_home(&archived) && package_entry_matches_view(&archived,PXA_ESP_PACKAGE_VIEW_MANAGED));
    assert(!package_entry_matches_view(&archived,PXA_ESP_PACKAGE_VIEW_RUNNABLE));
    archived.volume_local=0;assert(!package_entry_has_home(&archived));
    assert(!package_entry_matches_view(&archived,PXA_ESP_PACKAGE_VIEW_MANAGED));
    archived.staged=1;assert(package_entry_matches_view(&archived,PXA_ESP_PACKAGE_VIEW_MANAGED));
    assert(snprintf(store.packages_root,sizeof(store.packages_root),"%s/packages",argv[1])<(int)sizeof(store.packages_root));
    assert(snprintf(store.data_root,sizeof(store.data_root),"%s/data",argv[1])<(int)sizeof(store.data_root));
    assert(!mkdir(store.packages_root,0700) && !mkdir(store.data_root,0700));
    memcpy(entry.publisher_key_id,root,32);strcpy(entry.id,"game");entry.installed=entry.has_publisher_key_id=1;
    assert(canonical_identity(entry.identity,sizeof(entry.identity),root,entry.id));
    assert(filesystem_identity(entry.storage_key,sizeof(entry.storage_key),root,entry.id));
    pxa_app_locators_init(&store.locators,NULL,persist);store.legacy_ready=1;
    /* A signed profile-less old application works without allocating NVS. */
    fail_commit=true;
    assert(!pxa_esp_package_store_storage_paths(entry.identity,private_root,sizeof(private_root),media,sizeof(media)));
    assert(strstr(private_root,entry.storage_key) && !strcmp(media,"EXPECTED_MEDIA") && !commits && !store.locators.app_count);
    assert(pxa_esp_package_store_storage_paths("unknown",private_root,sizeof(private_root),media,sizeof(media))==PXA_STATUS_NOT_FOUND && !*private_root && !*media);
    store.locators.ready=0;
    assert(pxa_esp_package_store_storage_paths(entry.identity,private_root,sizeof(private_root),media,sizeof(media))==PXA_STATUS_UNAVAILABLE);
    store.locators.ready=1;entry.volume_local=1;
    assert(pxa_esp_package_store_storage_paths(entry.identity,private_root,sizeof(private_root),media,sizeof(media))==PXA_STATUS_UNSUPPORTED);
    entry.volume_local=0;fail_commit=false;
    assert(!pxa_app_locators_reserve(&store.locators,PXA_LOCATOR_SELF_NAMESPACE,root,"owner",root,"game",app_key,0));
    app_key[0]=4;
    assert(!pxa_app_locators_reserve(&store.locators,PXA_LOCATOR_SELF_NAMESPACE,other,"other",other,"game",app_key,0));
    unsigned before=commits;pxa_posix_app_home_t home={0};home.record.flags=0;
    memcpy(home.record.publisher_root,root,32);strcpy(home.record.app_id,"game");
    char parent[256],old_code[256];snprintf(parent,sizeof(parent),"%s/owner",store.packages_root);assert(!mkdir(parent,0700));
    snprintf(old_code,sizeof(old_code),"%s/game",parent);assert(!mkdir(old_code,0700));
    collect_legacy_locator(&home);assert(commits==before && store.locators.app_count==2);
    assert(!rmdir(old_code));snprintf(old_code,sizeof(old_code),"%s/%s",store.packages_root,entry.storage_key);assert(!mkdir(old_code,0700));
    collect_legacy_locator(&home);assert(commits==before);assert(!rmdir(old_code));
    snprintf(parent,sizeof(parent),"%s/owner",store.data_root);assert(!mkdir(parent,0700));
    snprintf(old_code,sizeof(old_code),"%s/game",parent);assert(!mkdir(old_code,0700));
    collect_legacy_locator(&home);assert(commits==before);assert(!rmdir(old_code));
    snprintf(old_code,sizeof(old_code),"%s/%s",store.data_root,entry.identity);assert(!mkdir(old_code,0700));
    collect_legacy_locator(&home);assert(commits==before);assert(!rmdir(old_code));
    fail_stat=true;collect_legacy_locator(&home);assert(commits==before);fail_stat=false;
    assert(!pxa_app_locators_set_flags(&store.locators,root,"game",PXA_LOCATOR_DATA_LEGACY|PXA_LOCATOR_DATA_MOVING));before=commits;
    collect_legacy_locator(&home);assert(commits==before);
    assert(!pxa_app_locators_set_flags(&store.locators,root,"game",0));before=commits;
    fail_commit=true;disabled=true;collect_legacy_locator(&home);
    assert(commits==before+1 && store.locators.ready && store.locators.app_count==2 && policy_writes==1 && !disabled && !home.record.flags);
    assert(!pxa_app_locators_find(&store.locators,root,"game",NULL,relative,sizeof(relative)) && !strcmp(relative,"owner/game"));
    fail_commit=false;collect_legacy_locator(&home);assert(store.locators.app_count==1 && store.locators.publisher_count==2);
    assert(pxa_app_locators_find(&store.locators,root,"game",NULL,relative,sizeof(relative))==PXA_STATUS_NOT_FOUND);
    assert(!pxa_app_locators_find(&store.locators,other,"game",NULL,relative,sizeof(relative)) && !strcmp(relative,"other/game"));
    before=commits;collect_legacy_locator(&home);assert(commits==before && !home.record.flags);
    /* Runtime always returns a fixed publisher/App root. Only explicit
     * adoption may discover an existing old AppKey directory on the card. */
    strcpy(test_mount,argv[1]);legacy_paths_t paths={0};pxa_esp_legacy_source_t source={.app_epoch=1};
    strcpy(paths.media_relative,"EXPECTED_MEDIA");
    assert(!legacy_media_source(&paths,&source,root,"game") && !source.media_epoch && !media_resolutions);
    char old_media[768],canonical_media[768];
    snprintf(old_media,sizeof(old_media),"%s/sd/PXA/media/OLD_MEDIA_KEY",test_mount);mkdirs(old_media);
    strcpy(paths.media_relative,"EXPECTED_MEDIA");
    assert(!legacy_media_source(&paths,&source,root,"game") && source.media_epoch==77 && !strcmp(source.media,"PXA/media/OLD_MEDIA_KEY"));
    snprintf(canonical_media,sizeof(canonical_media),"%s/sd/EXPECTED_MEDIA",test_mount);mkdirs(canonical_media);
    source.media_epoch=0;source.media[0]=0;strcpy(paths.media_relative,"EXPECTED_MEDIA");
    assert(!legacy_media_source(&paths,&source,root,"game") && source.media_epoch==77 && !strcmp(source.media,"EXPECTED_MEDIA"));
    assert(!rmdir(canonical_media) && !symlink(old_media,canonical_media));
    source.media_epoch=0;source.media[0]=0;strcpy(paths.media_relative,"EXPECTED_MEDIA");
    assert(legacy_media_source(&paths,&source,root,"game")==PXA_STATUS_DENIED && !source.media_epoch);
    assert(!unlink(canonical_media));media_offline=true;unsigned resolutions=media_resolutions;
    assert(!legacy_media_source(&paths,&source,root,"game") && !source.media_epoch && media_resolutions==resolutions && commits==before);
    puts("Legacy catalog: fixed signed fallback without NVS writes, exact-owner collection/retry, retained-save preservation, existing AppKey adoption and offline/symlink refusal passed");
    return 0;
}
'''


def main():
    source=(ROOT/'firmware/components/pxa/src/package/pxa_esp_package_store.c').read_text()
    bodies='\n'.join(function(source, signature) for signature in (
        'static int safe_app_id(', 'static int canonical_identity(', 'static int filesystem_identity(',
        'static bool package_entry_has_home(', 'static bool package_entry_matches_view(',
        'static __attribute__((noinline)) void collect_legacy_locator(',
        'static pxa_status_t legacy_media_at(', 'static pxa_status_t legacy_media_source(',
        'static pxa_status_t storage_paths_locked(const char *identity,',
        'pxa_status_t pxa_esp_package_store_storage_paths('))
    expected='PXA/media/'+('01'+'00'*31)+'/game'
    old_key=hashlib.sha256(b'PXA-APPKEY-1'+bytes([1])+bytes(31)+b'\x04\0game').hexdigest()
    with tempfile.TemporaryDirectory(prefix='pxa-legacy-catalog-') as directory:
        work=Path(directory);unit=work/'catalog.c';binary=work/'catalog'
        unit.write_text(FIXTURE.replace('// PRODUCTION',bodies).replace('EXPECTED_MEDIA',expected).replace('OLD_MEDIA_KEY',old_key))
        subprocess.run([os.environ.get('CC','clang'),'-std=c99','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-I'+str(ROOT/'firmware/components/pxa/src/package'),'-I'+str(ROOT/'deps/pxa-system/libpxa/include'),
            '-I'+str(ROOT/'firmware/components/pxa/src/services'),
            '-I'+str(ROOT/'deps/pxa-system/libpxa/adapters/include'),str(unit),
            str(ROOT/'firmware/components/pxa/src/package/pxa_app_locators.c'),'-lcrypto','-o',str(binary)],check=True)
        subprocess.run([str(binary),str(work)],check=True)


if __name__=='__main__':
    main()
