// SPDX-License-Identifier: Apache-2.0
#include "pai_touch_voice_trigger.h"
#include "jl701_audio_link.h"
#include "rpc_wrap.h"
#include <pxa/pxa_voice_trigger.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdlib>
#include <cstring>

namespace {
constexpr char tag[] = "PaiVoiceTrigger";
constexpr unsigned capacity = 8;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
struct State {
    pxa_vt_profile_t profile{};
    pxa_vt_match_t queue[capacity]{};
    uint64_t sequence = 0, last_time = 0;
    uint32_t last_vendor_seq = 0, last_keyword = 0;
    unsigned head = 0, count = 0;
    uint32_t link_generation = 0;
    uint64_t next_probe_us = 0;
    bool probing = false;
    bool initialized = false, enabled = false, overflow = false, have_vendor_seq = false;
} state;

pxa_status_t Profile(void *, pxa_vt_profile_t *output) {
    portENTER_CRITICAL(&mutex);
    *output = state.profile;
    output->ready = output->ready && jl701_audio_link_ready();
    portEXIT_CRITICAL(&mutex);
    return PXA_STATUS_OK;
}
pxa_status_t Start(void *, uint32_t revision) {
    pxa_status_t status = PXA_STATUS_OK;
    portENTER_CRITICAL(&mutex);
    if (!state.profile.ready || !jl701_audio_link_ready()) status = PXA_STATUS_UNAVAILABLE;
    else if (state.profile.revision != revision) status = PXA_STATUS_BAD_STATE;
    else if (state.enabled) status = PXA_STATUS_BUSY;
    else {
        state.head = state.count = 0;
        state.overflow = false;
        state.enabled = true;
    }
    portEXIT_CRITICAL(&mutex);
    return status;
}
void Stop(void *) {
    portENTER_CRITICAL(&mutex);
    state.enabled = false;
    state.head = state.count = 0;
    state.overflow = false;
    portEXIT_CRITICAL(&mutex);
}
pxa_status_t Read(void *, pxa_vt_match_t *output) {
    pxa_status_t status = PXA_STATUS_WOULD_BLOCK;
    portENTER_CRITICAL(&mutex);
    if (!state.enabled) status = PXA_STATUS_BAD_STATE;
    else if (!jl701_audio_link_ready() || !state.profile.ready) status = PXA_STATUS_UNAVAILABLE;
    else if (state.overflow) status = PXA_STATUS_RESOURCE_LIMIT;
    else if (state.count) {
        *output = state.queue[state.head];
        state.head = (state.head + 1) % capacity;
        --state.count;
        status = PXA_STATUS_OK;
    }
    portEXIT_CRITICAL(&mutex);
    return status;
}

void Recognized(void *, esp_event_base_t, int32_t id, void *data) {
    if (!data) return;
    const char *word;
    size_t length;
    uint32_t vendor_seq = 0;
    const bool extended = id == VB_EVT_ASR_WORD_EX;
    if (extended) {
        const auto *event = static_cast<const event_asr_word_ex_t *>(data);
        word = event->word; length = event->word_len; vendor_seq = event->seq;
    } else {
        const auto *event = static_cast<const event_asr_word_t *>(data);
        word = event->word; length = event->word_len;
    }
    // Both vendor event formats have 32-byte inline word storage.
    if (length >= 32 || !pxa_vt_valid_word(word, length)) return;
    const auto now = static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&mutex);
    if (state.enabled && state.profile.ready && jl701_audio_link_ready()) {
        uint32_t keyword = 0;
        for (unsigned i = 0; i < state.profile.keyword_count; ++i) {
            const auto &candidate = state.profile.keywords[i];
            if (std::strlen(candidate.label) == length && !std::memcmp(candidate.label, word, length)) {
                keyword = candidate.id;
                break;
            }
        }
        // Ignore commands and unknown words, not just arbitrary ASR text.
        bool duplicate = false;
        if (extended) {
            duplicate = state.have_vendor_seq &&
                static_cast<int32_t>(vendor_seq - state.last_vendor_seq) <= 0;
            if (!duplicate) { state.last_vendor_seq = vendor_seq; state.have_vendor_seq = true; }
        }
        if (keyword && !duplicate &&
            !(keyword == state.last_keyword && now - state.last_time < 800000)) {
            state.last_keyword = keyword;
            state.last_time = now;
            if (state.count == capacity) state.overflow = true;
            else {
                state.queue[(state.head + state.count) % capacity] =
                    {++state.sequence, now, state.profile.revision, keyword};
                ++state.count;
            }
        }
    }
    portEXIT_CRITICAL(&mutex);
}
}

bool pai_touch_voice_trigger_initialize() {
    if (state.initialized) return true;
    state.profile.revision = 1;
    state.profile.flags = PXA_VT_FOREGROUND_ONLY;
    const pxa_vt_backend_t backend = {sizeof(pxa_vt_backend_t), nullptr, Profile, Start, Read, Stop};
    // The link creates the default ESP event loop before this call.
    if (esp_event_handler_register(RPC_VB_EVENT, VB_EVT_ASR_WORD, Recognized, nullptr) != ESP_OK)
        return false;
    if (esp_event_handler_register(RPC_VB_EVENT, VB_EVT_ASR_WORD_EX, Recognized, nullptr) != ESP_OK) {
        esp_event_handler_unregister(RPC_VB_EVENT, VB_EVT_ASR_WORD, Recognized);
        return false;
    }
    if (!pxa_esp_voice_trigger_set_backend(&backend)) {
        esp_event_handler_unregister(RPC_VB_EVENT, VB_EVT_ASR_WORD, Recognized);
        esp_event_handler_unregister(RPC_VB_EVENT, VB_EVT_ASR_WORD_EX, Recognized);
        return false;
    }
    state.initialized = true;
    return true;
}

void pai_touch_voice_trigger_link_lost() {
    portENTER_CRITICAL(&mutex);
    state.profile.ready = 0;
    state.head = state.count = 0;
    state.have_vendor_seq = false;
    state.last_time = 0;
    state.last_keyword = 0;
    ++state.link_generation;
    state.next_probe_us = 0;
    portEXIT_CRITICAL(&mutex);
}

namespace {
void DiscoverCatalog() {
    portENTER_CRITICAL(&mutex);
    const auto generation = state.link_generation;
    portEXIT_CRITICAL(&mutex);
    wake_word_list_t words{};
    const int result = rpc_get_wake_word_list(&words);
    pxa_vt_profile_t next{};
    next.flags = PXA_VT_FOREGROUND_ONLY;
    bool valid = result == RPC_ERR_SUCCESS && words.words_list && words.n_words;
    for (size_t i = 0; valid && i < words.n_words; ++i) {
        const auto &word = words.words_list[i];
        if (word.type != 0) continue; // WORD_TYPE_WAKEUP
        const size_t size = strnlen(word.word_name, sizeof(word.word_name));
        if (size >= sizeof(word.word_name) || !pxa_vt_valid_word(word.word_name, size) ||
            next.keyword_count == PXA_VT_MAX_KEYWORDS) { valid = false; break; }
        for (unsigned j = 0; j < next.keyword_count; ++j)
            if (!std::strcmp(next.keywords[j].label, word.word_name)) valid = false;
        if (!valid) break;
        auto &out = next.keywords[next.keyword_count++];
        out.id = static_cast<uint32_t>(next.keyword_count);
        std::memcpy(out.label, word.word_name, size + 1);
    }
    std::free(words.words_list);
    if (!valid || !next.keyword_count) {
        ESP_LOGW(tag, "Wake catalog unavailable: status=%d", result);
        return;
    }
    portENTER_CRITICAL(&mutex);
    if (state.link_generation != generation || !jl701_audio_link_ready()) {
        portEXIT_CRITICAL(&mutex);
        return;
    }
    const bool changed = state.profile.keyword_count != next.keyword_count ||
        std::memcmp(state.profile.keywords, next.keywords, sizeof(next.keywords));
    next.revision = state.profile.revision + (changed ? 1u : 0u);
    if (!next.revision) next.revision = 1;
    next.ready = 1;
    state.profile = next;
    portEXIT_CRITICAL(&mutex);
    ESP_LOGI(tag, "Wake catalog ready: words=%u revision=%lu playback_detection=unverified",
        static_cast<unsigned>(next.keyword_count), static_cast<unsigned long>(next.revision));
}
void DiscoverTask(void *) {
    DiscoverCatalog();
    portENTER_CRITICAL(&mutex);
    state.probing = false;
    portEXIT_CRITICAL(&mutex);
    vTaskDelete(nullptr);
}
}

void pai_touch_voice_trigger_refresh() {
    if (!state.initialized || !jl701_audio_link_ready()) return;
    const auto now = static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&mutex);
    const bool start = !state.profile.ready && !state.probing && now >= state.next_probe_us;
    if (start) { state.probing = true; state.next_probe_us = now + 30000000; }
    portEXIT_CRITICAL(&mutex);
    if (!start) return;
    // A slow or unsupported catalog RPC must not delay heartbeat, playback,
    // or the runtime. The worker releases its stack after one attempt.
    if (xTaskCreate(DiscoverTask, "jl701_words", 4096, nullptr, 3, nullptr) != pdPASS) {
        portENTER_CRITICAL(&mutex);
        state.probing = false;
        portEXIT_CRITICAL(&mutex);
        ESP_LOGW(tag, "Wake catalog worker unavailable");
    }
}
