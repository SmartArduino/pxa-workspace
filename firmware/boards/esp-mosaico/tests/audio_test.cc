#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "esp_mosaico_audio.h"

namespace {
std::vector<std::unique_ptr<std::mutex>> locks;
std::atomic<unsigned> lock_requests{0}, writes{0}, reads{0}, enables{0}, disables{0};
std::atomic<unsigned> delay_ms{0};
std::atomic<int> pa{0};
std::atomic<bool> fail_write{false}, fail_reads{false};
std::mutex io_lock;
std::condition_variable io_changed;
bool block_write = false, write_entered = false, release_write = false;
bool block_preload = false, preload_entered = false, release_preload = false;

template <typename Predicate>
void WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
}

SemaphoreHandle_t xSemaphoreCreateMutex() {
    locks.push_back(std::make_unique<std::mutex>());
    return locks.back().get();
}
int xSemaphoreTake(SemaphoreHandle_t handle, uint32_t) {
    ++lock_requests;
    static_cast<std::mutex*>(handle)->lock();
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t handle) { static_cast<std::mutex*>(handle)->unlock(); }
void vTaskDelay(TickType_t ticks) { delay_ms += ticks; }
int gpio_set_level(gpio_num_t, int level) { pa = level; return ESP_OK; }
int gpio_get_level(gpio_num_t) { return pa; }
int gpio_config(const gpio_config_t*) { return ESP_OK; }
int i2s_new_channel(const i2s_chan_config_t*, i2s_chan_handle_t* output, void*) {
    *output = reinterpret_cast<void*>(1); return ESP_OK;
}
int i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t*) { return ESP_OK; }
int i2s_channel_register_event_callback(i2s_chan_handle_t, const i2s_event_callbacks_t*, void*) { return ESP_OK; }
int i2s_channel_enable(i2s_chan_handle_t) { ++enables; return ESP_OK; }
int i2s_channel_disable(i2s_chan_handle_t) { ++disables; return ESP_OK; }
int i2s_channel_preload_data(i2s_chan_handle_t, const void*, size_t size, size_t* count) {
    std::unique_lock<std::mutex> lock(io_lock);
    if (block_preload) {
        preload_entered = true;
        io_changed.notify_all();
        assert(io_changed.wait_for(lock, std::chrono::seconds(3), [] { return release_preload; }));
    }
    *count = size; return ESP_OK;
}
int i2s_channel_write(i2s_chan_handle_t, const void*, size_t size, size_t* count, unsigned) {
    ++writes;
    std::unique_lock<std::mutex> lock(io_lock);
    if (block_write) {
        write_entered = true;
        io_changed.notify_all();
        assert(io_changed.wait_for(lock, std::chrono::seconds(3), [] { return release_write; }));
    }
    *count = size;
    return fail_write ? ESP_FAIL : ESP_OK;
}
const audio_codec_data_if_t* audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t*) {
    static audio_codec_data_if_t data; return &data;
}
const audio_codec_gpio_if_t* audio_codec_new_gpio() { static audio_codec_gpio_if_t gpio; return &gpio; }
const audio_codec_ctrl_if_t* audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t*) {
    static audio_codec_ctrl_if_t control; return &control;
}
const audio_codec_if_t* es8311_codec_new(const es8311_codec_cfg_t*) { static audio_codec_if_t codec; return &codec; }
esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t*) { return reinterpret_cast<void*>(1); }
int esp_codec_dev_open(esp_codec_dev_handle_t, const esp_codec_dev_sample_info_t*) { return ESP_OK; }
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t, bool) { return ESP_OK; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int) { return ESP_OK; }
int esp_codec_dev_read_reg(esp_codec_dev_handle_t, int, int* output) {
    ++reads; *output = 0; return fail_reads ? ESP_FAIL : ESP_OK;
}
int esp_codec_dev_write(esp_codec_dev_handle_t, void*, size_t) { return ESP_OK; }
const char* esp_err_to_name(int) { return "test"; }

int main() {
    int16_t pcm[320] = {100};
    EspMosaicoAudio audio;
    assert(audio.Initialize(reinterpret_cast<void*>(1)));
    assert(pa == 1 && reads == 3);
    fail_reads = true;
    for (int i = 0; i < 10; ++i) audio.PublishDiagnostics();
    assert(reads == 3); // UI diagnostics cannot perform slow/failing codec I2C.
    assert(PxaAudioOutput::active->Emit(pcm, 320));
    audio.StopForShutdown();
    assert(pa == 0 && disables == 1);
    const unsigned previous_writes = writes, previous_delays = delay_ms;
    for (int i = 0; i < 100; ++i) assert(PxaAudioOutput::active->Emit(pcm, 320));
    assert(writes == previous_writes && pa == 0);
    assert(delay_ms == previous_delays + 2000); // Discarding after stop stays paced.

    EspMosaicoAudio concurrent;
    fail_reads = false;
    assert(concurrent.Initialize(reinterpret_cast<void*>(1)));
    block_write = true;
    fail_write = true;
    const unsigned before_enables = enables;
    bool write_result = true;
    std::thread writer([&] { write_result = PxaAudioOutput::active->Emit(pcm, 320); });
    {
        std::unique_lock<std::mutex> lock(io_lock);
        assert(io_changed.wait_for(lock, std::chrono::seconds(3), [] { return write_entered; }));
    }
    const unsigned before_locks = lock_requests;
    std::thread stopper([&] { concurrent.StopForShutdown(); });
    WaitUntil([&] { return lock_requests > before_locks; });
    {
        std::lock_guard<std::mutex> lock(io_lock);
        release_write = true;
    }
    io_changed.notify_all();
    writer.join(); stopper.join();
    assert(!write_result && enables == before_enables && pa == 0);
    const unsigned after_writes = writes;
    assert(PxaAudioOutput::active->Emit(pcm, 320));
    assert(writes == after_writes && pa == 0);
    // Also stop a recovery which passed its initial stopped check already.
    // Its PA enable must finish before StopForShutdown returns with PA off.
    block_write = false;
    EspMosaicoAudio recovering;
    assert(recovering.Initialize(reinterpret_cast<void*>(1)));
    block_preload = true;
    const unsigned recovery_enables = enables;
    std::thread recovery([&] { (void)PxaAudioOutput::active->Emit(pcm, 320); });
    {
        std::unique_lock<std::mutex> lock(io_lock);
        assert(io_changed.wait_for(lock, std::chrono::seconds(3), [] { return preload_entered; }));
    }
    const unsigned recovery_locks = lock_requests;
    std::thread stop_recovery([&] { recovering.StopForShutdown(); });
    WaitUntil([&] { return lock_requests > recovery_locks; });
    {
        std::lock_guard<std::mutex> lock(io_lock);
        release_preload = true;
    }
    io_changed.notify_all();
    recovery.join(); stop_recovery.join();
    assert(enables == recovery_enables + 1 && pa == 0);
    const unsigned recovered_writes = writes;
    assert(PxaAudioOutput::active->Emit(pcm, 320));
    assert(writes == recovered_writes && pa == 0);
    std::puts("Mosaico production audio: snapshot-only diagnostics, paced stop and write/recovery shutdown race passed.");
}
