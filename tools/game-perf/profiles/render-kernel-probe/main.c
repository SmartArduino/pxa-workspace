#include "pxa.h"
#include "pxa_event_port.h"
#include "pxa_ui.h"
#include "pxa_window.h"
#include "pxa_game_render_spans.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t events[PXA_MAX_CONTROL_BYTES], packet[512];
static uint64_t raster;
static unsigned completed;
static unsigned pointers, controllers, transitions, heartbeats, background;
static uint32_t last_controller_buttons = UINT32_MAX;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (unsigned)t.tv_nsec / 1000;
}

static void note(const char *kind, int value) {
    char message[160];
    snprintf(message, sizeof(message), "MAINLOOP %s value=%d heartbeat=%u pointer=%u controller=%u lifecycle=%u background=%u",
             kind, value, heartbeats, pointers, controllers, transitions, background);
    pxa_log_write(2, message);
}

static void dispatch(const pxa_event_t *event) {
    pxa_game_render_raster_result_t created;
    if (pxa_game_render_parse_raster(event, 30, &created)) {
        if (created.status) __builtin_trap();
        raster = created.handle;
        return;
    }
    uint8_t state;
    pxa_ui_pointer_data_t pointer;
    pxa_ui_controller_data_t controller;
    if (pxa_lifecycle_parse(event, &state)) {
        background = state == PXA_SYSTEM_LIFECYCLE_BACKGROUND;
        ++transitions; note("lifecycle", state);
    } else if (pxa_ui_parse_pointer(event, &pointer)) {
        ++pointers; note("pointer", pointer.phase);
    } else if (pxa_ui_parse_controller(event, &controller)) {
        ++controllers;
        /* Count every state refresh, but log edges rather than 20 identical
         * idle records each second. Keep the bounded device log queue useful. */
        if (controller.buttons != last_controller_buttons) {
            last_controller_buttons = controller.buttons;
            note("controller", (int)controller.buttons);
        }
    }
}

int32_t pxa_app_start(const uint8_t *config, uint32_t size) {
    (void)config; (void)size;
    pxa_ui_transaction_t tx = {0};
    if (!pxa_ui_transaction_begin(&tx, 1, PXA_UI_TRANSACTION_REPLACE_SURFACE, packet, sizeof(packet)) ||
        !pxa_ui_create(&tx, 1, 0, 0, PXA_UI_NODE_ROOT) ||
        !pxa_ui_set_rgba(&tx, 1, PXA_UI_PROPERTY_BACKGROUND, 0x16445aff) ||
        !pxa_ui_set_event_mask(&tx, 1, PXA_UI_EVENT_MASK_POINTER | PXA_UI_EVENT_MASK_CONTROLLER_STATE) ||
        !pxa_ui_create(&tx, 2, 1, 0, PXA_UI_NODE_TEXT) ||
        !pxa_ui_set_text(&tx, 2, "Raster kernel probe", 19) ||
        !pxa_ui_transaction_commit(&tx)) return PXA_STATUS_INTERNAL;
    (void)pxa_window_fullscreen();
    note("start", 0);
    if (pxa_game_render_create_raster(30, 256)) return PXA_STATUS_INTERNAL;
    return pxa_event_port_open(31);
}

static uint8_t target[320*240], texture[64*80], colormap[65536];
static uint32_t lights[18*18];
static uint8_t commands[PXA_GAME_RENDER_SPANS_HEADER_BYTES + 240*PXA_GAME_RENDER_SPAN_BYTES];
static void checkpoint(uint64_t port) {
    int count = pxa_event_port_wait(port, events, sizeof(events), 0);
    if (count > 0) {
        pxa_event_t event;
        if (!pxa_parse_event(events, (unsigned)count, &event)) __builtin_trap();
        dispatch(&event);
    } else if (count != PXA_STATUS_WOULD_BLOCK && count != PXA_STATUS_TIMED_OUT) __builtin_trap();
}
static void run_benchmark(uint64_t port) {
    for (unsigned i=0;i<sizeof(texture);++i) texture[i]=(i*37u+(i>>6)*19u)&255;
    for (unsigned i=0;i<sizeof(colormap);++i) colormap[i]=(i*23u+(i>>8)*71u)&255;
    for (unsigned i=0;i<18*18;++i) lights[i]=(i*97u%4096u)|((i*31u%4096u)<<16);
    for (unsigned round=0;round<3;++round) for(unsigned scene=0;scene<8;++scene) {
        memset(target,0xa5,sizeof(target));
        const unsigned mode=scene%4, pow2=scene<4, count=mode==3?3:256;
        pxa_game_render_spans_header_wire_t h={0};
        h.target_offset=(uintptr_t)target;h.target_bytes=sizeof(target);
        h.texture_offset=(uintptr_t)texture;h.texture_width=pow2?64:48;h.texture_height=pow2?64:80;
        h.texture_bytes=h.texture_width*h.texture_height;
        h.light_offset=(uintptr_t)lights;h.light_bytes=sizeof(lights);h.light_width=h.light_height=18;
        h.colormap_offset=(uintptr_t)colormap;h.colormap_bytes=sizeof(colormap);h.colormap_levels=256;
        h.wrap_s=UINT32_MAX/h.texture_width+1;h.wrap_t=UINT32_MAX/h.texture_height+1;
        h.light_shift=4;h.flags=(pow2?PXA_GAME_RENDER_SPANS_POW2:0)|(mode==2?PXA_GAME_RENDER_SPANS_FULLBRIGHT:0);
        pxa_game_render_span_batch_t batch;
        pxa_game_render_span_batch_begin(&batch,commands,sizeof(commands),&h);
        for(unsigned y=0;y<240;++y) {
            pxa_game_render_span_wire_t row={y*320,count,0,mode==1?0:(int32_t)(y*65536u),65536,mode==1?65536:0};
            if(!pxa_game_render_span_append(&batch,&row)) __builtin_trap();
        }
        pxa_game_render_span_telemetry_t before,after;
        for(unsigned frame=0;frame<10;++frame) {
            if(pxa_game_render_execute_spans(raster,&batch)<0) __builtin_trap();
            checkpoint(port);
        }
        if(pxa_game_render_query_span_telemetry(raster,&before)) __builtin_trap();
        uint64_t started=now_us();
        for(unsigned frame=0;frame<100;++frame) {
            if(pxa_game_render_execute_spans(raster,&batch)<0) __builtin_trap();
            checkpoint(port);
        }
        uint64_t elapsed=now_us()-started;
        if(pxa_game_render_query_span_telemetry(raster,&after)) __builtin_trap();
        uint32_t hash=2166136261u;
        for(unsigned i=0;i<sizeof(target);++i) hash=(hash^target[i])*16777619u;
        char message[256];
        snprintf(message,sizeof(message),"RENDERBENCH round=%u scene=%u batches=%llu rejected=%llu pixels=%llu validate_us=%llu execute_us=%llu wall_us=%llu hash=%08x",
            round,scene,(unsigned long long)(after.batches-before.batches),
            (unsigned long long)(after.rejected-before.rejected),(unsigned long long)(after.pixels-before.pixels),
            (unsigned long long)(after.validation_us-before.validation_us),
            (unsigned long long)(after.execute_us-before.execute_us),(unsigned long long)elapsed,hash);
        pxa_log_write(2,message);
        checkpoint(port);
    }
    pxa_log_write(2,"RENDERBENCH completed=24");
}

int32_t pxa_app_on_event(const uint8_t *data, uint32_t size) {
    pxa_event_t event;
    if (!pxa_parse_event(data, size, &event)) return PXA_STATUS_INTERNAL;
    pxa_event_port_result_t opened;
    if (!pxa_event_port_parse_open(&event, 31, &opened)) {
        dispatch(&event); return PXA_EVENT_HANDLED;
    }
    if (opened.status) return opened.status;
    /* This stack object must survive every native wait. The Host never calls
     * this export recursively while the retained call is running. */
    volatile uint8_t sentinel[128];
    for (unsigned i = 0; i < sizeof(sentinel); ++i) sentinel[i] = 0xa7;
    uint64_t next_heartbeat = now_us();
    note("entered", 0);
    for (;;) {
        if (raster && !background && !completed) {
            completed=1;run_benchmark(opened.handle);
        }
        int count = pxa_event_port_wait(opened.handle, events, sizeof(events), 100000);
        for (unsigned i = 0; i < sizeof(sentinel); ++i)
            if (sentinel[i] != 0xa7) return PXA_STATUS_INTERNAL;
        if (count > 0) {
            if (!pxa_parse_event(events, (unsigned)count, &event)) return PXA_STATUS_INTERNAL;
            dispatch(&event);
        } else if (count != PXA_STATUS_TIMED_OUT && count != PXA_STATUS_WOULD_BLOCK) {
            note("wait-error", count); return count;
        }
        uint64_t now = now_us();
        if (now >= next_heartbeat) {
            ++heartbeats; note("alive", 0); next_heartbeat = now + 1000000;
        }
    }
}

void pxa_app_stop(uint32_t reason) { (void)reason; }
