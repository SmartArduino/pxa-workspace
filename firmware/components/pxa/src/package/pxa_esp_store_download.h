#ifndef PXA_ESP_STORE_DOWNLOAD_H
#define PXA_ESP_STORE_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pxa_esp_package_store.h"
#include "pxa_esp_install_approval.h"
#include "pxa/wire.h"

#define PXA_STORE_INSTALL_SERVICE_ID UINT16_C(20)
#define PXA_STORE_INSTALL_SERVICE_MAJOR UINT16_C(0)
#define PXA_STORE_INSTALL_SERVICE_MINOR UINT16_C(8)
#define PXA_STORE_INSTALL_REQUEST UINT16_C(1)
#define PXA_STORE_DOWNLOAD_REQUEST UINT16_C(2)
#define PXA_STORE_INSTALL_FILE_REQUEST UINT16_C(3)
#define PXA_STORE_INSTALLED_LIST_REQUEST UINT16_C(4)
#define PXA_STORE_DOWNLOAD_LIST_REQUEST UINT16_C(5)
#define PXA_STORE_DELETE_REQUEST UINT16_C(6)
#define PXA_STORE_LAUNCH_REQUEST UINT16_C(7)
#define PXA_STORE_UNINSTALL_REQUEST UINT16_C(8)
#define PXA_STORE_LAUNCH_IDENTITY_REQUEST UINT16_C(9)
#define PXA_STORE_UNINSTALL_IDENTITY_REQUEST UINT16_C(10)
#define PXA_STORE_REGISTER_NAMESPACE_REQUEST UINT16_C(11)
#define PXA_STORE_INSTALL_IDENTITY_REQUEST UINT16_C(12)
#define PXA_STORE_CAPABILITIES_REQUEST UINT16_C(13)
#define PXA_STORE_CAP_RESOURCE_INSTALL UINT32_C(1)
#define PXA_STORE_CAP_VOLUME_PLACEMENT UINT32_C(2)
#define PXA_STORE_CAP_INDEXED_CONTAINER UINT32_C(8)
#define PXA_STORE_DOWNLOAD_PROGRESS UINT16_C(0x8001)
/* The selected filesystem and its signed install plan determine capacity. */
#define PXA_STORE_DOWNLOAD_MAX_BYTES UINT64_C(0x7fffffff)
#define PXA_STORE_WORKER_STACK_BYTES (10u * 1024u)

typedef struct pxa_esp_store_job {
    pxa_request_token_t request_token;
    uint32_t component;
    uint32_t prompt_id;
    uint64_t instance_id;
    uint64_t size;
    uint64_t downloaded_bytes;
    uint64_t total_bytes;
    uint8_t digest[32];
    uint8_t expected_root[32],has_expected_root;
    char app_id[65];
    char ticket[256];
    char filename[24];
    char path[384];
    char resource_path[384];
    char resource_url[512];
    uint64_t cache_epoch;
    pxa_esp_install_approval_t placement;
    pxa_esp_package_preview_t preview;
    TaskHandle_t worker;
    uint8_t approved;
    uint8_t separate;
    uint8_t ready;
    uint8_t keep_file;
    uint8_t reported_percent;
    uint8_t cancelled;
    int32_t status;
    void (*notify)(struct pxa_esp_store_job *job, uint8_t phase);
} pxa_esp_store_job_t;

int pxa_esp_store_job_decode(pxa_esp_store_job_t *job,
                             const uint8_t *payload, size_t size);
int pxa_esp_store_job_decode_identity(pxa_esp_store_job_t *,const uint8_t *,size_t);
void pxa_esp_store_worker(void *context);

#endif
