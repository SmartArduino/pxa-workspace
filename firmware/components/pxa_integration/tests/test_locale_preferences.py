#!/usr/bin/env python3
"""Exercise production preference callbacks with the actual locale service.

NVS is the boundary: committed values survive simulated process/power loss;
RAM and service observers are recreated without a timer tick or shutdown save.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from test_startup import function


FIXTURE = r'''
#include "pxsys/locale.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1;
constexpr int PXSYS_COLOR_SCHEME_DARK=1,PXSYS_THEME_PALETTE_COUNT=8;
using nvs_handle_t=int;using esp_err_t=int;
struct pxsys_reference_idle_policy_t {uint16_t dim_after_seconds,lock_after_seconds;uint8_t dim_percent;};
static std::map<std::string,std::string> durable,staged;
static unsigned commits;
static bool fail_commit,fail_open,unterminated;
int nvs_open(const char*,int mode,int* handle) {
    if((mode==NVS_READWRITE&&fail_open)||(mode==NVS_READONLY&&durable.empty()))return -1;
    *handle=mode+1;return ESP_OK;
}
void nvs_close(int) {staged.clear();}
int nvs_get_u8(int,const char*,uint8_t*) {return -1;}
int nvs_get_u16(int,const char*,uint16_t*) {return -1;}
int nvs_get_str(int,const char* key,char* out,size_t* size) {
    auto it=durable.find(key);if(it==durable.end())return -1;
    size_t needed=it->second.size()+1,capacity=*size;*size=needed;
    if(capacity<needed)return -1;
    std::memcpy(out,it->second.c_str(),needed);
    if(unterminated)out[needed-1]='X';return ESP_OK;
}
int nvs_set_str(int,const char* key,const char* value) {staged[key]=value;return ESP_OK;}
int nvs_commit(int) {
    if(fail_commit)return -1;
    for(auto& item:staged)durable[item.first]=item.second;
    ++commits;return ESP_OK;
}
// PREFERENCES
static UserPreferences g_preferences;
static bool g_locale_observer_seen;
static const char* kPreferencesNamespace="pxa_user";
// PRODUCTION
static pxsys_locale_service_t* service;
static void* allocate(void*,size_t size) {return std::malloc(size);}
static void release(void*,void* memory) {std::free(memory);}
static void boot(const char* expected,const char* fallback="zh-CN") {
    // A fresh process loses every previous in-memory preference and timer.
    assert(service==nullptr);g_preferences={};staged.clear();
    LoadUserPreferences();pxsys_locale_snapshot_t initial;
    assert(!pxsys_locale_snapshot_init(&initial,pxsys_string_from_cstr(InitialLocale(fallback))));
    assert(!std::strcmp(initial.tag,expected));
    pxsys_locale_service_config_t config;pxsys_locale_service_config_init(&config);
    config.allocator={sizeof(config.allocator),nullptr,allocate,release};
    assert(!pxsys_locale_service_create(&config,&initial,&service));
    assert(!pxsys_locale_service_subscribe(service,nullptr,SaveLocale));
}
static void off() {
    assert(!pxsys_locale_service_unsubscribe(service,nullptr,SaveLocale));
    assert(!pxsys_locale_service_destroy(service));service=nullptr;
}
static void choose(const char* tag) {
    pxsys_locale_snapshot_t value;
    assert(!pxsys_locale_snapshot_init(&value,pxsys_string_from_cstr(tag)));
    assert(!pxsys_locale_service_update(service,&value));
}
int main() {
    boot("zh-CN");assert(!commits);
    choose("en-US");assert(commits==1&&durable.at("locale")=="en-US");
    off();boot("en-US");assert(commits==1); // Immediate power loss; no idle flush.
    choose("en-US");assert(commits==1);
    choose("zh-CN");assert(commits==2);off();boot("zh-CN");assert(commits==2);off();
    durable["locale"]="en-us";boot("en-US");assert(commits==2);off();
    for(const auto& invalid:{std::string(""),std::string("en_US"),std::string(100,'x'),std::string("en-US\0junk",10)}) {
        durable["locale"]=invalid;boot("zh-CN");assert(!g_preferences.has_locale&&commits==2);off();
    }
    durable["locale"]="en-US";unterminated=true;boot("zh-CN");off();unterminated=false;
    const char* longest="en-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdef";
    assert(std::strlen(longest)==PXSYS_LOCALE_TAG_MAX_BYTES);
    durable["locale"]=longest;boot(longest);off();
    durable.clear();boot("en-US","en-US");assert(commits==2);off();
    boot("zh-CN");fail_commit=true;choose("en-US");assert(!g_preferences.has_locale&&commits==2);
    fail_commit=false;choose("zh-CN");choose("en-US");assert(commits==4);off();boot("en-US");off();
    durable.clear();boot("zh-CN");fail_open=true;choose("en-US");assert(!g_preferences.has_locale);
    fail_open=false;off();boot("zh-CN");off();
    puts("Locale preferences: first-change commit, fresh-boot restore, canonical/bounded tags, duplicate suppression and NVS errors passed");
}
'''


def main():
    directory = Path(__file__).resolve().parent
    root = directory.parents[3]
    production = (directory.parent / 'pxa_integration.cc').read_text()
    unit = FIXTURE.replace('// PREFERENCES', function(production, 'struct UserPreferences') + ';')
    unit = unit.replace('// PRODUCTION', '\n'.join(function(production, signature) for signature in (
        'void LoadUserPreferences(', 'const char* InitialLocale(', 'void SaveLocale(')))
    include = root / 'deps/pxa-system/system/core/include'
    with tempfile.TemporaryDirectory(prefix='pxa-locale-preferences-') as temporary:
        work = Path(temporary)
        source = work / 'test.cpp'
        source.write_text(unit)
        objects = []
        for name in ('locale', 'types'):
            obj = work / (name + '.o')
            objects.append(str(obj))
            subprocess.run([os.environ.get('CC', 'clang'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(include), '-c',
                            str(root / ('deps/pxa-system/system/core/src/' + name + '.c')), '-o', str(obj)], check=True)
        binary = work / 'test'
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I' + str(include), str(source), *objects, '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
