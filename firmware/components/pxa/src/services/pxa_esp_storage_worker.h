// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_ESP_STORAGE_WORKER_H
#define PXA_ESP_STORAGE_WORKER_H
#include "pxa/storage.h"
pxa_status_t pxa_esp_storage_worker_begin(void (*notify)(void *), void *context);
pxa_status_t pxa_esp_storage_worker_submit(void *context,
    pxa_storage_work_fn work, void *work_context);
/* Runtime-owner only. Drain accepted writes before releasing their activation,
 * even after the runtime itself has stopped. Does not publish completions. */
void pxa_esp_storage_worker_end(void);
#endif
