#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <driver/uart.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The pai-touch speaker hangs off a JL701 coprocessor: the ESP32 sends framed
 * RPC packets over one UART and the JL701 decodes and amplifies them. This
 * link owns that transport, the keep-alive handshake and the PCM cadence the
 * JL701 decoder expects. It intentionally exposes audio output only, so the
 * board's PXA audio sink never sees the vendor RPC stack.
 *
 * The coprocessor sleeps on its own once keep_alive_ms passes without a
 * heartbeat and only answers again after its RX line has been held low, so the
 * link pulses TX and re-runs the handshake whenever the link goes down. */
typedef struct {
    uart_port_t uart_num;
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    int baud_rate;
    /* Amplifier enable pin on the JL701 side; negative pa_io disables it.
     * Keep it disabled: the coprocessor stops answering the handshake when it
     * is asked to drive the pin (see pai_touch_config.h). */
    int pa_io;
    int pa_en_level;
} jl701_audio_link_config_t;

/* Starts the UART transport and the first handshake. Returns false when the
 * transport cannot be brought up; the keep-alive task still retries the
 * handshake in the background, so a later PCM write can succeed. */
bool jl701_audio_link_start(const jl701_audio_link_config_t* config);

/* True while the JL701 is initialized and has not been declared dead by the
 * keep-alive task. */
bool jl701_audio_link_ready(void);

/* Records the requested level. The next PCM write forwards it to the JL701 so
 * every request that shares the transport stays on the audio writer's task. */
void jl701_audio_link_set_volume(uint8_t percent);

/* Streams mono 16 kHz PCM while pacing the packets at the rate the JL701
 * decoder consumes them. Blocks for the duration of the audio and returns
 * false when the link is down or a packet could not be queued. */
bool jl701_audio_link_write_pcm(const int16_t* pcm, size_t samples);

#ifdef __cplusplus
}
#endif
