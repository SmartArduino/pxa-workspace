#include "pxa/pxa_esp_security.h"
#include "pxa_esp_record_seal.h"
#include "bootloader_random.h"
#include "esp_random.h"
static pxa_status_t early_entropy(void *context,uint8_t key[32]) {
    (void)context;esp_fill_random(key,32);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_security_initialize_early(void) {
    pxa_status_t status=pxa_esp_record_seal_initialize(NULL,NULL);
    if(status!=PXA_STATUS_UNAVAILABLE)return status;
    bootloader_random_enable();
    status=pxa_esp_record_seal_initialize(early_entropy,NULL);
    bootloader_random_disable();
    return status;
}
