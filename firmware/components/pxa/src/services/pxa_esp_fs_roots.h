#ifndef PXA_ESP_FS_ROOTS_H
#define PXA_ESP_FS_ROOTS_H
#include "pxa/posix/pxa_scoped_fs.h"
typedef struct pxa_esp_fs_roots pxa_esp_fs_roots_t;
pxa_status_t pxa_esp_fs_roots_create(void *allocator_context,
    void *(*allocate)(void *,size_t),const char *identity,const char *name,
    uint8_t user_directories,
    void *notify_context,void (*notify)(void *),pxa_esp_fs_roots_t **output,
    pxa_fs_roots_backend_t **backend);
/* Host-only ready app-media mount, for recoverable cold-path migration. */
pxa_status_t pxa_esp_fs_roots_media_mount(char *,size_t);
/* Host-only override before binding the service, never accepts Guest paths. */
pxa_status_t pxa_esp_fs_roots_set_media_relative(pxa_esp_fs_roots_t *,const char *);
void pxa_esp_fs_roots_bind(pxa_esp_fs_roots_t *,pxa_fs_service_t *);
uint64_t pxa_esp_fs_roots_features(const pxa_esp_fs_roots_t *);
void pxa_esp_fs_roots_poll(pxa_esp_fs_roots_t *);
void pxa_esp_fs_roots_destroy(pxa_esp_fs_roots_t *);
#endif
