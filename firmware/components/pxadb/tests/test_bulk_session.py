#!/usr/bin/env python3
"""Exercise the production binary receiver with slow storage and rejected frames."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include "pxadb_bulk.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace pxadb;
#define ESP_LOGW(...) ((void)0)
// BULK
struct Upload {size_t total=512,remaining=512;};
struct Session {BulkUpload bulk;Upload upload;std::atomic<uint64_t> last_activity_us{0};} session;
Session& CurrentSession() {return session;}
BulkUpload& s_bulk=session.bulk;
Upload& s_file_upload=session.upload;
std::atomic<uint64_t>& s_last_control_activity_us=session.last_activity_us;
// RESET
uint64_t now_us=1,commit_delay=0;
int commits=0,aborts=0;
bool active=false;
const char* reply=nullptr;
uint64_t esp_timer_get_time() {return now_us;}
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* data,size_t size) {
    crc=~crc;
    while(size--) {crc^=*data++;for(int i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&-(crc&1));}
    return ~crc;
}
bool FileUploadActive() {return active;}
void AbortFileUpload() {ResetBulkUpload();active=false;++aborts;}
void SendFrame(unsigned long,const char*,const char* error) {reply=error;}
void CommitFsUploadBytes(unsigned long,const uint8_t*,size_t size) {
    assert(active && size<=s_file_upload.remaining);
    now_us+=commit_delay;s_file_upload.remaining-=size;++commits;
}
// RECEIVER
std::vector<uint8_t> encode(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> result(1);size_t start=0;
    for(auto byte:raw) {
        if(byte==0 || result.size()-start==255) {
            result[start]=result.size()-start;start=result.size();result.push_back(0);
            if(byte==0)continue;
        }
        result.push_back(byte);
    }
    result[start]=result.size()-start;result.push_back(0);return result;
}
void begin(bool fast) {
    ResetBulkUpload();active=true;s_file_upload.remaining=512;reply=nullptr;
    s_bulk.bytes=(uint8_t*)malloc(1024);assert(s_bulk.bytes);
    s_bulk.capacity=1024;s_bulk.expected=512;s_bulk.fast=fast;
    s_bulk.last_us=now_us;
}
std::vector<uint8_t> frame(size_t offset,size_t size,bool good=true) {
    std::vector<uint8_t> raw(24+size,0);
    memcpy(raw.data(),"PXD1",4);raw[4]=7;
    for(unsigned i=0;i<8;++i)raw[8+i]=(uint64_t)offset>>(8*i);
    for(unsigned i=0;i<4;++i)raw[16+i]=size>>(8*i);
    auto crc=esp_rom_crc32_le(0,raw.data(),raw.size()-4)^(good?0:1);
    for(unsigned i=0;i<4;++i)raw[raw.size()-4+i]=crc>>(8*i);
    return encode(raw);
}
int main() {
    // Storage/decoder work takes longer than the 30-second session lease.
    // After either a middle or final ACK the host still gets a full idle window.
    commit_delay=31000000;
    begin(true);
    for(size_t offset:{0u,256u}) {
        auto bytes=frame(offset,256);
        assert(ConsumeFsBulk(bytes.data(),7)==7);
        assert(ConsumeFsBulk(bytes.data()+7,bytes.size()-7)==bytes.size()-7);
        assert(s_last_control_activity_us==now_us);
        assert(s_file_upload.remaining==512-offset-256);
        if(!offset)assert(s_bulk.bytes && s_bulk.last_us==now_us);
        else assert(!s_bulk.bytes);
    }
    begin(false);
    std::vector<uint8_t> raw(512,42),wire=encode(raw);
    s_bulk.crc=esp_rom_crc32_le(0,raw.data(),raw.size());
    assert(ConsumeFsBulk(wire.data(),wire.size())==wire.size());
    assert(s_last_control_activity_us==now_us && !s_bulk.bytes);
    assert(commits==3);
    for(bool wrong_crc:{true,false}) {
        begin(true);wire=frame(wrong_crc?0:1,256,!wrong_crc);
        ConsumeFsBulk(wire.data(),wire.size());
        assert(!active && !s_bulk.bytes && commits==3);
        assert(!strcmp(reply,wrong_crc?"chunk_checksum_mismatch":"offset_mismatch"));
    }
    begin(true);wire=encode({'P','X','A','D','B','1',' ','R','E','B','O','O','T','\n'});
    ConsumeFsBulk(wire.data(),wire.size());
    assert(!active && !s_bulk.bytes && commits==3);
    begin(true);uint8_t partial[]={8,'a','b'},cancel[]={0,0};
    ConsumeFsBulk(partial,sizeof(partial));
    assert(ConsumeFsBulk(cancel,sizeof(cancel))==1);
    assert(!active && !s_bulk.bytes && commits==3);
    ResetBulkUpload();
}
'''


def main():
    text = (ROOT / 'firmware/components/pxadb/src_pxa_service.cc').read_text()
    source = FIXTURE.replace('// BULK', function(text, 'struct BulkUpload') + ';')
    source = source.replace('// RESET', function(text, 'void ResetBulkUpload('))
    source = source.replace('// RECEIVER', function(text, 'size_t ConsumeFsBulk('))
    with tempfile.TemporaryDirectory(prefix='pxadb-session-') as directory:
        unit, binary = Path(directory) / 'test.cc', Path(directory) / 'test'
        unit.write_text(source)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I' + str(ROOT / 'firmware/components/pxadb/include'),
                        str(unit), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PXADB binary session: slow commits, fragmented frames, CRC/offset failures and cancellation passed')


if __name__ == '__main__':
    main()
