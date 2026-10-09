#undef NDEBUG
#include <cassert>
#include <deque>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <new>
#include <unistd.h>
#define private public
#include <pxa/pxa_audio_output.h>
#undef private
#include "../src/services/pxa_audio_output.cc"
static pxa_memory_budget_t resource_budget;
static pxa_memory_allocator_t allocators[3][PXA_MEMORY_CLASSES][PXA_MEMORY_KINDS];
static pxa_memory_owner_t owners[3];
static unsigned current_owner, music_lock_depth;
static const size_t limits[2] = {64 * 1024, 128 * 1024};
static unsigned fail_allocation, live_queues, live_mutexes, created_tasks;
static void* registered_codecs[3];
static void* budget_raw_allocate(void*, size_t bytes) { assert(!music_lock_depth); if (fail_allocation && !--fail_allocation) return nullptr; return malloc(bytes); }
static void budget_raw_free(void*, void* p) { assert(!music_lock_depth); free(p); }
static void open_budget_owner(unsigned slot) {
    assert(pxa_memory_owner_open(&resource_budget, limits, &owners[slot]) == PXA_STATUS_OK);
    for (unsigned c=0;c<PXA_MEMORY_CLASSES;++c)
        for(unsigned k=0;k<PXA_MEMORY_KINDS;++k)
            allocators[slot][c][k] = {&resource_budget, owners[slot], static_cast<uint8_t>(c),
                static_cast<uint8_t>(k), nullptr, budget_raw_allocate, budget_raw_free};
    current_owner = slot;
}
extern "C" const pxa_memory_allocator_t* pxa_esp_resource_allocator(unsigned cls, unsigned kind) {
    return &allocators[current_owner][cls][kind];
}
extern "C" pxa_status_t pxa_esp_resource_memory_initialize() { return PXA_STATUS_OK; }
extern "C" const pxa_memory_allocator_t* pxa_esp_device_resource_allocator(unsigned cls, unsigned kind) {
    return &allocators[2][cls][kind];
}
extern "C" void* pxa_esp_resource_allocate(unsigned cls, unsigned kind, size_t bytes) {
    return pxa_memory_allocate(pxa_esp_resource_allocator(cls, kind), bytes);
}
static pxa_memory_stats_t budget_stats(pxa_memory_owner_t owner=0) {
    pxa_memory_stats_t stats; assert(pxa_memory_budget_stats(&resource_budget, owner ? owner : owners[current_owner], &stats)==0); return stats;
}
// Codec state-machine fixture. Real-file reads and catalog lifetime are
// exercised by pxa_esp_assets_backend_test, independently of this mock codec.
struct pxa_esp_music_input {
    FILE* file;
    void* context;
    int (*cancelled)(void*);
    char path[512];
};
static unsigned input_leases;
static pxa_status_t injected_read_error;
extern "C" pxa_status_t pxa_esp_music_input_acquire(const char* path,
    const pxa_memory_allocator_t* allocator, pxa_esp_music_input_t** out) {
    assert(!music_lock_depth); *out=nullptr;
    size_t n=strlen(path);
    if (n<4 || strcmp(path+n-4,".ogg")) return PXA_STATUS_UNSUPPORTED;
    auto* input=static_cast<pxa_esp_music_input_t*>(pxa_memory_allocate(allocator,sizeof(pxa_esp_music_input_t)));
    if (!input) return PXA_STATUS_RESOURCE_LIMIT;
    memset(input,0,sizeof(*input)); assert(n<sizeof(input->path)); strcpy(input->path,path);
    ++input_leases; *out=input; return PXA_STATUS_OK;
}
extern "C" pxa_status_t pxa_esp_music_input_open(pxa_esp_music_input_t* input,void* context,int (*cancelled)(void*)) {
    assert(!music_lock_depth); input->context=context; input->cancelled=cancelled;
    input->file=fopen(input->path,"rb"); return input->file ? PXA_STATUS_OK : PXA_STATUS_NOT_FOUND;
}
extern "C" pxa_status_t pxa_esp_music_input_read(pxa_esp_music_input_t* input,uint8_t* out,size_t cap,size_t* count) {
    assert(!music_lock_depth); *count=0;
    if (input->cancelled(input->context)) return PXA_STATUS_CANCELLED;
    if (injected_read_error) return injected_read_error;
    *count=fread(out,1,cap,input->file); return ferror(input->file) ? PXA_STATUS_IO_ERROR : PXA_STATUS_OK;
}
extern "C" pxa_status_t pxa_esp_music_input_rewind(pxa_esp_music_input_t* input) {
    assert(!music_lock_depth); return fseek(input->file,0,SEEK_SET) ? PXA_STATUS_IO_ERROR : PXA_STATUS_OK;
}
extern "C" int pxa_esp_music_input_eof(const pxa_esp_music_input_t* input) { return feof(input->file); }
extern "C" void pxa_esp_music_input_release(pxa_esp_music_input_t* input) {
    assert(!music_lock_depth); if (!input) return;
    if (input->file) fclose(input->file);
    assert(input_leases); --input_leases; pxa_memory_release(input);
}
struct Queue { size_t capacity, bytes; std::deque<std::vector<uint8_t>> items; };
struct Tick {};
struct Idle {};
static int decoder_mode;
static unsigned decoder_calls, decoder_closes, decoded_samples;
static PxaAudioOutput* decoding_audio;
static PxaAudioOutput* pending_output;
static bool natural_drain;
// Six I2S descriptors leave five writable buffers. The DMA consumes one
// block every 20 ms, independently of the Host's timer and rendering work.
static struct {
    bool active;
    uint64_t now_us, next_dma_us;
    unsigned queued, writes, underruns, high_water;
} output_clock;
static void advance_output_clock(uint64_t elapsed_us) {
    const uint64_t until = output_clock.now_us + elapsed_us;
    while (output_clock.next_dma_us <= until) {
        if (output_clock.queued) --output_clock.queued;
        else ++output_clock.underruns;
        output_clock.next_dma_us += 20000;
    }
    output_clock.now_us = until;
}

void* heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
void* heap_caps_calloc(size_t n, size_t s, unsigned) { return calloc(n,s); }
QueueHandle_t xQueueCreateStatic(unsigned n, size_t s, uint8_t* bytes, StaticQueue_t* storage) {
    assert(bytes && storage); static_assert(sizeof(Queue) <= sizeof(StaticQueue_t));
    ++live_queues; return new(storage) Queue{n,s,{}};
}
int xQueueSend(QueueHandle_t h,const void* data,uint32_t) {
    auto* q=static_cast<Queue*>(h); if(q->items.size()==q->capacity) return 0;
    const auto* p=static_cast<const uint8_t*>(data);
    q->items.emplace_back(p,p+q->bytes); return 1;
}
int xQueueOverwrite(QueueHandle_t h,const void* p) { xQueueReset(h);return xQueueSend(h,p,0); }
int xQueueReceive(QueueHandle_t h,void* data,uint32_t wait) {
    auto* q=static_cast<Queue*>(h);
    if(q->items.empty()) { if(wait) throw Idle{}; return 0; }
    memcpy(data,q->items.front().data(),q->bytes);q->items.pop_front();return 1;
}
void xQueueReset(QueueHandle_t h) {static_cast<Queue*>(h)->items.clear();}
void vQueueDelete(QueueHandle_t h) {assert(live_queues); --live_queues; static_cast<Queue*>(h)->~Queue();}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) {++live_mutexes; s->locked=false; return &s->locked;}
int xSemaphoreTake(SemaphoreHandle_t h,uint32_t) {assert(!*static_cast<bool*>(h));*static_cast<bool*>(h)=true;++music_lock_depth;return 1;}
void xSemaphoreGive(SemaphoreHandle_t h) {assert(*static_cast<bool*>(h));*static_cast<bool*>(h)=false;--music_lock_depth;}
void vSemaphoreDelete(SemaphoreHandle_t h) {assert(h && live_mutexes && !music_lock_depth);--live_mutexes;}
TaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,uint32_t bytes,void* ctx,unsigned,
                              StackType_t* stack,StaticTask_t* tcb) {
    auto* audio = static_cast<PxaAudioOutput*>(ctx);
    assert(bytes == kMusicTaskStackBytes || bytes == kOutputTaskStackBytes);
    assert(stack && tcb && audio->runtime_storage_ && live_queues == 2 && live_mutexes == 1);
    assert(registered_codecs[0] && registered_codecs[1] && registered_codecs[2]);
    ++created_tasks; return tcb;
}
void vTaskDelete(TaskHandle_t) {}
void xTaskNotifyGive(TaskHandle_t) {}
uint32_t ulTaskNotifyTake(int,TickType_t) {throw Idle{};}
TickType_t xTaskGetTickCount() {return output_clock.active ? output_clock.now_us / 1000 : 0;}
int64_t esp_timer_get_time() {static int64_t us; return ++us;}
void vTaskDelayUntil(TickType_t* last,TickType_t period) {
    if (!output_clock.active) throw Tick{};
    *last += period;
    const uint64_t until = static_cast<uint64_t>(*last) * 1000;
    if (until > output_clock.now_us) advance_output_clock(until - output_clock.now_us);
}
void vTaskDelay(TickType_t) {
    if (decoding_audio) {
        assert(decoding_audio->music_count_ > 0 && decoding_audio->music_count_ <= 320);
        decoded_samples += decoding_audio->music_count_;
        assert(decoding_audio->music_ring_[0] == 1000);
        if (natural_drain) {
            decoding_audio->music_count_=0;
            decoding_audio->music_output_token_=decoding_audio->music_token_.load();
            pending_output=decoding_audio;
        } else assert(decoding_audio->ControlAsset(decoding_audio,0,PXA_AUDIO_ASSET_STOP,0));
        decoding_audio = nullptr;
        return;
    }
    if (pending_output) {
        assert(pending_output->music_active_.load() && !pending_output->music_events_.records[0].terminal);
        pending_output->music_output_token_=0; pending_output=nullptr;
        return;
    }
    throw Idle{};
}
unsigned uxTaskGetStackHighWaterMark(void*) {return 4096;}
static int register_codec(unsigned index) {
    assert(!registered_codecs[index]);
    registered_codecs[index] = media_lib_module_calloc("registry",1,32);
    return registered_codecs[index] ? ESP_AUDIO_ERR_OK : 1;
}
int esp_vorbis_dec_register(){return register_codec(0);}
int esp_opus_dec_register(){return register_codec(1);}
int esp_ogg_dec_register(){return register_codec(2);}
void esp_audio_dec_unregister(int type) {assert(type < 2);media_lib_free(registered_codecs[type]);registered_codecs[type]=nullptr;}
int esp_ogg_dec_unregister(){media_lib_free(registered_codecs[2]);registered_codecs[2]=nullptr;return 0;}
int esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*,esp_audio_simple_dec_handle_t* h){
    if (!decoder_mode) return 1;
    *h=media_lib_module_calloc("mock codec",1,1024);
    return *h ? ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_MEM_LACK;
}
int esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t h){media_lib_free(h);++decoder_closes;return 0;}
int esp_audio_simple_dec_reset(esp_audio_simple_dec_handle_t){return 0;}
int esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t,esp_audio_simple_dec_raw_t* raw,esp_audio_simple_dec_out_t* out){
    ++decoder_calls;
    if(out->len < 32768) {out->needed_size=32768;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;}
    auto* pcm=reinterpret_cast<int16_t*>(out->buffer);
    for(unsigned i=0;i<320;++i)pcm[i]=1000;
    out->decoded_size=640;raw->consumed=raw->len;return ESP_AUDIO_ERR_OK;
}
int esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t,esp_audio_simple_dec_info_t* info){*info={16,16000,1};return ESP_AUDIO_ERR_OK;}
void pxa_host_set_audio_sink(pxa_host_audio_submit_fn,pxa_host_audio_flush_fn,void*){}
void pxa_host_set_audio_asset_sink(pxa_host_audio_asset_play_fn,pxa_host_audio_asset_control_fn,void*){}
void pxa_host_set_audio_sound_sink(pxa_host_audio_sound_fn,void*){}
void pxa_host_set_audio_sound_track_sink(pxa_host_audio_sound_track_fn,pxa_host_audio_sound_control_fn,void*){}
void pxa_host_set_audio_music_sink(const pxa_host_audio_music_sink_t*){}
void pxa_host_audio_notify() { assert(!music_lock_depth); }
static void* sound_allocate(void*,size_t n) { return pxa_esp_resource_allocate(1,PXA_MEMORY_AUDIO,n); }
static void sound_free(void*,void* p) { pxa_memory_release(p); }
static unsigned frames;
static bool reject_output, expect_silence;
static bool write_pcm(void*,const int16_t* pcm,size_t n) {
    assert(n==320);
    bool audible=false; for(size_t i=0;i<n;++i) audible |= pcm[i]>0;
    assert(audible != expect_silence); // The music fade may round its first sample to zero.
    ++frames; return !reject_output;
}
static bool write_clocked_pcm(void*,const int16_t* pcm,size_t n) {
    assert(n==320 && output_clock.active);
    bool audible=false; for(size_t i=0;i<n;++i) audible |= pcm[i]>0;
    assert(audible);
    // The initial attack ends in the first block; buffering must preserve
    // every subsequent sample of this constant 1,000-amplitude loop.
    if (output_clock.writes) for(size_t i=0;i<n;++i) assert(pcm[i]==1000);
    // Mixing is cheap, but occasionally the task is not scheduled for 35 ms.
    advance_output_clock(500 + (output_clock.writes % 17 == 16 ? 35000 : 0));
    if (output_clock.queued == 5)
        advance_output_clock(output_clock.next_dma_us - output_clock.now_us);
    ++output_clock.queued;
    output_clock.high_water=std::max(output_clock.high_water,output_clock.queued);
    if (++output_clock.writes == 320) throw Tick{};
    return true;
}
static void tick(PxaAudioOutput& a){try {a.Run();}catch(Tick&){}catch(Idle&){} }
int main(){
    pxa_memory_budget_config_t cfg = {};
    memcpy(cfg.limit, limits, sizeof(limits));
    memcpy(cfg.temporary_limit, limits, sizeof(limits));
    cfg.temporary_limit[PXA_MEMORY_EXTERNAL] = 64 * 1024;
    assert(!pxa_memory_budget_init(&resource_budget, &cfg));
    open_budget_owner(2); // device, separate from either app generation
    open_budget_owner(0);
    PxaAudioOutput a;
    assert(!a.Initialize(nullptr,nullptr) && !created_tasks);
    for(unsigned fail=1;fail<=6;++fail) {
        fail_allocation=fail;
        assert(!a.Initialize(write_pcm,nullptr));
        assert(!created_tasks && !live_queues && !live_mutexes);
        assert(!a.runtime_storage_ && !a.music_stack_ && !a.music_ring_ && !a.write_);
        assert(!registered_codecs[0] && !registered_codecs[1] && !registered_codecs[2]);
        const auto device=budget_stats(owners[2]); assert(!device.charged[0] && !device.charged[1]);
    }
    fail_allocation=0;
    for(unsigned cls=0;cls<PXA_MEMORY_CLASSES;++cls) {
        void* occupied=pxa_esp_resource_allocate(cls,PXA_MEMORY_RASTER,
            limits[cls]-(pxa_memory_allocation_bytes(1)-1));
        assert(occupied && !a.Initialize(write_pcm,nullptr));
        assert(!created_tasks && !live_queues && !live_mutexes);
        const auto device=budget_stats(owners[2]); assert(!device.charged[0] && !device.charged[1]);
        pxa_memory_release(occupied);
    }
    assert(a.Initialize(write_pcm,nullptr,true) && created_tasks == 2);
    assert(a.device_paced_);
    a.device_paced_=false; // Remaining legacy fixtures end at the software timer.
    const auto device_resident=budget_stats(owners[2]);
    assert(device_resident.charged[0] == pxa_memory_allocation_bytes(sizeof(PxaAudioOutput::RuntimeStorage)) +
        pxa_memory_allocation_bytes(kMusicTaskStackBytes));
    assert(device_resident.charged[1] == pxa_memory_allocation_bytes(a.kMusicSamples*sizeof(int16_t)) +
        3*pxa_memory_allocation_bytes(32));
    const size_t available_external=limits[1]-device_resident.charged[1];
    assert(a.Initialize(write_pcm,nullptr) && created_tasks == 2); // idempotent, no duplicate registration
    char root[]="/tmp/pxa-audio-output.XXXXXX";assert(mkdtemp(root));
    unsigned char pcm[160];memset(pcm,255,sizeof(pcm));
    pxa_asset_info_t info = {}; info.kind=PXA_ASSET_AUDIO;
    info.encoding=PXA_ASSET_ENCODING_PCM_U8_16K_MONO; info.stored_bytes=info.decoded_bytes=160;
    pxa_asset_object_t* sound=nullptr; uint8_t* payload=nullptr;
    assert(!pxa_asset_object_create(&info,sound_allocate,sound_free,nullptr,&sound,&payload));
    memset(payload,255,160); pxa_asset_object_finish_loading(sound);
    // The initial reference represents the cache, retained through playback.
    const size_t sound_charge=budget_stats().charged[1];
    for(unsigned i=0;i<25;++i){ assert(a.PlaySound(&a,0,sound,0));tick(a); }
    assert(frames==25 && budget_stats().charged[1]==sound_charge);
    assert(pxa_asset_object_reference_count(sound)==1);
    assert(!a.PlayAsset(&a,0,"assets/cold.pcm",false,0));
    for(unsigned i=0;i<6;++i) assert(a.PlaySound(&a,i%2,sound,0));
    assert(!a.PlaySound(&a,0,sound,0) && pxa_asset_object_reference_count(sound)==7);
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_PAUSE,0));
    assert(a.ControlAsset(&a,1,PXA_AUDIO_ASSET_PAUSE,0));tick(a);assert(frames==25);
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_STOP,0));
    unsigned live=0;for(auto& voice:a.sounds_)live+=voice.asset!=nullptr;assert(live==3);
    assert(a.ControlAsset(&a,1,PXA_AUDIO_ASSET_RESUME,0));tick(a);assert(frames==26);
    assert(a.ControlAsset(&a,1,PXA_AUDIO_ASSET_STOP,0));
    assert(pxa_asset_object_reference_count(sound)==1);
    assert(pxa_audio_output_limit(20000)==20000);
    assert(pxa_audio_output_limit(40000)<pxa_audio_output_limit(80000));
    assert(pxa_audio_output_limit(80000)<32767);
    assert(pxa_audio_output_limit(-80000)==-pxa_audio_output_limit(80000));
    // Tracked signed PCM keeps playing while the Guest issues no commands.
    pxa_asset_info_t signed_info={}; signed_info.kind=PXA_ASSET_AUDIO;
    signed_info.encoding=PXA_ASSET_ENCODING_PCM_S16LE_16K_MONO;
    signed_info.stored_bytes=signed_info.decoded_bytes=2000;
    pxa_asset_object_t* signed_sound=nullptr;
    assert(!pxa_asset_object_create(&signed_info,sound_allocate,sound_free,nullptr,&signed_sound,&payload));
    for(unsigned i=0;i<1000;++i) {payload[2*i]=0xe8;payload[2*i+1]=3;}
    pxa_asset_object_finish_loading(signed_sound);
    pxa_audio_sound_options_t track={0,1,1};
    assert(a.PlaySoundTrack(&a,0,signed_sound,&track));
    assert(a.PlaySoundTrack(&a,1,signed_sound,&track));
    for(unsigned i=0;i<12;++i) tick(a); // Host loops beyond the original clip.
    assert(pxa_asset_object_reference_count(signed_sound)==3);
    assert(a.ControlMusic(&a,0,PXA_AUDIO_ASSET_SET_GAIN,-20*256));
    for(auto& v:a.sounds_) if(v.asset) assert(v.target_gain_q15==32768);
    pxa_audio_sound_control_t track_control={0,1,PXA_AUDIO_ASSET_STOP};
    assert(a.ControlSound(&a,0,&track_control));tick(a);
    assert(pxa_asset_object_reference_count(signed_sound)==2); // other session survives
    assert(a.PlaySoundTrack(&a,1,signed_sound,&track)); // replacing retains exactly one pin
    assert(pxa_asset_object_reference_count(signed_sound)==2);
    assert(a.ControlSound(&a,1,&track_control));tick(a);
    assert(pxa_asset_object_reference_count(signed_sound)==1);
    // Exercise the actual continuous mixer against a virtual blocking DMA
    // sink. Timer pacing leaves gaps under jitter; device pacing fills the
    // existing ring and survives the identical stalls without changing PCM.
    for (bool device_paced : {false,true}) {
        assert(a.PlaySoundTrack(&a,0,signed_sound,&track));
        a.device_paced_=device_paced;
        a.write_=write_clocked_pcm;
        output_clock={true,0,20000,0,0,0,0};
        tick(a);
        assert(output_clock.writes==320);
        if (device_paced) assert(!output_clock.underruns && output_clock.high_water==5);
        else assert(output_clock.underruns>0 && output_clock.high_water<5);
        if (device_paced) assert(output_clock.now_us>=6300000 && output_clock.now_us<=6400000);
        printf("DMA clock fixture: device_paced=%u frames=%u gaps=%u buffered=%u elapsed_us=%llu\n",
            device_paced,output_clock.writes,output_clock.underruns,output_clock.high_water,
            static_cast<unsigned long long>(output_clock.now_us));
        output_clock.active=false;
        a.write_=write_pcm;
        a.device_paced_=false;
        assert(a.ControlSound(&a,0,&track_control));tick(a);
        assert(pxa_asset_object_reference_count(signed_sound)==1);
    }
    // An immediately failing hardware sink backs off instead of spinning.
    assert(a.PlaySoundTrack(&a,0,signed_sound,&track));
    a.device_paced_=true;
    reject_output=true;
    const unsigned before_failure=frames;
    tick(a);
    assert(frames==before_failure+1);
    reject_output=false;
    a.device_paced_=false;
    assert(a.ControlSound(&a,0,&track_control));tick(a);
    assert(pxa_asset_object_reference_count(signed_sound)==1);
    pxa_asset_object_release(signed_sound);
    // Hot playback requires no allocation, even when all remaining app quota is held.
    void* pressure = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL,PXA_MEMORY_RASTER,
        available_external-sound_charge-(pxa_memory_allocation_bytes(1)-1));
    assert(pressure && a.PlaySound(&a,0,sound,0));tick(a);
    pxa_memory_release(pressure);pxa_asset_object_release(sound);
    assert(!budget_stats().charged[1]);
    FILE* f;
    char ogg[512]; snprintf(ogg,sizeof(ogg),"%s/music.ogg",root);
    f=fopen(ogg,"wb");assert(f);fclose(f);
    assert(a.PlayAsset(&a,0,ogg,false,0));
    const size_t pin = pxa_memory_allocation_bytes(sizeof(pxa_esp_music_input_t));
    assert(budget_stats().charged[1] == pin);
    assert(a.PlayAsset(&a,0,ogg,false,0)); // replaced queued command drops old pin
    assert(budget_stats().charged[1] == pin);
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_STOP,0));
    assert(budget_stats().charged[1] == 0);
    assert(a.PlayAsset(&a,0,ogg,false,0));
    a.music_token_.fetch_add(1); // stale queue item must release without decoding
    try { a.RunMusic(); } catch(Idle&) {}
    assert(budget_stats().charged[1] == 0);
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_STOP,0));
    assert(a.PlayAsset(&a,0,ogg,false,0));
    assert(pxa_memory_owner_close(&resource_budget, owners[0]) == PXA_STATUS_WOULD_BLOCK);
    open_budget_owner(1);
    try { a.RunMusic(); } catch(Idle&) {} // old closed owner cannot charge the new app
    assert(budget_stats(owners[1]).peak[1] == 0);
    assert(!pxa_memory_owner_close(&resource_budget, owners[0]));
    assert(budget_stats().charged[1] == 0);
    // The mock decoder rejects Ogg; all newly allocated output buffers roll back.
    assert(a.PlayAsset(&a,0,ogg,false,0));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(budget_stats(owners[1]).peak[1] > pin && !budget_stats().charged[1]);
    f=fopen(ogg,"wb");assert(f);assert(fwrite(pcm,1,sizeof(pcm),f)==sizeof(pcm));fclose(f);
    decoder_mode = 1; decoding_audio = &a;
    assert(a.PlayAsset(&a,0,ogg,false,0));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(!decoding_audio && decoder_calls == 2 && decoder_closes == 1 && decoded_samples > 0);
    assert(!budget_stats().charged[1]);
    assert(budget_stats(owners[1]).peak[1] >= pxa_memory_allocation_bytes(16384) +
        pxa_memory_allocation_bytes(32768) + pxa_memory_allocation_bytes(4096) + pin);
    // Initial buffers fit, but old+new resize peak does not. No partial output.
    pressure = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER,
        available_external - (pxa_memory_allocation_bytes(1)-1) - (25000 + pin));
    assert(pressure && a.PlayAsset(&a,0,ogg,false,0));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(decoder_calls == 3 && decoder_closes == 2);
    assert(budget_stats().charged[1] == pxa_memory_allocation_bytes(available_external - (pxa_memory_allocation_bytes(1)-1) - (25000 + pin)));
    pxa_memory_release(pressure); assert(!budget_stats().charged[1]);
    // Output + conversion buffers fit but the codec's private allocation
    // does not. Open fails through the real hook and releases the command.
    pressure = pxa_esp_resource_allocate(PXA_MEMORY_EXTERNAL, PXA_MEMORY_RASTER,
        available_external - (pxa_memory_allocation_bytes(1)-1) - (21000 + pin));
    const size_t remaining_charge = budget_stats().charged[1];
    assert(pressure && a.PlayAsset(&a,0,ogg,false,0));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(decoder_calls == 3 && decoder_closes == 2);
    assert(budget_stats().charged[1] == remaining_charge);
    pxa_memory_release(pressure); assert(!budget_stats().charged[1]);
    // Enough total memory remains for decode, but the independent temporary
    // ceiling prevents either output growth or codec-private allocation.
    for (const size_t available_temporary : {size_t{25000}, size_t{21000}}) {
        const auto* temporary = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
        pressure = pxa_memory_allocate(temporary, cfg.temporary_limit[1] -
            (pxa_memory_allocation_bytes(1)-1) - (available_temporary + pin));
        assert(pressure);
        const size_t occupied = budget_stats().charged[1];
        assert(available_external - occupied > 55000);
        const unsigned old_calls = decoder_calls, old_closes = decoder_closes;
        assert(a.PlayAsset(&a,0,ogg,false,0));
        try { a.RunMusic(); } catch(Idle&) {}
        assert(decoder_calls == old_calls + (available_temporary == 25000));
        assert(decoder_closes == old_closes + (available_temporary == 25000));
        assert(budget_stats().charged[1] == occupied);
        assert(budget_stats().temporary_peak[1] <= cfg.temporary_limit[1]);
        pxa_memory_release(pressure); assert(!budget_stats().charged[1]);
    }
    decoding_audio = &a;
    const unsigned old_calls = decoder_calls, old_closes = decoder_closes;
    assert(a.PlayAsset(&a,0,ogg,false,0));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(!decoding_audio && decoder_calls == old_calls + 2 && decoder_closes == old_closes + 1);
    assert(!budget_stats().charged[1]); // resumes after temporary capacity returns
    // Tracked music retains READY and terminal records until the Host consumes
    // them. Slot exhaustion rejects a replacement without disturbing the song.
    uint64_t ids[PXA_AUDIO_PLAYBACK_CAPACITY], rejected;
    for (unsigned i=0;i<PXA_AUDIO_PLAYBACK_CAPACITY;++i)
        assert(!a.PlayMusic(&a,0,91,ogg,false,0,true,&ids[i]));
    const auto active_token=a.music_token_.load();
    assert(a.PlayMusic(&a,0,91,ogg,false,0,false,&rejected)==PXA_STATUS_WOULD_BLOCK && !rejected);
    assert(a.music_token_.load()==active_token && a.music_paused_.load());
    fail_allocation=1;
    assert(a.PlayMusic(&a,0,91,ogg,false,0,false,&rejected)==PXA_STATUS_RESOURCE_LIMIT && !rejected);
    assert(a.music_token_.load()==active_token && a.music_paused_.load());
    pxa_audio_playback_event_t event;
    for (unsigned i=0;i<PXA_AUDIO_PLAYBACK_CAPACITY-1;++i) {
        assert(!a.PlaybackPeek(&a,&event));
        assert(event.instance==ids[i] && event.provider_session==91 && event.state==PXA_AUDIO_PLAYBACK_REPLACED);
        assert(!a.PlaybackConsume(&a,&event));
    }
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_STOP,0));
    assert(!a.PlaybackPeek(&a,&event) && event.state==PXA_AUDIO_PLAYBACK_STOPPED);
    assert(!a.PlaybackConsume(&a,&event));
    decoding_audio=&a; natural_drain=true;
    assert(!a.PlayMusic(&a,0,91,ogg,false,0,false,&ids[0]));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(!decoding_audio && !pending_output && !budget_stats().charged[1]); natural_drain=false;
    assert(!a.PlaybackPeek(&a,&event) && event.instance==ids[0] && event.state==PXA_AUDIO_PLAYBACK_READY);
    assert(!a.PlaybackConsume(&a,&event));
    assert(!a.PlaybackPeek(&a,&event) && event.instance==ids[0] && event.state==PXA_AUDIO_PLAYBACK_ENDED);
    assert(!a.PlaybackConsume(&a,&event));
    // Initial buffering and recovery use the same bounded ring. Partial input
    // is inaudible until ready, each outage counts once, and pause adds no debt.
    int16_t buffered_samples[4096];
    for (auto& sample : buffered_samples) sample=1000;
    PxaAudioOutput::MusicStats before_buffer, after_buffer;
    assert(a.GetMusicStats(&before_buffer));
    assert(!a.PlayMusic(&a,0,91,ogg,false,0,false,&ids[0]));
    assert(a.QueueMusic(buffered_samples,1024,a.music_token_.load()));
    expect_silence=true; tick(a); expect_silence=false;
    assert(a.music_count_==1024 && a.PlaybackPeek(&a,&event)==PXA_STATUS_NOT_FOUND);
    assert(a.QueueMusic(buffered_samples,3072,a.music_token_.load()));
    assert(!a.PlaybackPeek(&a,&event) && event.state==PXA_AUDIO_PLAYBACK_READY);
    assert(!a.PlaybackConsume(&a,&event));
    while (a.music_count_) tick(a);
    assert(a.GetMusicStats(&after_buffer));
    assert(after_buffer.buffer.underruns==before_buffer.buffer.underruns+1);
    expect_silence=true; tick(a); expect_silence=false;
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_PAUSE,0));
    assert(a.GetMusicStats(&after_buffer));
    const uint64_t missing=after_buffer.buffer.missing_samples;
    tick(a);
    assert(a.GetMusicStats(&after_buffer) && after_buffer.buffer.missing_samples==missing);
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_RESUME,0));
    assert(a.QueueMusic(buffered_samples,3072,a.music_token_.load()));
    expect_silence=true; tick(a); expect_silence=false;
    assert(a.music_count_==3072);
    assert(a.QueueMusic(buffered_samples,1024,a.music_token_.load()));
    tick(a);
    assert(a.GetMusicStats(&after_buffer));
    assert(after_buffer.buffer.underruns==before_buffer.buffer.underruns+1);
    assert(after_buffer.buffer.recoveries==before_buffer.buffer.recoveries+1);
    assert(after_buffer.buffer.low_water_valid && after_buffer.buffer.low_water==0);
    assert(a.PlaybackPeek(&a,&event)==PXA_STATUS_NOT_FOUND); // READY is not repeated.
    assert(a.ControlAsset(&a,0,PXA_AUDIO_ASSET_STOP,0));
    assert(!a.PlaybackPeek(&a,&event) && event.state==PXA_AUDIO_PLAYBACK_STOPPED);
    assert(!a.PlaybackConsume(&a,&event) && !input_leases && !budget_stats().charged[1]);
    // Device output failure wins over EOF. Clearing the in-flight marker first
    // would allow the decoder to publish ENDED before this ERROR.
    assert(!a.PlayMusic(&a,0,91,ogg,false,0,false,&ids[0]));
    int16_t output_samples[4096];
    for (auto& sample : output_samples) sample=1000;
    assert(a.QueueMusic(output_samples,4096,a.music_token_.load()));
    reject_output=true; tick(a); reject_output=false;
    assert(!a.music_active_.load() && !a.music_output_token_);
    assert(!a.PlaybackPeek(&a,&event) && event.instance==ids[0] && event.state==PXA_AUDIO_PLAYBACK_READY);
    assert(!a.PlaybackConsume(&a,&event));
    assert(!a.PlaybackPeek(&a,&event) && event.instance==ids[0] && event.state==PXA_AUDIO_PLAYBACK_ERROR && event.status==PXA_STATUS_IO_ERROR);
    assert(!a.PlaybackConsume(&a,&event));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(!budget_stats().charged[1]);
    decoder_mode=0;
    assert(!a.PlayMusic(&a,0,91,ogg,false,0,false,&ids[1]));
    try { a.RunMusic(); } catch(Idle&) {}
    assert(!a.PlaybackPeek(&a,&event) && event.instance==ids[1] && event.state==PXA_AUDIO_PLAYBACK_ERROR && event.status==PXA_STATUS_PROTOCOL_ERROR);
    a.PlaybackClose(&a,91);
    assert(a.PlaybackPeek(&a,&event)==PXA_STATUS_NOT_FOUND && !budget_stats().charged[1]);
    decoder_mode=1;
    for (pxa_status_t failure : {PXA_STATUS_DENIED,PXA_STATUS_NOT_FOUND,PXA_STATUS_CANCELLED}) {
        injected_read_error=failure;
        assert(!a.PlayMusic(&a,0,91,ogg,false,0,false,&ids[0]));
        const unsigned before=decoder_calls;
        try { a.RunMusic(); } catch(Idle&) {}
        assert(decoder_calls==before && !input_leases && !budget_stats().charged[1]);
        assert(!a.PlaybackPeek(&a,&event) && event.state==PXA_AUDIO_PLAYBACK_ERROR && event.status==failure);
        assert(!a.PlaybackConsume(&a,&event));
    }
    injected_read_error=PXA_STATUS_OK;
    assert(a.GetMusicStats(&after_buffer));
    assert(after_buffer.decode_calls==decoder_calls && after_buffer.decode_us==decoder_calls);
    assert(after_buffer.read_calls && after_buffer.read_bytes && after_buffer.read_max_bytes<=768);
    assert(after_buffer.read_max_us<=after_buffer.read_us && after_buffer.buffer.high_water<=8192);
    a.next_music_token_=UINT32_MAX;
    assert(a.PlayMusic(&a,0,91,ogg,false,0,false,&rejected)==PXA_STATUS_RESOURCE_LIMIT && !rejected);
    unlink(ogg);
    // Decoder EOF completion from an old generation cannot stop a new track.
    a.music_token_.store(2);a.music_voice_.store(1);a.music_active_.store(true);
    a.FinishMusic(1);assert(a.music_active_.load() && a.music_voice_.load()==1);
    a.FinishMusic(2);assert(!a.music_active_.load() && a.music_voice_.load()==-1);
    assert(!pxa_memory_owner_close(&resource_budget, owners[1]));
    const auto still_resident=budget_stats(owners[2]);
    assert(still_resident.charged[0] == device_resident.charged[0] &&
           still_resident.charged[1] == device_resident.charged[1]);
    rmdir(root);
    vQueueDelete(a.output_queue_);vQueueDelete(a.music_commands_);vSemaphoreDelete(a.music_mutex_);
    // Mock tasks never ran: dispose their board-lifetime backing only in this fixture.
    pxa_memory_release(a.music_ring_);
    pxa_memory_release(a.runtime_storage_);
    pxa_memory_release(a.music_stack_);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_VORBIS);esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS);
    assert(!esp_ogg_dec_unregister());
    assert(!live_queues && !live_mutexes && !pxa_memory_owner_close(&resource_budget, owners[2]));
    pxa_memory_stats_t final_stats;
    assert(!pxa_memory_budget_stats(&resource_budget,0,&final_stats));
    assert(!final_stats.charged[0] && !final_stats.charged[1]);
    puts("shared ESP output: bounded cache, total/temporary quota rollback/recovery, command replacement/cancel, prebuffer/EOF/underrun recovery/pause, decoder metrics and old/new owner isolation passed");
}
