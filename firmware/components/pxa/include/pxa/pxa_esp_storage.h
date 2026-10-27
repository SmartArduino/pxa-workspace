#ifndef PXA_ESP_STORAGE_H
#define PXA_ESP_STORAGE_H
#include "pxa/volume.h"
#include "pxa/install_plan.h"
#include "pxa/wire.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PXA_ESP_STORAGE_MAX_VOLUMES 8u
#define PXA_ESP_STORAGE_PATH_BYTES 256u
typedef pxa_status_t (*pxa_esp_storage_space_fn)(void *context,
    const char *mount_path,uint64_t *total,uint64_t *free_bytes,uint8_t *known);
typedef struct {
    uint32_t struct_size;
    const char *mount_path,*name;
    const void *device_identity;
    size_t device_identity_bytes;
    /* Required only for read-only media without a PXA volume descriptor.
     * Include the partition identity; separate partitions must not alias. */
    const void *filesystem_identity;
    size_t filesystem_identity_bytes;
    uint64_t capabilities,total_bytes,free_bytes,max_file_bytes;
    uint32_t allocation_unit;
    uint8_t space_known,link_free;
    void *context;
    pxa_esp_storage_space_fn query_space;
    /* Optional for filesystems needing an explicit metadata barrier.
     * Link-free LittleFS/FAT commit directory changes in their VFS calls. */
    int (*sync_directory)(void *,const char *path);
} pxa_esp_storage_volume_config_t;
/* Physical filesystem must already be mounted. Persistent UUID mirrors live
 * in its own PXA directory, without NVS or a primary-volume registry. The
 * mount owner serializes registration/drain/unmount with application
 * lifecycle: drain stops new access, then close all users before unregister
 * and physically unmount. Mounting late does not require recreating Guests. */
pxa_status_t pxa_esp_storage_register_volume(
    const pxa_esp_storage_volume_config_t *,uint64_t *mount_epoch);
pxa_status_t pxa_esp_storage_refresh_volume(uint64_t mount_epoch);
pxa_status_t pxa_esp_storage_drain_volume(uint64_t mount_epoch);
pxa_status_t pxa_esp_storage_unregister_volume(uint64_t mount_epoch);
pxa_status_t pxa_esp_storage_volume(uint64_t mount_epoch,pxa_volume_info_t *);
pxa_status_t pxa_esp_storage_find_volume(const pxa_volume_id_t *,pxa_volume_info_t *);
/* Cached Host lookup of a normalized native path. Uses the most specific
 * mount, including private aliases, and refuses a stale/ambiguous child
 * instead of falling through to its parent. No filesystem I/O. */
pxa_status_t pxa_esp_storage_resolve_volume(const char *,pxa_volume_info_t *);
/* Cached device overview, never admission. Unknown capacity or overflow
 * returns an error with zeroed outputs; private aliases are not counted. */
pxa_status_t pxa_esp_storage_usage(uint64_t *total,uint64_t *free_bytes,size_t *volumes);
/* Copies a Host-only path after checking this precise mount instance. */
pxa_status_t pxa_esp_storage_volume_path(uint64_t mount_epoch,char *,size_t);
/* app_epoch=0 selects a fitting volume; media_epoch=0 follows the app.
 * Capacity is refreshed before planning and again before reservation. */
pxa_status_t pxa_esp_storage_plan(const pxa_install_requirements_t *,
    uint64_t app_epoch,uint64_t media_epoch,pxa_install_plan_t *);
pxa_status_t pxa_esp_storage_reserve_plan(pxa_install_plan_t *);
pxa_status_t pxa_esp_storage_release_plan(pxa_install_plan_t *);
/* Convenience registration for a writable link-free FAT mount. */
pxa_status_t pxa_esp_storage_register_fat(const char *path,const char *name,
    const void *device_identity,size_t identity_bytes,
    uint64_t total_bytes,uint64_t free_bytes,uint8_t space_known,
    uint32_t allocation_unit);
size_t pxa_esp_storage_volumes(pxa_volume_info_t *out,size_t capacity);
/* Explicit board policy, never enumeration order. Before app activation. */
pxa_status_t pxa_esp_storage_select_app_media(const char *registered_mount);
/* Selected app-media mount, relative to the PXADB storage root; an empty
 * successful result denotes that root itself (for example an SD-only boot).
 * Read-only;
 * UNAVAILABLE means no usable selected volume, never a directory fallback. */
pxa_status_t pxa_esp_storage_media_mount(char *output,size_t capacity);
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
/* Prompt-scoped trusted browser. Uses the requesting app's protected paths,
 * no-link checks and exact mount epoch; neither call creates a grant. */
typedef pxa_status_t (*pxa_esp_directory_entry_fn)(void *,size_t,uint8_t,pxa_bytes_t);
pxa_status_t pxa_esp_storage_directory_entries(uint32_t prompt_id,uint64_t epoch,
    const char *relative,size_t skip,size_t capacity,size_t *count,uint8_t *more,
    pxa_esp_directory_entry_fn,void *);
pxa_status_t pxa_esp_storage_directory_check(uint32_t prompt_id,uint64_t epoch,const char *relative);
#ifdef __cplusplus
}
#endif
#endif
