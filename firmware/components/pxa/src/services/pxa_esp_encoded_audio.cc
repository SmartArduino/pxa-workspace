// SPDX-License-Identifier: Apache-2.0
#include "pxa_esp_encoded_audio.h"
#include "pxa_codec_memory.h"
#include "pxa_esp_resource_memory.h"
#include "pxa/wire.h"
#include "esp_aac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace {
constexpr unsigned queue_packets = 24;
constexpr size_t queue_bytes = 32 * 1024;
constexpr unsigned worker_stack = 8192;
constexpr unsigned worker_priority = 6;
constexpr size_t max_pcm_chunk = 48000 / 50 * 2 * sizeof(int16_t);
constexpr size_t max_decoded_frame = 16384;
constexpr const char *tag = "PxaEncodedAudio";

struct Stream {
    const pxa_memory_allocator_t *allocator;
    void *context;
    pxa_audio_submit_fn submit;
    uint64_t session;
    unsigned sample_rate, channels;
    // Producer and consumer share this bounded ring under queue_lock.
    std::array<uint8_t,queue_bytes> queue{};
    std::array<uint16_t,queue_packets> lengths{};
    size_t read_at=0, bytes=0;
    unsigned read_slot=0, count=0;
    uint64_t last_pts=0;
    bool have_pts=false, finishing=false, drained=false, worker_owned=false;
    pxa_status_t error=PXA_STATUS_OK;
    // After attach publishes the stream, only the worker touches the codec.
    void *decoder=nullptr;
    std::array<uint8_t,PXA_AUDIO_ENCODED_PACKET_BYTES> packet{};
    // Decode at a fixed offset so a partial 20ms mixer chunk can precede it.
    alignas(4) std::array<uint8_t,max_pcm_chunk+max_decoded_frame> decoded{};
    size_t decoded_at=0, decoded_bytes=0;
    uint64_t decode_us=0;
    unsigned frames=0;
};

portMUX_TYPE init_lock=portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t mutex_storage;
SemaphoreHandle_t queue_lock=nullptr;
TaskHandle_t worker_task=nullptr;
Stream *active=nullptr;

struct Lock {
    SemaphoreHandle_t mutex;
    Lock() {
        mutex=__atomic_load_n(&queue_lock,__ATOMIC_ACQUIRE);
        if (!mutex) {
            portENTER_CRITICAL(&init_lock);
            mutex=__atomic_load_n(&queue_lock,__ATOMIC_RELAXED);
            if (!mutex) {
                mutex=xSemaphoreCreateMutexStatic(&mutex_storage);
                __atomic_store_n(&queue_lock,mutex,__ATOMIC_RELEASE);
            }
            portEXIT_CRITICAL(&init_lock);
        }
        xSemaphoreTake(mutex,portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(mutex); }
};

void release(Stream *stream) {
    PxaCodecMemoryScope memory(stream->allocator);
    if (stream->decoder) esp_aac_dec_close(stream->decoder);
    stream->~Stream();
    pxa_memory_release(stream);
}

void report_error(Stream &stream,pxa_status_t status) {
    Lock lock;
    if (active!=&stream) return;
    stream.error=status;
    ESP_LOGW(tag,"AAC stream stopped, status=%d",int(status));
}

bool decode(Stream &stream,size_t bytes) {
    PxaCodecMemoryScope memory(stream.allocator);
    esp_audio_dec_in_raw_t input{};
    input.buffer=stream.packet.data();
    input.len=bytes;
    esp_audio_dec_out_frame_t output{};
    output.buffer=stream.decoded.data()+max_pcm_chunk;
    output.len=max_decoded_frame;
    const size_t partial=stream.decoded_bytes;
    if (partial)
        std::memmove(output.buffer-partial,stream.decoded.data()+stream.decoded_at,partial);
    esp_audio_dec_info_t format{};
    const auto started=esp_timer_get_time();
    auto result=esp_aac_dec_decode(stream.decoder,&input,&output,&format);
    stream.decode_us+=esp_timer_get_time()-started;
    if (result!=ESP_AUDIO_ERR_OK || input.consumed!=bytes ||
        !output.decoded_size || output.decoded_size>max_decoded_frame ||
        format.sample_rate!=stream.sample_rate || format.channel!=stream.channels ||
        format.bits_per_sample!=16 || output.decoded_size%(stream.channels*2u)) {
        report_error(stream,result==ESP_AUDIO_ERR_MEM_LACK ? PXA_STATUS_RESOURCE_LIMIT :
            PXA_STATUS_PROTOCOL_ERROR);
        return false;
    }
    stream.decoded_at=max_pcm_chunk-partial;
    stream.decoded_bytes=partial+output.decoded_size;
    if (++stream.frames%128==0)
        ESP_LOGI(tag,"decoded=%u aac_ms=%llu",stream.frames,(unsigned long long)(stream.decode_us/1000));
    return true;
}

pxa_status_t publish(Stream &stream,size_t bytes) {
    // Mixer lock is taken inside submit. Holding queue_lock fences this write
    // against detaching the producer during FLUSH or CLOSE.
    Lock lock;
    if (active!=&stream) return PXA_STATUS_NOT_FOUND;
    auto result=stream.submit(stream.context,stream.session,stream.decoded.data()+stream.decoded_at,bytes);
    if (result==PXA_STATUS_OK) {
        stream.decoded_at+=bytes;
        stream.decoded_bytes-=bytes;
    }
    else if (result!=PXA_STATUS_WOULD_BLOCK) stream.error=result;
    return result;
}

enum class Work { more, idle, mixer_full };
Work publish_work(Stream &stream,size_t bytes) {
    const auto result=publish(stream,bytes);
    if (result==PXA_STATUS_WOULD_BLOCK) return Work::mixer_full;
    return result==PXA_STATUS_OK || result==PXA_STATUS_NOT_FOUND ? Work::more : Work::idle;
}
Work process(Stream &stream) {
    const size_t pcm_limit=(stream.sample_rate/50)*stream.channels*2;
    if (stream.decoded_bytes>=pcm_limit) return publish_work(stream,pcm_limit);

    size_t bytes=0;
    {
        Lock lock;
        if (active!=&stream) return Work::more;
        if (stream.error || stream.drained) return Work::idle;
        if (stream.count) {
            bytes=stream.lengths[stream.read_slot];
            const size_t first=std::min(bytes,queue_bytes-stream.read_at);
            std::memcpy(stream.packet.data(),stream.queue.data()+stream.read_at,first);
            std::memcpy(stream.packet.data()+first,stream.queue.data(),bytes-first);
            stream.read_at=(stream.read_at+bytes)%queue_bytes;
            stream.read_slot=(stream.read_slot+1)%queue_packets;
            stream.bytes-=bytes;
            --stream.count;
        } else if (!stream.finishing) return Work::idle;
        else if (!stream.decoded_bytes) {
            stream.drained=true;
            return Work::idle;
        }
    }
    if (bytes) return decode(stream,bytes) ? Work::more : Work::idle;
    return publish_work(stream,stream.decoded_bytes);
}

void worker(void *) {
    Stream *owned=nullptr;
    for (;;) {
        Stream *retired=nullptr;
        {
            Lock lock;
            if (owned!=active) {
                retired=owned;
                owned=active;
                if (owned) owned->worker_owned=true;
            }
        }
        if (retired) release(retired);
        const auto work=owned ? process(*owned) : Work::idle;
        if (work!=Work::more)
            ulTaskNotifyTake(pdTRUE,work==Work::mixer_full ? pdMS_TO_TICKS(80)+1 : portMAX_DELAY);
    }
}

int32_t attach(void *context,uint64_t session,const pxa_audio_format_t &format,
    const uint8_t *data,size_t size,pxa_audio_submit_fn submit) {
    if (size!=4 || pxa_read_u16(data+2)) return PXA_STATUS_INVALID_ARGUMENT;
    if (pxa_read_u16(data)!=PXA_AUDIO_ENCODED_AAC_ADTS) return PXA_STATUS_UNSUPPORTED;
    auto *allocator=pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_AUDIO);
    if (!allocator) return PXA_STATUS_RESOURCE_LIMIT;
    auto *memory=pxa_memory_allocate(allocator,sizeof(Stream));
    if (!memory) return PXA_STATUS_RESOURCE_LIMIT;
    auto *stream=new(memory) Stream{allocator,context,submit,session,format.sample_rate,format.channels};
    {
        // Report codec allocation failures to CONFIGURE before accepting packets.
        // The first video keyframe may need most of the remaining external RAM.
        PxaCodecMemoryScope scope(allocator);
        if (esp_aac_dec_open(nullptr,0,&stream->decoder)!=ESP_AUDIO_ERR_OK) {
            release(stream);
            return PXA_STATUS_RESOURCE_LIMIT;
        }
    }
    {
        Lock lock;
        if (active) {
            release(stream);
            return PXA_STATUS_BUSY;
        }
        if (!worker_task &&
            xTaskCreateWithCaps(worker,"pxa_aac",worker_stack,nullptr,worker_priority,&worker_task,
                MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
            release(stream);
            return PXA_STATUS_RESOURCE_LIMIT;
        }
        active=stream;
        xTaskNotifyGive(worker_task);
    }
    ESP_LOGI(tag,"AAC queue session=%llu rate=%u channels=%u",
        (unsigned long long)session,unsigned(format.sample_rate),unsigned(format.channels));
    return PXA_STATUS_OK;
}

bool supported_adts(const uint8_t *data,size_t size,const Stream &stream) {
    constexpr unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    if (size<7 || data[0]!=0xff || (data[1]&0xf6)!=0xf0 || (data[2]>>6)!=1 ||
        (data[6]&3) || (!(data[1]&1) && size<9)) return false;
    const unsigned frequency=(data[2]>>2)&15u;
    const unsigned channels=((data[2]&1u)<<2)|(data[3]>>6);
    return frequency<std::size(rates) && rates[frequency]==stream.sample_rate &&
        channels==stream.channels && size==((unsigned(data[3]&3u)<<11)|(unsigned(data[4])<<3)|(data[5]>>5));
}

int32_t enqueue(Stream &stream,const uint8_t *data,size_t size) {
    if (stream.finishing) return PXA_STATUS_BAD_STATE;
    if (size<4 || size>PXA_AUDIO_ENCODED_BATCH_BYTES || pxa_read_u16(data+2))
        return PXA_STATUS_INVALID_ARGUMENT;
    const unsigned count=pxa_read_u16(data);
    if (!count || count>PXA_AUDIO_ENCODED_BATCH_MAX) return PXA_STATUS_INVALID_ARGUMENT;
    struct Packet {const uint8_t *data;uint16_t bytes;};
    std::array<Packet,PXA_AUDIO_ENCODED_BATCH_MAX> packets{};
    uint64_t last_pts=stream.last_pts;
    size_t total=0, at=4;
    for (unsigned i=0;i<count;++i) {
        if (size-at<10) return PXA_STATUS_INVALID_ARGUMENT;
        const auto pts=pxa_read_u64(data+at);
        const auto bytes=pxa_read_u16(data+at+8);
        at+=10;
        if ((stream.have_pts || i) && pts<last_pts) return PXA_STATUS_INVALID_ARGUMENT;
        if (bytes>PXA_AUDIO_ENCODED_PACKET_BYTES || bytes>size-at ||
            !supported_adts(data+at,bytes,stream)) return PXA_STATUS_INVALID_ARGUMENT;
        packets[i]={data+at,bytes};
        at+=bytes;
        total+=bytes;
        last_pts=pts;
    }
    if (at!=size) return PXA_STATUS_INVALID_ARGUMENT;
    if (count>queue_packets-stream.count || total>queue_bytes-stream.bytes) return PXA_STATUS_WOULD_BLOCK;
    size_t write_at=(stream.read_at+stream.bytes)%queue_bytes;
    for (unsigned i=0;i<count;++i) {
        const auto &packet=packets[i];
        const size_t first=std::min<size_t>(packet.bytes,queue_bytes-write_at);
        std::memcpy(stream.queue.data()+write_at,packet.data,first);
        std::memcpy(stream.queue.data(),packet.data+first,packet.bytes-first);
        write_at=(write_at+packet.bytes)%queue_bytes;
        stream.lengths[(stream.read_slot+stream.count+i)%queue_packets]=packet.bytes;
    }
    const bool was_empty=stream.count==0;
    stream.count+=count;
    stream.bytes+=total;
    stream.have_pts=true;
    stream.last_pts=last_pts;
    if (was_empty) xTaskNotifyGive(worker_task);
    return int32_t(size);
}
}

extern "C" int32_t pxa_esp_encoded_audio_io(void *context,uint64_t session,
    const pxa_audio_format_t *format,uint32_t operation,uint8_t *data,size_t size,
    pxa_audio_submit_fn submit) {
    if (operation==PXA_AUDIO_IO_CONFIGURE_ENCODED)
        return attach(context,session,*format,data,size,submit);
    Lock lock;
    if (!active || active->session!=session) return PXA_STATUS_BAD_STATE;
    if (active->error) return active->error;
    if (operation==PXA_AUDIO_IO_WRITE_ENCODED) return enqueue(*active,data,size);
    if (operation==PXA_AUDIO_IO_FINISH_ENCODED) {
        if (size) return PXA_STATUS_INVALID_ARGUMENT;
        active->finishing=true;
        xTaskNotifyGive(worker_task);
        return PXA_STATUS_OK;
    }
    if (operation==PXA_AUDIO_IO_ENCODED_STATE && size==4) {
        pxa_write_u32(data,active->drained ? PXA_AUDIO_ENCODED_DRAINED : 0);
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_INVALID_ARGUMENT;
}

// The single worker lives for the duration of the Host. Notifications are
// retained if the renderer frees space before the worker starts waiting.
extern "C" void pxa_esp_encoded_audio_writable() {
    xTaskNotifyGive(worker_task);
}

extern "C" void pxa_esp_encoded_audio_detach(uint64_t session) {
    Stream *retired=nullptr;
    {
        Lock lock;
        if (!active || (session && active->session!=session)) return;
        if (!active->worker_owned) retired=active;
        active=nullptr;
        xTaskNotifyGive(worker_task);
    }
    if (retired) release(retired);
}
