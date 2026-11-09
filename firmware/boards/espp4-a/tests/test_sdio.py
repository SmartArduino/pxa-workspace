#!/usr/bin/env python3
"""Exercise production radio DMA adaptation with constrained internal memory."""
import os
from pathlib import Path
import subprocess
import tempfile

STUB = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_INVALID_SIZE=2, ESP_ERR_NO_MEM=257;
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_DMA=2,MALLOC_CAP_8BIT=4,MALLOC_CAP_SPIRAM=8;
struct sdmmc_card_t {struct {int slot;bool(*check_buffer_alignment)(int,const void*,size_t);}host;};
inline std::map<void*,int> allocations;
inline bool fail_internal=false,fail_external=false,external_supported=true;
inline int allocated=0,freed=0;
inline void* heap_caps_aligned_alloc(size_t alignment,size_t size,int caps){
 if((caps&MALLOC_CAP_INTERNAL && fail_internal) || (caps&MALLOC_CAP_SPIRAM && fail_external))return nullptr;
 void* p=nullptr;assert(posix_memalign(&p,alignment,size)==0);allocations[p]=caps;++allocated;return p;
}
inline void heap_caps_free(void* p){assert(allocations.erase(p)==1);++freed;free(p);}
inline bool alignment(int,const void* p,size_t n){
 auto found=allocations.find(const_cast<void*>(p));
 return found!=allocations.end() && (!(found->second&MALLOC_CAP_SPIRAM) || (external_supported && n%128==0));
}
'''

TEST = r'''
#include "espp4_a_sdio.cc"
bool external_used=false;
int calls=0,result=ESP_OK;
uint32_t function_seen=0,address_seen=0;
size_t size_seen=0;
int transfer(bool write,uint32_t function,uint32_t address,void* data,size_t size){
 ++calls;function_seen=function;address_seen=address;size_seen=size;
 auto found=allocations.find(data);external_used=found!=allocations.end() && found->second&MALLOC_CAP_SPIRAM;
 if(write)for(size_t i=0;i<size;++i)assert(static_cast<uint8_t*>(data)[i]==0x39);
 else if(result==ESP_OK)std::memset(data,0x5a,size);
 return result;
}
extern "C" int __real_sdmmc_io_read_bytes(sdmmc_card_t*,uint32_t f,uint32_t a,void* p,size_t n){return transfer(false,f,a,p,n);}
extern "C" int __real_sdmmc_io_read_blocks(sdmmc_card_t*,uint32_t f,uint32_t a,void* p,size_t n){return transfer(false,f,a,p,n);}
extern "C" int __real_sdmmc_io_write_bytes(sdmmc_card_t*,uint32_t f,uint32_t a,const void* p,size_t n){return transfer(true,f,a,const_cast<void*>(p),n);}
extern "C" int __real_sdmmc_io_write_blocks(sdmmc_card_t*,uint32_t f,uint32_t a,const void* p,size_t n){return transfer(true,f,a,const_cast<void*>(p),n);}
int main(){
 sdmmc_card_t radio{{1,alignment}},sd{{0,alignment}};
 std::vector<uint8_t> data(32768,0x39);
 fail_internal=true; // The TLS handshake owns the remaining internal RAM.
 assert(__wrap_sdmmc_io_read_blocks(&radio,1,0x80001000,data.data(),data.size())==ESP_OK);
 assert(external_used && calls==1 && function_seen==1 && address_seen==0x80001000 && size_seen==32768);
 for(auto byte:data)assert(byte==0x5a);
 std::fill(data.begin(),data.end(),0x39);
 assert(__wrap_sdmmc_io_write_blocks(&radio,2,0x2000,data.data(),data.size())==ESP_OK && external_used);
 fail_internal=false;fail_external=true;
 assert(__wrap_sdmmc_io_read_blocks(&radio,1,0,data.data(),512)==ESP_OK && !external_used);
 fail_external=false;external_supported=false;
 assert(__wrap_sdmmc_io_read_blocks(&radio,1,0,data.data(),512)==ESP_OK && !external_used);
 external_supported=true;
 std::fill(data.begin(),data.end(),0x39);
 assert(__wrap_sdmmc_io_write_bytes(&radio,1,3,data.data(),17)==ESP_OK && !external_used);
 assert(size_seen==17 && address_seen==3); // Never pad or split the actual transaction.
 int prior=allocated;
 assert(__wrap_sdmmc_io_read_blocks(&sd,1,0,data.data(),512)==ESP_OK && allocated==prior);
 void* direct=heap_caps_aligned_alloc(128,512,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA);
 prior=allocated;assert(__wrap_sdmmc_io_read_blocks(&radio,1,0,direct,512)==ESP_OK && allocated==prior);
 heap_caps_free(direct);
 std::fill(data.begin(),data.end(),0x39);result=99;
 assert(__wrap_sdmmc_io_read_blocks(&radio,1,0,data.data(),512)==99);
 for(size_t i=0;i<512;++i)assert(data[i]==0x39); // Failed reads leave caller data intact.
 result=ESP_OK;fail_internal=fail_external=true;prior=calls;
 assert(__wrap_sdmmc_io_read_blocks(&radio,1,0,data.data(),512)==ESP_ERR_NO_MEM && calls==prior);
 assert(__wrap_sdmmc_io_read_blocks(nullptr,1,0,data.data(),512)==ESP_ERR_INVALID_ARG);
 assert(__wrap_sdmmc_io_read_blocks(&radio,1,0,data.data(),SIZE_MAX)==ESP_ERR_INVALID_SIZE);
 assert(allocations.empty() && allocated==freed);
 puts("SDIO: TLS memory pressure, external DMA validation, fallback, bypass, byte counts, fixed addresses, errors and cleanup passed");
}
'''


def main():
    board = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='pxa-sdio-') as temp:
        out = Path(temp)
        for header in ['sdmmc_cmd.h', 'esp_heap_caps.h']:
            (out / header).write_text(STUB)
        # Both platform includes share a single fake definition set.
        (out / 'esp_heap_caps.h').write_text('#include "sdmmc_cmd.h"\n')
        (out / 'test.cpp').write_text(TEST)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17',
                        '-fsanitize=address,undefined', '-I', str(out),
                        '-I', str(board / 'src'), str(out / 'test.cpp'),
                        '-o', str(out / 'test')], check=True)
        subprocess.run([str(out / 'test')], check=True)


if __name__ == '__main__':
    main()
