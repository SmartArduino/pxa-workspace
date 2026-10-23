#include "kernels.h"
#include "wasm_export.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_private/esp_clk.h"
#include "esp_memory_utils.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern const uint8_t aot_start[] asm("_binary_bench_aot_start");
extern const uint8_t aot_end[] asm("_binary_bench_aot_end");
static wasm_module_inst_t instance;
static wasm_exec_env_t exec_env;
static uint8_t *data;
#define ITEM(name) {#name, name},
static const struct { const char *name; bench_kernel_t native; } kernels[] = {
    BENCH_KERNELS(ITEM)
};
#undef ITEM
static __attribute__((noreturn)) void fail(const char *message) {
    printf("BENCH_ERROR %s\n", message); fflush(stdout);
    abort();
}
static void initialize_data(void) {
    uint32_t x = 0x12345678U;
    for (uint32_t i = 0; i < BENCH_BYTES; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        data[i] = (uint8_t)x;
    }
}
static uint32_t data_hash(void) {
    uint32_t x = 2166136261U;
    for (uint32_t i = 0; i < BENCH_BYTES; ++i) x = (x ^ data[i]) * 16777619U;
    return x;
}
typedef struct { int64_t us; uint32_t cycles, value, memory_hash; } sample_t;
static sample_t measure(unsigned index, wasm_function_inst_t fn,
                        uint32_t count, int aot) {
    initialize_data();
    uint32_t args[] = {BENCH_OFFSET, count, 0x31415926U};
    int64_t begin = esp_timer_get_time();
    uint32_t cycles = esp_cpu_get_cycle_count();
    uint32_t value;
    if (aot) {
        if (!wasm_runtime_call_wasm(exec_env, fn, 3, args))
            fail(wasm_runtime_get_exception(instance));
        value = args[0];
    } else value = kernels[index].native(data, count, args[2]);
    cycles = esp_cpu_get_cycle_count() - cycles;
    int64_t duration = esp_timer_get_time() - begin;
    return (sample_t){duration, cycles, value, data_hash()};
}
static void *benchmark(void *unused) {
    (void)unused;
    esp_chip_info_t chip; esp_chip_info(&chip);
    printf("BENCH_META {\"target\":\"%s\",\"idf\":\"%s\",\"gcc\":\"%s\","
           "\"cpu_mhz\":%d,\"core\":%d,\"psram_total\":%u,\"repeats\":7,"
           "\"native_code\":\"flash\",\"aot_code\":\"%s\",\"aot_bytes\":%u}\n",
           CONFIG_IDF_TARGET, esp_get_idf_version(), __VERSION__,
           esp_clk_cpu_freq()/1000000, xPortGetCoreID(),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
#ifdef CONFIG_WAMR_AOT_CODE_IN_PSRAM
           "psram",
#else
           "internal_ram",
#endif
           (unsigned)(aot_end-aot_start));
    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) fail("runtime init");
    size_t bytes = (size_t)(aot_end-aot_start);
    uint8_t *binary = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!binary) fail("binary allocation");
    memcpy(binary, aot_start, bytes);
    char error[256] = {0};
    wasm_module_t module = wasm_runtime_load(binary, bytes, error, sizeof(error));
    if (!module) fail(error);
    instance = wasm_runtime_instantiate(module, 32768, 0, error, sizeof(error));
    if (!instance) fail(error);
    exec_env = wasm_runtime_create_exec_env(instance, 32768);
    if (!exec_env || !wasm_runtime_validate_app_addr(instance, BENCH_OFFSET, BENCH_BYTES))
        fail("linear memory setup");
    data = wasm_runtime_addr_app_to_native(instance, BENCH_OFFSET);
    if (!esp_ptr_external_ram(data)) fail("both paths must use the same PSRAM data");
    for (unsigned k = 0; k < sizeof(kernels)/sizeof(kernels[0]); ++k) {
        wasm_function_inst_t fn = wasm_runtime_lookup_function(instance, kernels[k].name);
        if (!fn) fail("missing kernel export");
        uint32_t count = 1024;
        while (count < 4194304U) {
            sample_t pilot = measure(k, fn, count, 0);
            vTaskDelay(1);
            if (pilot.us >= 10000) break;
            count *= 2;
        }
        /* Untimed warm-up of each code path before the paired samples. */
        (void)measure(k, fn, count, 0); vTaskDelay(1);
        (void)measure(k, fn, count, 1); vTaskDelay(1);
        for (unsigned repeat = 0; repeat < 7; ++repeat) {
            sample_t native, aot;
            if (repeat & 1U) {
                aot = measure(k, fn, count, 1); vTaskDelay(1);
                native = measure(k, fn, count, 0);
            } else {
                native = measure(k, fn, count, 0); vTaskDelay(1);
                aot = measure(k, fn, count, 1);
            }
            if (native.value != aot.value || native.memory_hash != aot.memory_hash)
                fail("native/AOT output mismatch");
            printf("BENCH_ROW {\"kernel\":\"%s\",\"round\":%u,\"iterations\":%" PRIu32 ","
                   "\"native_us\":%" PRId64 ",\"aot_us\":%" PRId64 ","
                   "\"native_cycles\":%" PRIu32 ",\"aot_cycles\":%" PRIu32 ",\"checksum\":%" PRIu32 ","
                   "\"memory_hash\":%" PRIu32 "}\n", kernels[k].name, repeat, count,
                   native.us, aot.us, native.cycles, aot.cycles,
                   native.value, native.memory_hash);
            fflush(stdout); vTaskDelay(1);
        }
    }
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
    wasm_runtime_destroy(); free(binary);
    puts("BENCH_DONE"); fflush(stdout);
    return NULL;
}
void app_main(void) {
    /* Delay permits capture after the programmer releases the UART/JTAG port. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 65536; cfg.prio = 5; cfg.pin_to_core = 1;
    cfg.thread_name = "native_aot";
    if (esp_pthread_set_cfg(&cfg) != ESP_OK) fail("pthread configuration");
    pthread_t thread;
    if (pthread_create(&thread, NULL, benchmark, NULL) != 0)
        fail("task allocation");
    pthread_detach(thread);
}
