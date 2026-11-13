#!/usr/bin/env python3
"""Run the production parser/dispatcher, leases, framing and log fanout on two fake links.

Only device/RTOS boundaries are replaced. The test uses real mbedTLS Base64,
the real bulk decoder and production session/input ownership functions.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include "pxadb_transport.h"
#include "pxadb_input_mailbox.h"
#include "pxadb_bulk.h"
#include <mbedtls/base64.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <inttypes.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#define CONFIG_PXA_ENABLED 0
#define CONFIG_PXADB_TEST_CONTROL 1
#define CONFIG_PXADB_UART_SECONDARY 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(p) ((p)->lock())
#define portEXIT_CRITICAL(p) ((p)->unlock())
#define pdMS_TO_TICKS(n) (n)

namespace pxadb {
constexpr size_t kMaxCommandLength=2048,kMaxFramePayload=320,kMaxEncodedPayload=2048;
constexpr size_t kMaxLogFramePayload=332,kLogQueueDepth=4,kLogHistoryDepth=16,kLogScratchCount=2;
constexpr size_t kFsAbsolutePathLength=256;
constexpr unsigned kSessionIdleTimeoutMs=30000,kIoTimeout=1000,configTICK_RATE_HZ=1000;
const char* kProtocol="PXADB1";
const char* kFsUploadTemporary="TEMP_ROOT/orphan.partial";
using QueueHandle_t=void*;
using vprintf_like_t=int(*)(const char*,va_list);
using TickType_t=unsigned;
constexpr int pdTRUE=1;
constexpr int ESP_OK=0;
uint64_t now_us=100;
int64_t esp_timer_get_time() {return static_cast<int64_t>(now_us);}
unsigned esp_log_timestamp() {return unsigned(now_us/1000);}
void vTaskDelay(unsigned) {std::this_thread::yield();}
std::mutex tx_lock,log_lock;
void* s_tx_mutex=&tx_lock;
std::atomic<bool> tx_waiting{false};
int xSemaphoreTake(void* p,unsigned) {tx_waiting=true;static_cast<std::mutex*>(p)->lock();return pdTRUE;}
void xSemaphoreGive(void* p) {if(p==s_tx_mutex)static_cast<std::mutex*>(p)->unlock();}
void esp_log_impl_lock() {log_lock.lock();}
void esp_log_impl_unlock() {log_lock.unlock();}
int raw_log(const char*,va_list) {return 0;}
vprintf_like_t active_log=raw_log;
std::atomic<vprintf_like_t> s_previous_vprintf{nullptr};
bool s_log_hook_installed=false;
std::atomic<unsigned> s_log_writers{0};
vprintf_like_t esp_log_set_vprintf(vprintf_like_t value) {auto old=active_log;active_log=value;return old;}

// TYPES
// SESSION
struct Queue {std::mutex lock;std::deque<LogRecord> rows;};
std::mutex s_history_lock,s_log_scratch_lock,s_input_lock;
LogRecord retained[kLogHistoryDepth];
LogRecord* s_log_history=retained;
size_t s_log_history_start=0,s_log_history_count=0;
LogScratch s_log_scratch[kLogScratchCount];
int xQueueSend(void* p,const LogRecord* record,unsigned) {
    auto& q=*static_cast<Queue*>(p);std::lock_guard guard(q.lock);
    if(q.rows.size()==kLogQueueDepth)return 0;
    q.rows.push_back(*record);return pdTRUE;
}
int xQueueReceive(void* p,LogRecord* record,unsigned) {
    auto& q=*static_cast<Queue*>(p);std::lock_guard guard(q.lock);
    if(q.rows.empty())return 0;
    *record=q.rows.front();q.rows.pop_front();return pdTRUE;
}
void xQueueReset(void* p) {auto& q=*static_cast<Queue*>(p);std::lock_guard guard(q.lock);q.rows.clear();}
std::deque<uint8_t> incoming[2];
std::string outgoing[2];
int TransportRead(Transport transport,uint8_t* bytes,size_t capacity,uint32_t) {
    auto& q=incoming[unsigned(transport)];size_t count=0;
    while(count<capacity&&!q.empty()){bytes[count++]=q.front();q.pop_front();}
    return int(count);
}
int TransportWrite(Transport transport,const uint8_t* bytes,size_t count) {
    // Deliberate partial writes exercise the production bounded frame loop.
    count=std::min<size_t>(count,13);outgoing[unsigned(transport)].append(reinterpret_cast<const char*>(bytes),count);
    return int(count);
}
bool TransportTxDone(Transport) {return true;}
size_t TransportWriteChunk(Transport) {return 256;}
const char* TransportName(Transport t) {return t==Transport::kPrimary?"usb":"uart";}
std::atomic<bool> s_package_cleanup_pending{false};
int64_t s_next_package_cleanup_us=0;
InputMailbox<16> s_input_mailbox;
PointerState s_accepted_pointer;
uint8_t s_input_owner=kNoSession;
std::atomic<bool> s_input_cancel_pending{false},s_running{true};
std::atomic<unsigned> s_input_cancel_callers{0};
void* s_input_signal=nullptr;
struct {uint8_t max_pointers=2;} s_test_control;
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* data,size_t count) {
    crc=~crc;while(count--){crc^=*data++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&-(crc&1));}return ~crc;
}

// FRAMING
// TRANSFERS
// LOGS
// INPUT
// RESET

unsigned starts=0,reboots=0;
void SendHello(unsigned long n){SendFrame(n,"OK","protocol=1;mode=normal");}
void SendInfo(unsigned long n){SendFrame(n,"OK","pxa=ready");}
void StartFsUpload(unsigned long n,const char* path,const char*,const char*) {
    ++starts;AbortFileUpload();CurrentSession().upload.file=tmpfile();assert(CurrentSession().upload.file);
    CurrentSession().upload.total=CurrentSession().upload.remaining=4;
    if(!strcmp(path,"inbox"))CurrentSession().package_staging=true;
    SendFrame(n,"READY","4");
}
void CommitFsUploadBytes(unsigned long n,const uint8_t* bytes,size_t count) {
    auto& u=CurrentSession().upload;assert(u.file&&count<=u.remaining);
    assert(fwrite(bytes,1,count,u.file)==count);u.remaining-=count;
    if(!u.remaining){fclose(u.file);u.file=nullptr;SendFrame(n,"OK","");}
    else SendFrame(n,"READY","2");
}
void WriteFsUpload(unsigned long n,const char*,const char*,const char* =nullptr) {
    const uint8_t bytes[]={1,2};CommitFsUploadBytes(n,bytes,sizeof(bytes));
}
// BULK
void SendMemory(unsigned long n,bool=true){SendFrame(n,"OK","");}
int heap_caps_monitor_local_minimum_free_size_start(){return ESP_OK;}
int heap_caps_monitor_local_minimum_free_size_stop(){return ESP_OK;}
void esp_restart(){++reboots;}
bool PowerControlAvailable(){return false;}
struct {void* context=nullptr;bool (*power_off)(void*)=nullptr;} s_power_control;
#define STUB(name) template<class... T> void name(unsigned long n,T...){SendFrame(n,"OK","");}
STUB(SendInteractionInfo) STUB(SendRuntimeInfo) STUB(HandleVolumes) STUB(SendUiInspection)
STUB(SendPackages) STUB(HandlePerf) STUB(HandlePermissions) STUB(HandleInput) STUB(SendJpegSelfTest)
STUB(SendScreenshot) STUB(SendFsList) STUB(SendFsFile) STUB(SendFsRead) STUB(SendFsDigest)
STUB(SendNetworkInfo) STUB(StartFsBulk) STUB(StartFsFast) STUB(MakeFsDirectory)
STUB(MoveExternalFile) STUB(RemoveFsPath) STUB(RemoveFsTree) STUB(HandlePackageInstallVolume)
STUB(HandlePackageCacheImport) STUB(HandlePackageMoveVolume) STUB(HandlePackagePaths)
STUB(HandlePackageMoveData) STUB(HandlePackageDeploy) STUB(HandlePackageRun)
STUB(HandlePackageStop) STUB(HandlePackageAction) STUB(SendLogHistory)
#undef STUB
// DISPATCH
// LOG_DRAIN
// POLL

void feed(unsigned endpoint,const std::string& bytes) {
    incoming[endpoint].insert(incoming[endpoint].end(),bytes.begin(),bytes.end());
    while(!incoming[endpoint].empty())PollControlSession(s_sessions[endpoint]);
}
void request(unsigned endpoint,unsigned sequence,const char* command) {
    feed(endpoint,"PXADB1 "+std::to_string(sequence)+" "+command+"\n");
}
bool has(unsigned endpoint,unsigned sequence,const char* kind) {
    return outgoing[endpoint].find("PXADB1 "+std::to_string(sequence)+" "+kind+" ")!=std::string::npos;
}
std::vector<uint8_t> encode(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out(1);size_t at=0;
    for(auto byte:raw){if(!byte||out.size()-at==255){out[at]=out.size()-at;at=out.size();out.push_back(0);if(!byte)continue;}out.push_back(byte);}
    out[at]=out.size()-at;out.push_back(0);return out;
}
void emit(const char* format,...) {va_list args;va_start(args,format);PxaLogVprintf(format,args);va_end(args);}
} // namespace pxadb

int main() {
    using namespace pxadb;
    ControlParser parsers[2];Queue queues[2];
    for(unsigned i=0;i<2;++i){s_sessions[i].enabled=true;s_sessions[i].parser=&parsers[i];s_sessions[i].transport=Transport(i);s_sessions[i].log_queue=&queues[i];}
    // Without HELLO there is no active idle timer to release an upload lease.
    request(1,10,"FSPUT normal 4");assert(has(1,10,"ERR")&&!starts&&s_mutation_owner==kNoSession);
    // Fragments/overlong input on USB cannot consume a UART command or vice versa.
    feed(0,"PXADB1 11 PI");request(1,12,"HELLO");assert(has(1,12,"OK")&&!has(0,12,"OK"));
    feed(0,"NG\n");assert(has(0,11,"OK")&&!has(1,11,"OK"));
    request(0,13,"HELLO");feed(0,std::string(kMaxCommandLength+5,'x'));
    request(1,14,"PING");feed(0,"\n");assert(has(1,14,"OK"));
    request(0,15,"FSUNLOCK 1");assert(s_sessions[0].unlock_expiry_us&& !s_sessions[1].unlock_expiry_us);
    request(1,16,"FSUNLOCK 2");request(0,17,"FSLOCK");assert(!s_sessions[0].unlock_expiry_us&&s_sessions[1].unlock_expiry_us);

    request(0,20,"FSPUT normal 4");FILE* original=s_sessions[0].upload.file;assert(original&&s_mutation_owner==0);
    request(1,21,"HELLO");request(1,22,"FSABORT");request(1,23,"FSPUT normal 4");
    assert(s_sessions[0].upload.file==original&&s_mutation_owner==0&&starts==1&&has(1,23,"ERR"));
    request(1,24,"PING");assert(has(1,24,"OK"));
    request(0,25,"FSDATA x");request(1,26,"BYE");request(0,27,"FSDATA x");
    assert(!s_sessions[0].upload.file&&s_mutation_owner==kNoSession);
    request(1,28,"HELLO");request(1,29,"FSPUT normal 4");
    request(0,30,"BYE");assert(s_sessions[1].upload.file&&s_mutation_owner==1);
    request(1,31,"FSABORT");assert(!s_sessions[1].upload.file&&s_mutation_owner==kNoSession);

    request(0,40,"HELLO");request(0,41,"FSPUT inbox 4");request(0,42,"FSDATA x");request(0,43,"FSDATA x");
    assert(s_sessions[0].package_staging&&s_mutation_owner==0);
    request(1,44,"FSPUT normal 4");assert(has(1,44,"ERR"));
    request(0,45,"PACKAGE deploy demo");assert(s_mutation_owner==kNoSession);
    request(1,46,"FSPUT normal 4");assert(has(1,46,"READY"));request(1,47,"FSABORT");

    // A half-delivered binary body survives activity/reconnection on the other link.
    request(0,50,"FSPUT normal 4");s_current_session=&s_sessions[0];
    auto& bulk=CurrentSession().bulk;std::vector<uint8_t> payload={10,20,30,40},wire=encode(payload);
    bulk.bytes=static_cast<uint8_t*>(malloc(64));bulk.capacity=64;bulk.expected=4;bulk.sequence=51;bulk.crc=esp_rom_crc32_le(0,payload.data(),payload.size());
    feed(0,std::string(reinterpret_cast<char*>(wire.data()),2));request(1,52,"HELLO");
    assert(s_sessions[0].bulk.used==2&&s_mutation_owner==0);
    feed(0,std::string(reinterpret_cast<char*>(wire.data()+2),wire.size()-2));assert(has(0,51,"OK")&&!s_sessions[0].bulk.bytes&&s_mutation_owner==kNoSession);

    // Input queue ownership and delayed ACKs are bound to their original endpoint/generation.
    InputCommand input;input.kind=InputCommandKind::kPointer;input.pointer_action=PointerAction::kDown;
    input.session_id=0;input.session_generation=s_sessions[0].generation;input.request_sequence=60;input.event_sequence=1;
    const char* error=nullptr;assert(QueueInput(input,&error));
    request(1,61,"BYE");assert(s_input_owner==0&&!s_input_cancel_pending&&s_accepted_pointer.pressed());
    request(1,62,"HELLO");s_current_session=&s_sessions[1];SendInputAck(input,"processed");assert(has(0,60,"OK")&&!has(1,60,"OK"));
    auto second=input;second.session_id=1;second.session_generation=s_sessions[1].generation;
    assert(!QueueInput(second,&error)&&!strcmp(error,"input_session_busy"));
    request(0,63,"HELLO");assert(s_input_cancel_pending);outgoing[0].clear();SendInputAck(input,"processed");assert(!has(0,60,"OK"));
    s_input_mailbox.Clear();s_accepted_pointer.Reset();s_input_owner=kNoSession;s_input_cancel_pending=false;
    // Generation is checked again after a response obtains the transmit lock.
    second.kind=InputCommandKind::kSync;second.request_sequence=64;
    tx_lock.lock();tx_waiting=false;std::thread late([&]{SendInputAck(second,"processed");});
    while(!tx_waiting.load())std::this_thread::yield();
    s_sessions[1].generation.fetch_add(1);tx_lock.unlock();late.join();assert(!has(1,64,"OK"));

    request(0,70,"LOGSUB");request(1,71,"LOGSUB");emit("both");
    assert(queues[0].rows.size()==1&&queues[1].rows.size()==1);
    request(0,72,"LOGUNSUB");assert(s_log_hook_installed&&s_sessions[1].log_subscribed);
    xQueueReset(&queues[0]);xQueueReset(&queues[1]);
    std::thread producer([]{for(unsigned n=0;n<1000;++n)emit("entry-%u",n);});
    for(unsigned i=0;i<50;++i){s_current_session=&s_sessions[i%2];std::this_thread::yield();}
    producer.join();assert(queues[0].rows.empty()&&queues[1].rows.size()==kLogQueueDepth&&s_sessions[1].dropped_logs>0);
    s_current_session=&s_sessions[1];outgoing[0].clear();outgoing[1].clear();SendPendingLogs();
    assert(outgoing[0].empty()&&has(1,0,"LOG")&&has(1,0,"DROP"));
    request(0,73,"HELLO");assert(s_log_hook_installed&&s_sessions[1].log_subscribed);
    request(1,74,"BYE");assert(!s_log_hook_installed&&!AnyLogSubscribed()&&!s_log_writers);
    // Only the owner's own inactivity can release an abandoned upload lease.
    request(0,80,"HELLO");request(1,81,"HELLO");request(0,82,"FSPUT normal 4");
    original=s_sessions[0].upload.file;now_us+=uint64_t(kSessionIdleTimeoutMs)*1000+1;
    s_sessions[0].last_activity_us=now_us;PollControlSession(s_sessions[1]);
    assert(!s_sessions[1].active&&s_sessions[0].active&&s_sessions[0].upload.file==original&&s_mutation_owner==0);
    now_us+=uint64_t(kSessionIdleTimeoutMs)*1000+1;PollControlSession(s_sessions[0]);
    assert(!s_sessions[0].active&&!s_sessions[0].upload.file&&s_mutation_owner==kNoSession);
    assert(!reboots);
    puts("PXADB dual sessions: real parser/dispatcher, fragmented/bulk input, transfer fencing, independent unlock/logs, delayed ACK routing and reconnect generations passed");
}
'''


def main():
    production = (ROOT / 'firmware/components/pxadb/src_pxa_service.cc').read_text()
    source = FIXTURE.replace('// TYPES', '\n'.join(function(production, 'struct ' + name) + ';'
        for name in ('BulkUpload', 'LogRecord', 'LogScratch', 'FileUpload', 'ControlParser')))
    start = production.index('#if CONFIG_PXADB_UART_SECONDARY\nconstexpr size_t kSessionCount')
    end = production.index('std::atomic<bool> s_running', start)
    source = source.replace('// SESSION', production[start:end])
    groups = {
        '// FRAMING': ('bool WriteAll(', 'bool SendFrameTo(', 'bool SendFrame('),
        '// TRANSFERS': ('bool FileUploadActive(', 'void AbortFileUpload(', 'void TryDiscardPackageTransfers(',
            'bool PackageTransfersReady(', 'void AbortTransfers(', 'bool IsMutatingCommand(',
            'bool AdmitMutation(', 'void ReleaseMutationIfIdle(', 'void HandleFsUnlock(', 'void HandleFsLock('),
        '// LOGS': ('void RememberLog(', 'LogScratch* AcquireLogScratch(', 'void ReleaseLogScratch(',
            'bool AnyLogSubscribed(', 'void EnableLogCapture(', 'void DisableLogCapture(',
            'int PxaLogVprintf(const char* format, va_list arguments) {'),
        '// INPUT': ('bool InputSessionActive(', 'void SendInputAck(', 'bool QueueInput(', 'void QueueSessionCancel('),
        '// RESET': ('void ResetControlSession(',),
        '// BULK': ('size_t ConsumeFsBulk(',),
        '// DISPATCH': ('void HandleCommand(',),
        '// LOG_DRAIN': ('void SendPendingLogs() {',),
        '// POLL': ('bool PollControlSession(',),
    }
    # The hook installs a function defined below; the forward declaration is a boundary.
    source = source.replace('// LOGS', 'int PxaLogVprintf(const char*,va_list);\n// LOGS')
    for marker, signatures in groups.items():
        source = source.replace(marker, '\n'.join(function(production, signature) for signature in signatures))
    component = ROOT / 'firmware/components/pxadb'
    with tempfile.TemporaryDirectory(prefix='pxadb-dual-session-') as temporary:
        unit, binary = Path(temporary)/'test.cpp', Path(temporary)/'test'
        unit.write_text(source.replace('TEMP_ROOT', temporary))
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
            '-I' + str(component/'tests/stubs'), '-I' + str(component/'include'),
            str(unit), '-lmbedcrypto', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
