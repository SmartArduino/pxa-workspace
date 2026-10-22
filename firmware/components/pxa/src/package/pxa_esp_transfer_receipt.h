#ifndef PXA_ESP_TRANSFER_RECEIPT_H
#define PXA_ESP_TRANSFER_RECEIPT_H
#include "pxa/install_transfer.h"
#include <stddef.h>
#define PXA_ESP_TRANSFER_SOURCE UINT8_C(1)
#define PXA_ESP_TRANSFER_TARGET UINT8_C(2)
#define PXA_ESP_TRANSFER_CONTEXT_BYTES 512u
#define PXA_ESP_TRANSFER_SLOT_0 ".transfer.0.pxm"
#define PXA_ESP_TRANSFER_SLOT_1 ".transfer.1.pxm"
typedef struct {
    int directory;
    uint64_t mount_epoch;
    uint8_t role;
    const char *locator; /* Canonical signed publisher digest / App ID. */
} pxa_esp_transfer_receipt_config_t;
typedef struct {
    pxa_install_transfer_t transfer;
    uint16_t context_size;
    /* Host backend recovery context, e.g. bounded legacy source locators.
     * Opaque here; the owner validates its schema before acting on it. */
    uint8_t context[PXA_ESP_TRANSFER_CONTEXT_BYTES];
} pxa_esp_transfer_receipt_t;
/* Caller owns a no-follow directory and a lease on that same physical mount.
 * Source receipts/retired homes belong below protected PXA/apps/.transfers;
 * target receipts belong in the unpublished canonical target App home.
 * Scratch must be aligned PSRAM and distinct from config/input/output. */
size_t pxa_esp_transfer_receipt_workspace_size(void);
pxa_status_t pxa_esp_transfer_receipt_load(const pxa_esp_transfer_receipt_config_t *,
    void *workspace,size_t,pxa_esp_transfer_receipt_t *);
/* CAS against revision zero (absent) or the exact authenticated revision.
 * Only phase/revision may change; opaque context is immutable. Success means
 * both slots are durable. durable=1 also identifies a known committed value
 * when subsequent mirroring fails; reload before retry. durable=0 on an I/O
 * error does NOT prove rollback: rename/directory-sync can be uncertain. */
pxa_status_t pxa_esp_transfer_receipt_store(const pxa_esp_transfer_receipt_config_t *,
    uint64_t expected_revision,const pxa_esp_transfer_receipt_t *,
    void *workspace,size_t,uint8_t *durable);
#endif
