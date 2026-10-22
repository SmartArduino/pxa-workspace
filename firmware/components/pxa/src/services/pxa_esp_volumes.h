#ifndef PXA_ESP_VOLUMES_H
#define PXA_ESP_VOLUMES_H
#include "pxa/pxa_esp_storage.h"
#include "pxa/posix/pxa_scoped_fs.h"
/* Internal inventory views. Hold the recursive inventory lock while using
 * these pointers; never retain a path across unlock. Slot addresses are
 * stable for the Host lifetime. Installation I/O does not hold this lock. */
void pxa_esp_volumes_lock(void);
void pxa_esp_volumes_unlock(void);
pxa_status_t pxa_esp_volumes_initialize(void);
pxa_volume_registry_t *pxa_esp_volumes_registry(void);
const pxa_scoped_fs_mount_t *pxa_esp_volumes_mounts(void);
/* Operation owner holds this reference until all native I/O handles close.
 * Drain revokes access immediately; physical unregister waits for release. */
pxa_status_t pxa_esp_volumes_retain(uint64_t epoch);
pxa_status_t pxa_esp_volumes_release(uint64_t epoch);
/* Private image VFS mounts inherit the physical volume's lifetime, but are
 * never listed as storage choices or exposed by directory grants. Unbind
 * only after closing their users and unmounting the private filesystem. */
pxa_status_t pxa_esp_volumes_bind_private(const char *virtual_path,uint64_t epoch);
/* Pins a verified app home on its existing physical filesystem, including
 * code/assets lifetime before a private image has been opened. */
pxa_status_t pxa_esp_volumes_bind_app_home(const char *home,uint64_t epoch);
pxa_status_t pxa_esp_volumes_unbind_private(const char *virtual_path,uint64_t epoch);
#endif
