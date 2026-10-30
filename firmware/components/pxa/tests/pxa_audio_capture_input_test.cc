// SPDX-License-Identifier: Apache-2.0
#include <pxa/pxa_audio_capture_input.h>
#include <pxa/pxa_audio_capture.h>
#include <esp_opus_enc.h>
#include "pxa_codec_memory.h"
#include "pxa_esp_resource_memory.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>
static std::thread worker;
static std::mutex notification_mutex, queue_mutex;
static std::condition_variable notification;
static unsigned notifications=0;
static std::atomic<bool> hold_notification{false}, notification_entered{false}, task_deleted{false};
static std::deque<std::vector<uint8_t>> packets;
static size_t item_size;
static std::vector<void*> allocations;
static unsigned fail_allocation=0;
static std::atomic<int> reads_left{0}, input_opens{0}, input_closes{0}, encoded{0};
static std::atomic<bool> enabled{false}, process_failure{false}, block_read{false}, reading{false};
static std::atomic<int16_t> marker{10};
static void* source;
static pxa_status_t (*source_open)(void*);
static int32_t (*source_read)(void*,uint8_t*,size_t);
static void (*source_close)(void*);
template<typename F> static void Wait(F condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()) { assert(std::chrono::steady_clock::now()<deadline); std::this_thread::yield(); }
}
extern "C" pxa_status_t pxa_esp_resource_memory_initialize(void) { return PXA_STATUS_OK; }
extern "C" const pxa_memory_allocator_t* pxa_esp_device_resource_allocator(unsigned,unsigned) { return nullptr; }
extern "C" void* pxa_memory_allocate(const pxa_memory_allocator_t*,size_t size) {
    if(fail_allocation && !--fail_allocation) return nullptr;
    void* p=std::calloc(1,size); allocations.push_back(p); return p;
}
extern "C" void pxa_memory_release(void* p) {
    if(!p)return;
    for(auto& value:allocations) if(value==p) { value=nullptr; break; }
    std::free(p);
}
PxaCodecMemoryScope::PxaCodecMemoryScope(const pxa_memory_allocator_t*):previous_(nullptr) {}
PxaCodecMemoryScope::~PxaCodecMemoryScope()=default;
extern "C" void pxa_esp_audio_set_capture_source(void* ctx,uint16_t frame,pxa_status_t(*open)(void*),
    int32_t(*read)(void*,uint8_t*,size_t),void(*close)(void*)) {
    assert(frame==20); source=ctx; source_open=open; source_read=read; source_close=close;
}
QueueHandle_t xQueueCreateStatic(unsigned count,size_t bytes,uint8_t*,StaticQueue_t*) { assert(count==4); item_size=bytes; return &packets; }
void vQueueDelete(QueueHandle_t) { packets.clear(); }
int xQueueSend(QueueHandle_t,const void* data,uint32_t timeout) {
    assert(!timeout); std::lock_guard<std::mutex> lock(queue_mutex);
    if(packets.size()==4) return 0;
    const auto* p=static_cast<const uint8_t*>(data); packets.emplace_back(p,p+item_size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t,void* data,uint32_t timeout) {
    assert(!timeout); std::lock_guard<std::mutex> lock(queue_mutex);
    if(packets.empty()) return 0;
    std::memcpy(data,packets.front().data(),item_size); packets.pop_front(); return pdTRUE;
}
TaskHandle_t xTaskCreateStatic(void(*fn)(void*),const char*,uint32_t,void* ctx,unsigned,StackType_t*,StaticTask_t*) {
    worker=std::thread([=]{fn(ctx);}); return &worker;
}
void vTaskDelete(TaskHandle_t) { task_deleted=true; }
void xTaskNotifyGive(TaskHandle_t) {
    assert(!task_deleted);
    if(hold_notification) { notification_entered=true; Wait([]{return !hold_notification;}); }
    assert(!task_deleted);
    {std::lock_guard<std::mutex> lock(notification_mutex); ++notifications;}
    notification.notify_all();
}
uint32_t ulTaskNotifyTake(int,TickType_t timeout) {
    std::unique_lock<std::mutex> lock(notification_mutex);
    if(timeout==portMAX_DELAY) notification.wait(lock,[]{return notifications;});
    else notification.wait_for(lock,std::chrono::milliseconds(timeout),[]{return notifications;});
    const auto result=notifications; notifications=0; return result;
}
void vTaskDelay(TickType_t) { std::this_thread::yield(); }
int esp_opus_enc_open(esp_opus_enc_config_t* c,size_t,void** out) {
    assert(c->sample_rate==16000 && c->channel==1 && c->complexity==0 && c->frame_duration==20);
    *out=reinterpret_cast<void*>(1); return ESP_AUDIO_ERR_OK;
}
int esp_opus_enc_get_frame_size(void*,int* in,int* out) { *in=640; *out=1275; return ESP_AUDIO_ERR_OK; }
int esp_opus_enc_process(void*,esp_audio_enc_in_frame_t* input,esp_audio_enc_out_frame_t* out) {
    if(process_failure)return -1;
    assert(input->len==640 && out->len>=3); out->buffer[0]=input->buffer[0]; out->buffer[1]=0; out->buffer[2]=1;
    out->encoded_bytes=3; ++encoded; return ESP_AUDIO_ERR_OK;
}
void esp_opus_enc_close(void*) {}
static bool Control(void*,bool on) { enabled=on; if(on) ++input_opens; else ++input_closes; return true; }
static int32_t Read(void*,int16_t* out,size_t size) {
    if(block_read) { reading=true; Wait([]{return !block_read;}); reading=false; }
    int left=reads_left;
    while(left>0 && !reads_left.compare_exchange_weak(left,left-1)) {}
    if(left<=0) return PXA_STATUS_WOULD_BLOCK;
    const size_t count=std::min<size_t>(160,size);
    std::fill_n(out,count,marker.load()); return count;
}
int main(int argc,char** argv) {
    PxaAudioCaptureInput input;
    if(argc>1) {
        fail_allocation=std::strtoul(argv[1],nullptr,10);
        assert(!input.Initialize(Control,Read,nullptr) && !source);
        for(const auto value:allocations) assert(!value);
        return 0;
    }
    assert(input.Initialize(Control,Read,nullptr));
    assert(source_open(source)==PXA_STATUS_OK && source_open(source)==PXA_STATUS_BUSY);
    uint8_t data[PXA_AUDIO_CAPTURE_MAX_PACKET];
    assert(source_read(source,data,sizeof(data))==PXA_STATUS_WOULD_BLOCK);
    reads_left=2;
    Wait([&]{return source_read(source,data,sizeof(data))==3;}); assert(data[0]==10 && enabled);
    const int before=encoded; reads_left=20; Wait([&]{return encoded>=before+10;});
    // A slow consumer retains at most four packets.
    unsigned count=0; while(source_read(source,data,sizeof(data))==3) ++count; assert(count<=4 && count>=1);
    block_read=true; Wait([]{return reading.load();});
    source_close(source); assert(source_open(source)==PXA_STATUS_OK);
    marker=20; reads_left=4; block_read=false;
    Wait([&]{return source_read(source,data,sizeof(data))==3;}); assert(data[0]==20 && input_opens>=2);
    process_failure=true; reads_left=2;
    Wait([&]{return source_read(source,data,sizeof(data))==PXA_STATUS_IO_ERROR;});
    source_close(source); process_failure=false; assert(source_open(source)==PXA_STATUS_OK); reads_left=2;
    Wait([&]{return source_read(source,data,sizeof(data))==3;});
    block_read=true; Wait([]{return reading.load();});
    hold_notification=true;
    std::atomic<bool> stopped{false}; std::thread stop([&]{input.StopForShutdown(); stopped=true;});
    Wait([]{return notification_entered.load();});
    Wait([&]{return source_open(source)==PXA_STATUS_UNAVAILABLE;});
    std::atomic<bool> second_entered{false}, second_stopped{false};
    std::thread second([&]{second_entered=true; input.StopForShutdown(); second_stopped=true;});
    Wait([&]{return second_entered.load();});
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); assert(!stopped && !second_stopped);
    block_read=false; Wait([]{return !enabled;});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!task_deleted && !stopped && !second_stopped);
    hold_notification=false; stop.join(); second.join(); worker.join();
    assert(stopped && second_stopped && task_deleted && !enabled && input_opens==input_closes);
    assert(source_open(source)==PXA_STATUS_UNAVAILABLE); source_close(source); input.StopForShutdown();
    for(const auto value:allocations) std::free(value);
    puts("Capture worker: bounded queue, partial PCM, generation changes, fault recovery and concurrent shutdown and task deletion race passed");
}
