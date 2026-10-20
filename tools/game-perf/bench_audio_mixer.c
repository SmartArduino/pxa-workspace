/* Native microbenchmark; this is not a device FPS or DAC underrun test.
 * Compile with libpxa's audio_mixer.c and -lm. Compare identical flags/inputs
 * against an earlier mixer revision; matching checksums guard the waveform. */
#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include "pxa/audio_mixer.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + time.tv_nsec;
}

int main(int argc, char **argv) {
    const unsigned rate = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 11025;
    const unsigned rounds = 20000, frames = rate / 50;
    if (rate < 8000 || rate > 48000) return 2;
    pxa_audio_mixer_t mixer;
    memset(&mixer, 0, sizeof(mixer));
    pxa_audio_mixer_voice_t *voice = &mixer.voices[0];
    pxa_audio_mixer_reset(voice);
    assert(pxa_audio_mixer_configure(voice, rate, 2, 48000) == PXA_STATUS_OK);
    pxa_audio_graph_t graph = {.route=PXA_AUDIO_ROUTE_SPEAKER, .gain_db_q8=-6 * 256};
    assert(pxa_audio_mixer_commit(voice, &graph) == PXA_STATUS_OK);
    uint8_t pcm[960 * 4];
    int16_t out[960];
    for (unsigned i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)(i * 13753u);
    uint64_t submit_ns = 0, render_ns = 0, samples = 0;
    uint32_t checksum = 2166136261u;
    for (unsigned round = 0; round < rounds; ++round) {
        const uint64_t begin = now_ns();
        assert(pxa_audio_mixer_write(voice, pcm, frames * 4) == PXA_STATUS_OK);
        const uint64_t submitted = now_ns();
        size_t count = 0;
        while (voice->count) {
            const size_t capacity = sizeof(out)/sizeof(*out) - count;
            assert(capacity);
            count += pxa_audio_mixer_render(&mixer, out + count,
                                           capacity < 320 ? capacity : 320);
        }
        const uint64_t rendered = now_ns();
        submit_ns += submitted - begin;
        render_ns += rendered - submitted;
        samples += count;
        for (size_t i = 0; i < count; ++i)
            checksum = (checksum ^ (uint16_t)out[i]) * 16777619u;
    }
    assert(voice->state.accepted_samples == (uint64_t)rounds * frames);
    printf("{\"source_rate\":%u,\"channels\":2,\"sink_rate\":48000,"
        "\"rounds\":%u,\"submit_ns\":%" PRIu64 ",\"render_ns\":%" PRIu64
        ",\"output_samples\":%" PRIu64 ",\"checksum\":%u,\"workspace\":%zu}\n",
        rate, rounds, submit_ns, render_ns, samples, checksum, sizeof(mixer));
    return 0;
}
