#ifndef PXA_ESP_STORAGE_H
#define PXA_ESP_STORAGE_H
#include "pxa/volume.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PXA_ESP_STORAGE_MAX_VOLUMES 8u
#define PXA_ESP_STORAGE_PATH_BYTES 256u
/* Board bring-up only, after a successful link-free FAT mount. Registers a
 * read-only Guest data source. Does not change the internal app/state root.
 * A known filesystem serial binds grants to this format as well as the card.
 * There is no hotplug claim; late registration is rejected after activation. */
pxa_status_t pxa_esp_storage_register_fat(const char *path,const char *name,
    const void *device_identity,size_t identity_bytes,uint32_t filesystem_serial,
    uint8_t serial_known,uint64_t total_bytes,uint64_t free_bytes,uint8_t space_known);
size_t pxa_esp_storage_volumes(pxa_volume_info_t *out,size_t capacity);
/* Explicit board policy, never enumeration order. Before app activation. */
pxa_status_t pxa_esp_storage_select_app_media(const char *registered_mount);
/* PXADB Host tooling only. Validates a descendant on a registered removable
 * volume and optionally returns a staging path on that SAME filesystem.
 * PXA/system descendants require the PXADB system unlock; the system root,
 * PXA and the mount point stay in place. Other card paths need no unlock. */
bool pxa_esp_storage_debug_path(const char *relative,bool system_unlocked,
    char *temporary,size_t capacity);
/* Trusted system picker only; these paths are never returned to Guest. */
typedef struct { char name[PXA_ESP_STORAGE_PATH_BYTES]; } pxa_esp_storage_folder_t;
pxa_status_t pxa_esp_storage_folders(uint64_t epoch,const char *relative,
    uint32_t skip,pxa_esp_storage_folder_t *out,size_t capacity,
    size_t *count,uint8_t *more);
bool pxa_esp_storage_choose_directory(uint32_t prompt_id,uint64_t epoch,
    const char *relative);
#ifdef __cplusplus
}
#endif
#endif
