#!/usr/bin/env python3
"""Production deployment routing and retained-duplicate recovery regression."""
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pxa_esp_package_store.h"
#include "pxa/posix/pxa_posix_app_store.h"
#define PXA_ESP_PACKAGE_MAX_PATH 160
#define PXA_ESP_PACKAGE_MAX_ID 65
#define PXA_ESP_PACKAGE_DIGEST_BYTES 32
#define ESP_LOGW(...) ((void)0)
typedef struct {
    char id[65],identity[130],root[160],inbox_root[160];uint8_t publisher_key_id[32];
    int installed,volume_local,ambiguous,built_in,staged,has_publisher_key_id;
    uint64_t mount_epoch;
} pxa_esp_package_entry_t;
static struct {char inbox_root[160],state_root[160];void *transaction_lock,*metadata_lock;pxa_posix_installer_t *installer;} store,*g_store=&store;
static pxa_esp_package_entry_t entry;
static pxa_package_manifest_t manifest;
static int transaction,metadata,native_calls,legacy_calls,cleanup_calls,refresh_calls;
static int fail_move;
static pxa_status_t native_status;
static bool store_is_initialized(void) {return true;}
static bool take_lock(void *lock) {int *value=lock;assert(!*value);*value=1;return true;}
static void release_lock(void *lock) {int *value=lock;assert(*value);*value=0;}
static int safe_app_id(const char *id) {(void)id;return 0;}
static int is_directory(const char *p) {struct stat s;return !lstat(p,&s)&&S_ISDIR(s.st_mode);}
static int is_regular_file(const char *p) {struct stat s;return !lstat(p,&s)&&S_ISREG(s.st_mode);}
static bool publish_transfer_locked(const char *id) {(void)id;abort();}
static pxa_esp_package_entry_t *find_entry(const char *id) {assert(!strcmp(id,entry.identity));return &entry;}
static int scan_inbox(int a) {(void)a;assert(transaction&&metadata);return 1;}
static bool bind_store_result_buffers(pxa_posix_installer_result_t *r) {(void)r;return true;}
static bool manifest_matches_app_id(const pxa_package_manifest_t *m,const char *id) {assert(m==&manifest);return !strcmp(id,entry.id);}
static int manifest_publisher_root(const pxa_package_manifest_t *m,uint8_t root[32]) {assert(m==&manifest);memcpy(root,entry.publisher_key_id,32);return 1;}
static int refresh_entry_from_manifest(const char *id,const char *root,int builtin,const pxa_package_manifest_t *m) {
    (void)root;(void)builtin;assert(m==&manifest&&!strcmp(id,entry.id)&&!entry.volume_local);++refresh_calls;return 1;
}
static const char *path_basename(const char *p) {const char *s=strrchr(p,'/');return s?s+1:p;}
static bool remove_staging_source(const char *p) {assert(transaction&&metadata&&!strcmp(p,entry.inbox_root));++cleanup_calls;return true;}
pxa_status_t pxa_posix_installer_adopt_staged_directory(pxa_posix_installer_t *i,const char *p,const pxa_posix_installer_identity_t *id,
    pxa_posix_installer_result_t *r,pxa_posix_install_disposition_t *d) {
    (void)i;(void)p;(void)id;(void)d;assert(transaction&&metadata&&!entry.volume_local);++legacy_calls;*r->manifest=&manifest;strcpy(r->root,entry.root);return PXA_STATUS_OK;
}
pxa_status_t pxa_posix_installer_install_for_identity(pxa_posix_installer_t *i,const char *p,const pxa_posix_installer_identity_t *id,
    pxa_posix_installer_result_t *r,pxa_posix_install_disposition_t *d) {return pxa_posix_installer_adopt_staged_directory(i,p,id,r,d);}
pxa_status_t pxa_posix_installer_recover(pxa_posix_installer_t *i,const pxa_posix_installer_identity_t *id) {(void)i;(void)id;return PXA_STATUS_OK;}
pxa_status_t pxa_esp_package_store_install_file_on_volume(const char *p,uint64_t epoch,const uint8_t root[32],const char *id) {
    assert(!transaction&&!metadata);assert(epoch==77&&!strcmp(p,entry.inbox_root));
    assert(!memcmp(root,entry.publisher_key_id,32)&&!strcmp(id,entry.id));++native_calls;return native_status;
}
static void esp_fill_random(void *p,size_t n) {static unsigned seed;assert(n==4);*(uint32_t *)p=++seed;}
static int test_rename(const char *from,const char *to) {if(fail_move){errno=EIO;return -1;}return rename(from,to);}
#define rename test_rename
// PRODUCTION
static void reset(void) {native_calls=legacy_calls=cleanup_calls=refresh_calls=0;native_status=PXA_STATUS_OK;assert(!metadata&&!transaction);}
static void write_file(const char *p,const char *s) {FILE *f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
int main(int argc,char **argv) {
    assert(argc==2);assert(strlen(argv[1])<100);strcpy(store.state_root,argv[1]);
    store.metadata_lock=&metadata;store.transaction_lock=&transaction;
    strcpy(entry.id,"pxa-test");strcpy(entry.identity,"publisher:pxa-test");memset(entry.publisher_key_id,1,32);
    entry.has_publisher_key_id=entry.installed=entry.staged=1;entry.mount_epoch=77;
    snprintf(entry.inbox_root,sizeof(entry.inbox_root),"%s/candidate",argv[1]);assert(!mkdir(entry.inbox_root,0700));
    snprintf(entry.root,sizeof(entry.root),"%s/installed",argv[1]);assert(!mkdir(entry.root,0700));
    pxa_esp_package_store_deploy_result_t result;
    reset();entry.volume_local=1;assert(pxa_esp_package_store_deploy_detailed(entry.identity,&result));
    assert(native_calls==1&&!legacy_calls&&!refresh_calls&&cleanup_calls==1&&result.status==PXA_STATUS_OK);
    reset();native_status=PXA_STATUS_IO_ERROR;assert(!pxa_esp_package_store_deploy_detailed(entry.identity,&result));
    assert(native_calls==1&&!legacy_calls&&cleanup_calls==1&&result.status==PXA_STATUS_IO_ERROR);
    reset();entry.ambiguous=1;assert(!pxa_esp_package_store_deploy_detailed(entry.identity,&result));
    assert(!native_calls&&!legacy_calls&&cleanup_calls==1&&!strcmp(result.stage,"ambiguous_installation"));
    reset();entry.ambiguous=entry.volume_local=0;assert(pxa_esp_package_store_deploy_detailed(entry.identity,&result));
    assert(!native_calls&&legacy_calls==1&&refresh_calls==1&&cleanup_calls==1);
    assert(!rmdir(entry.inbox_root));write_file(entry.inbox_root,"container");
    reset();entry.volume_local=1;assert(pxa_esp_package_store_deploy_detailed(entry.identity,&result));
    assert(native_calls==1&&!legacy_calls&&cleanup_calls==1);
    pxa_posix_app_home_t selected={0};strcpy(selected.record.app_id,entry.id);memcpy(selected.record.publisher_root,entry.publisher_key_id,32);
    char original[160],save[160];strcpy(original,entry.root);snprintf(save,sizeof(save),"%s/save.bin",argv[1]);write_file(save,"original private data");
    entry.volume_local=0;entry.ambiguous=1;
    selected.record.publisher_root[0]^=1;assert(retain_legacy_duplicate(&entry,&selected)==PXA_STATUS_BAD_STATE&&is_directory(original));
    selected.record.publisher_root[0]^=1;entry.built_in=1;assert(retain_legacy_duplicate(&entry,&selected)==PXA_STATUS_BAD_STATE);entry.built_in=0;
    fail_move=1;assert(retain_legacy_duplicate(&entry,&selected)==PXA_STATUS_IO_ERROR&&is_directory(original)&&entry.ambiguous);fail_move=0;
    assert(retain_legacy_duplicate(&entry,&selected)==PXA_STATUS_OK&&!is_directory(original)&&!entry.ambiguous&&!entry.installed);
    assert(is_regular_file(save));FILE *f=fopen(save,"rb");char saved[32]={0};assert(fread(saved,1,31,f)==21&&!strcmp(saved,"original private data"));fclose(f);
    puts("Deployment routing: native directory/container updates, legacy path, failure/ambiguity cleanup and retained-duplicate identity/data preservation passed");
    return 0;
}
'''

def main():
    source=(ROOT/'firmware/components/pxa/src/package/pxa_esp_package_store.c').read_text()
    bodies='\n'.join(function(source,s) for s in (
        'static int migration_probe(', 'static pxa_status_t migration_move(void *context,',
        'static pxa_status_t retain_legacy_duplicate(', 'bool pxa_esp_package_store_deploy_detailed('))
    with tempfile.TemporaryDirectory(prefix='pxa-deploy-') as temporary:
        work=Path(temporary);unit=work/'test.c';binary=work/'test'
        unit.write_text(FIXTURE.replace('// PRODUCTION',bodies))
        subprocess.run([os.environ.get('CC','clang'),'-std=c99','-Wall','-Wextra','-Werror','-g',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-I'+str(ROOT/'firmware/components/pxa/src/package'),'-I'+str(ROOT/'deps/pxa-system/libpxa/include'),
            '-I'+str(ROOT/'deps/pxa-system/libpxa/adapters/include'),str(unit),'-o',str(binary)],check=True)
        subprocess.run([str(binary),str(work)],check=True)
if __name__=='__main__':main()
