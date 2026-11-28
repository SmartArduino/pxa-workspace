#!/usr/bin/env python3
"""Check UART byte delivery, timeout ownership and native USB isolation."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#define CONFIG_PXADB_TRANSPORT_UART 0
#define CONFIG_PXADB_UART_SECONDARY 1
#define CONFIG_PXADB_TRANSPORT_USB_CDC 1
#define TINYUSB_CDC_ACM_0 0
#define STDOUT_FILENO 1
namespace pxadb {
enum class Transport {kPrimary,kUart,kLan};
constexpr int kUart=0,kIoTimeout=1000,ESP_OK=0;
bool kUseConsoleVfs=false;
std::string uart,usb;
std::vector<char> events;
size_t short_limit=10000;
int drain_result=ESP_OK;
unsigned delay_us=0;
int write(int fd,const uint8_t* bytes,size_t count) {
    assert(fd==STDOUT_FILENO);
    events.push_back('C');uart.append(reinterpret_cast<const char*>(bytes),count);
    return static_cast<int>(count);
}
int uart_wait_tx_done(int,int) {events.push_back('D');return drain_result;}
void esp_rom_delay_us(unsigned n) {events.push_back('G');delay_us+=n;}
int uart_write_bytes(int,const uint8_t* bytes,size_t count) {
    events.push_back('W');count=std::min(count,short_limit);
    uart.append(reinterpret_cast<const char*>(bytes),count);return static_cast<int>(count);
}
size_t tinyusb_cdcacm_write_queue(int,const uint8_t* bytes,size_t count) {
    events.push_back('U');usb.append(reinterpret_cast<const char*>(bytes),count);return count;
}
int tinyusb_cdcacm_write_flush(int,int) {events.push_back('F');return ESP_OK;}
// TRANSPORT
}
int main() {
    using namespace pxadb;
    const std::string source(517,'p');
    const auto* bytes=reinterpret_cast<const uint8_t*>(source.data());
    assert(TransportWrite(Transport::kUart,bytes,0)==0&&events.empty());
    for(size_t offset=0;offset<source.size();) {
        events.clear();int n=TransportWrite(Transport::kUart,bytes+offset,source.size()-offset);
        assert(n>0);
#if CONFIG_PXADB_UART_TX_BURST_BYTES > 0
        assert(n<=CONFIG_PXADB_UART_TX_BURST_BYTES);
#if CONFIG_PXADB_UART_TX_GAP_US > 0
        assert((events==std::vector<char>{'D','G','W'}));
#else
        assert((events==std::vector<char>{'D','W'}));
#endif
#else
        assert((events==std::vector<char>{'W'}));
#endif
        offset+=n;
    }
    assert(uart==source);
    // Partial driver writes must report only the bytes accepted, without replay.
    uart.clear();short_limit=7;
    for(size_t offset=0;offset<source.size();) {
        int n=TransportWrite(Transport::kUart,bytes+offset,source.size()-offset);
        assert(n>0&&n<=7);offset+=n;
    }
    assert(uart==source);
#if CONFIG_PXADB_UART_TX_BURST_BYTES > 0
    // A drain timeout occurs before any bytes from this call are accepted.
    const auto accepted=uart;drain_result=-1;events.clear();
    assert(TransportWrite(Transport::kUart,bytes,source.size())==-1);
    assert(uart==accepted&&(events==std::vector<char>{'D'}));
#endif
    // Native USB keeps its original continuous writer even if UART is stalled.
    const auto previous_delay=delay_us;events.clear();
    assert(TransportWrite(Transport::kPrimary,bytes,source.size())==static_cast<int>(source.size()));
    assert(usb==source&&delay_us==previous_delay&&(events==std::vector<char>{'U','F'}));
    // The console lock owns a whole frame, independent of UART burst tuning.
    kUseConsoleVfs=true;
    uart.clear();events.clear();
    assert(TransportWriteChunk(Transport::kUart)>=4096);
    assert(TransportWriteChunk(Transport::kPrimary)==256);
    assert(TransportWrite(Transport::kUart,bytes,source.size())==static_cast<int>(source.size()));
    assert(uart==source&&(events==std::vector<char>{'C'}));
}
'''


def main():
    production = (ROOT / 'firmware/components/pxadb/pxadb_transport.cc').read_text()
    source = FIXTURE.replace('// TRANSPORT', '\n'.join(
        function(production, name) for name in
        ('bool TransportIsUart(', 'int TransportWrite(', 'size_t TransportWriteChunk(')))
    with tempfile.TemporaryDirectory(prefix='pxadb-uart-pacing-') as temporary:
        path = Path(temporary)
        unit, binary = path / 'test.cc', path / 'test'
        unit.write_text(source)
        for burst, gap in ((0, 0), (32, 0), (32, 100)):
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1',
                            '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            f'-DCONFIG_PXADB_UART_TX_BURST_BYTES={burst}',
                            f'-DCONFIG_PXADB_UART_TX_GAP_US={gap}',
                            str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
    print('PXADB UART pacing: complete/partial byte delivery, pre-write drain timeout, disabled pacing and independent native USB passed')


if __name__ == '__main__':
    main()
