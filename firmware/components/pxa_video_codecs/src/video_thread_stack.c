// SPDX-License-Identifier: Apache-2.0
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include <string.h>

esp_err_t __real_esp_pthread_set_cfg(const esp_pthread_cfg_t *cfg);

esp_err_t __wrap_esp_pthread_set_cfg(const esp_pthread_cfg_t *cfg)
{
    if (!cfg || !cfg->thread_name) {
        return __real_esp_pthread_set_cfg(cfg);
    }

    // On S31 the ordinary internal heap may include LP SRAM. PIE vector
    // stores require a stack in high-performance SRAM or external RAM.
    const uint32_t main_sram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    esp_pthread_cfg_t task = *cfg;
    if (!strcmp(cfg->thread_name, "oh264_worker")) {
        task.stack_alloc_caps = main_sram;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        if (heap_caps_get_largest_free_block(main_sram) < cfg->stack_size + 256)
            task.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    } else if (cfg->pin_to_core == 1 && !strcmp(cfg->thread_name, "pxa_runtime")) {
        // Host imports on the SIMD core can also call PIE. Package and
        // settings operations require this stack to remain flash-safe.
        task.stack_alloc_caps = main_sram;
    } else {
        return __real_esp_pthread_set_cfg(cfg);
    }
    return __real_esp_pthread_set_cfg(&task);
}
