#!/usr/bin/env python3
"""Exercise real protocol framing against concurrent stdout/stderr writers."""
import base64
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include <mbedtls/base64.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace pxadb {
constexpr unsigned kIoTimeout=1000, configTICK_RATE_HZ=1000;
constexpr size_t kMaxEncodedPayload=2048;
constexpr int pdTRUE=1;
const char* kProtocol="PXADB1";
struct Session {
    std::atomic<bool> active{true};
    std::atomic<uint32_t> generation{1};
    int transport=0;
};
Session session;
std::mutex tx_mutex, legacy_log_mutex;
void* s_tx_mutex=&tx_mutex;
std::atomic<bool> first_piece{false}, done{false};
int xSemaphoreTake(void* p,unsigned) {static_cast<std::mutex*>(p)->lock();return pdTRUE;}
void xSemaphoreGive(void* p) {static_cast<std::mutex*>(p)->unlock();}
void esp_log_impl_lock() {legacy_log_mutex.lock();}
void esp_log_impl_unlock() {legacy_log_mutex.unlock();}
Session& CurrentSession() {return session;}
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void vTaskDelay(unsigned) {std::this_thread::yield();}
size_t TransportWriteChunk(int) {return 256;}
int TransportWrite(int,const uint8_t* data,size_t size) {
    size=std::min<size_t>(size,13);
    const auto result=write(STDOUT_FILENO,data,size);
    first_piece=true;
    std::this_thread::sleep_for(std::chrono::microseconds(5));
    return static_cast<int>(result);
}
bool TransportTxDone(int) {return true;}
// FRAMING
}

int main() {
    using namespace pxadb;
    setvbuf(stdout,nullptr,_IONBF,0);
    setvbuf(stderr,nullptr,_IONBF,0);
    auto logger=[](FILE* stream,char label) {
        while(!first_piece.load())std::this_thread::yield();
        unsigned n=0;
        do {
            // Like IDF vprintf, this takes the FILE lock without taking the
            // tag-level legacy mutex used by the negative control.
            fprintf(stream,"RAW%c:%u:concurrent-console-output\n",label,n++);
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        } while(!done.load());
    };
    std::thread out(logger,stdout,'O'),err(logger,stderr,'E');
    const std::string payload(768,'x');
    for(unsigned n=1;n<=64;++n) {
        if(!SendFrame(n,"DATA",payload.c_str()))return 2;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    done=true;out.join();err.join();
    // A rejected late reply must release both FILE locks as well.
    if(SendFrameTo(session,65,"OK","late",2))return 3;
#if CONFIG_LIBC_NEWLIB
    if(ftrylockfile(stdout))return 4;
    funlockfile(stdout);
    if(ftrylockfile(stderr))return 5;
    funlockfile(stderr);
#endif
}
'''


def validate(output):
    found = []
    for line in output.splitlines():
        if not line.startswith(b'PXADB1 '):
            continue
        try:
            protocol, sequence, kind, payload = line.split(b' ', 3)
            decoded = base64.b64decode(payload, validate=True)
            if protocol == b'PXADB1' and kind == b'DATA' and decoded == b'x' * 768:
                found.append(int(sequence))
        except (ValueError, TypeError):
            pass
    return found == list(range(1, 65))


def main():
    production = (ROOT / 'firmware/components/pxadb/src_pxa_service.cc').read_text()
    source = FIXTURE.replace('// FRAMING', '\n'.join(
        function(production, name) for name in
        ('bool WriteAll(', 'bool SendFrameTo(', 'bool SendFrame(')))
    with tempfile.TemporaryDirectory(prefix='pxadb-stdio-') as temporary:
        path = Path(temporary)
        (path / 'test.cc').write_text(source)
        for newlib in (0, 1):
            binary = path / f'test-{newlib}'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1',
                            '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            f'-DCONFIG_LIBC_NEWLIB={newlib}', str(path / 'test.cc'),
                            '-lmbedcrypto', '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, check=True, timeout=30)
            assert b'RAWO:' in result.stdout and b'RAWE:' in result.stdout
            assert validate(result.stdout) == bool(newlib), (
                'FILE locks must prevent the interleaving reproduced by the legacy mutex', newlib)
    print('PXADB framing: concurrent stdout/stderr, partial writes and late-reply lock release passed; legacy mutex negative control reproduced corruption')


if __name__ == '__main__':
    main()
