#!/usr/bin/env python3
"""Exercise the production optional-primary mount policy without a board."""
import os
from pathlib import Path
import subprocess
import tempfile

from test_startup import function

FIXTURE = r'''
#include <cassert>
#include <cstddef>
#include <cstring>
#include <cstdio>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1;
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_DATA_LITTLEFS=2;
const char* label="assets";
#define CONFIG_PXA_STORAGE_PARTITION_LABEL label
#define CONFIG_PXA_MOUNT_POINT "/pxa"
#define kTag "mount-test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct esp_partition_t {} partition;
struct esp_vfs_littlefs_conf_t {
    const char* base_path;
    const char* partition_label;
    const esp_partition_t* partition;
    bool format_if_mount_failed,read_only,dont_mount,grow_on_mount;
};
bool present=true,mounted=false;
unsigned probes=0,attempts=0,registrations=0;
int result=ESP_OK;
const esp_partition_t* esp_partition_find_first(int type,int subtype,const char* name) {
    assert(type==ESP_PARTITION_TYPE_DATA && subtype==ESP_PARTITION_SUBTYPE_DATA_LITTLEFS);
    assert(!strcmp(name,"assets"));++probes;return present?&partition:nullptr;
}
bool esp_littlefs_mounted(const char* name) { assert(!strcmp(name,"assets"));return mounted; }
int esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t* config) {
    ++attempts;assert(!strcmp(config->base_path,"/pxa") && !strcmp(config->partition_label,"assets"));
    assert(!config->partition && !config->format_if_mount_failed && !config->read_only && !config->dont_mount && !config->grow_on_mount);
    return result;
}
void RegisterPrimaryVolume() { ++registrations; }
// PRODUCTION_MOUNT
int main() {
    label="";assert(MountPxaStorage() && !probes && !attempts && !registrations);
    label="assets";present=false;assert(MountPxaStorage() && probes==1 && !attempts && !registrations);
    present=true;result=ESP_FAIL;assert(MountPxaStorage() && attempts==1 && !registrations);
    result=ESP_ERR_INVALID_STATE;assert(MountPxaStorage() && attempts==2 && !registrations);
    mounted=true;assert(MountPxaStorage() && attempts==3 && registrations==1);
    result=ESP_OK;mounted=false;assert(MountPxaStorage() && attempts==4 && registrations==2);
    puts("Optional primary storage: disabled/missing/damaged volumes preserve external startup; no autoformat or false registration");
}
'''

def main():
    directory=Path(__file__).resolve().parent
    source=FIXTURE.replace('// PRODUCTION_MOUNT',function((directory.parent/'pxa_integration.cc').read_text(),'bool MountPxaStorage('))
    with tempfile.TemporaryDirectory(prefix='pxa-storage-startup-') as temporary:
        root=Path(temporary);unit=root/'startup.cpp';output=root/'startup'
        unit.write_text(source)
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++20','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(unit),'-o',str(output)],check=True)
        subprocess.run([str(output)],check=True)

if __name__=='__main__':
    main()
