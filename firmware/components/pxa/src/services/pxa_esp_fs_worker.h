// SPDX-License-Identifier: Apache-2.0
#ifndef PXA_ESP_FS_WORKER_H
#define PXA_ESP_FS_WORKER_H
#include "pxa/fs.h"
pxa_status_t pxa_esp_fs_worker_begin(void (*notify)(void *), void *context);
pxa_status_t pxa_esp_fs_worker_submit(void *, pxa_fs_async_work_fn, void *);
void pxa_esp_fs_worker_end(void);
#endif
