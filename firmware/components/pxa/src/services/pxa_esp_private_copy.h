#ifndef PXA_ESP_PRIVATE_COPY_H
#define PXA_ESP_PRIVATE_COPY_H
#include "pxa/status.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t allocation_unit;
    uint64_t max_file_bytes,max_logical_bytes,max_allocated_bytes;
    void *context;
    /* Original mount-epoch validation, cancellation and cooperative yield.
     * Called before each directory entry and bounded file I/O step. */
    pxa_status_t (*check)(void *);
} pxa_esp_private_copy_config_t;
typedef struct {
    uint64_t logical_bytes,allocated_bytes;
    uint32_t files,directories;
} pxa_esp_private_copy_info_t;

/* Host-only idle-source migration. Caller supplies aligned PSRAM scratch;
 * no tree-sized allocation, recursive SRAM frames or unbounded open dirs.
 * Paths are canonical private roots from authenticated storage sessions.
 * The source is never changed. Destination must be fresh and unpublished;
 * failures leave an owned partial destination for coordinator cleanup.
 * Root .pxa-policy and .pxa-directory-grants are omitted and must be verified
 * and resealed separately before the installation's handoff gate commits. */
size_t pxa_esp_private_copy_workspace_size(void);
pxa_status_t pxa_esp_private_copy_measure(const char *source,
    const pxa_esp_private_copy_config_t *,void *workspace,size_t,
    pxa_esp_private_copy_info_t *);
/* Admission includes control files that will be authenticated/resealed by
 * the authority copier, so retained grants cannot exceed the reservation. */
pxa_status_t pxa_esp_private_copy_measure_install(const char *source,
    const pxa_esp_private_copy_config_t *,void *workspace,size_t,
    pxa_esp_private_copy_info_t *);
/* Copies and rereads each file, checking its exact size and SHA-256. Names,
 * empty directories and case distinctions are preserved. Never merges an
 * existing destination or follows symlinks/hardlinks/special files. */
pxa_status_t pxa_esp_private_copy_tree(const char *source,const char *destination,
    const pxa_esp_private_copy_config_t *,void *workspace,size_t,
    pxa_esp_private_copy_info_t *);
#endif
