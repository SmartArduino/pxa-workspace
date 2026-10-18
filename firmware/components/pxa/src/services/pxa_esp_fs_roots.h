#ifndef PXA_ESP_FS_ROOTS_H
#define PXA_ESP_FS_ROOTS_H
#include "pxa/posix/pxa_scoped_fs.h"
typedef struct pxa_esp_fs_roots pxa_esp_fs_roots_t;
pxa_status_t pxa_esp_fs_roots_create(void *allocator_context,
    void *(*allocate)(void *,size_t),const char *identity,const char *name,
    uint8_t user_directories,
    void *notify_context,void (*notify)(void *),pxa_esp_fs_roots_t **output,
    pxa_fs_roots_backend_t **backend);
void pxa_esp_fs_roots_bind(pxa_esp_fs_roots_t *,pxa_fs_service_t *);
uint64_t pxa_esp_fs_roots_features(const pxa_esp_fs_roots_t *);
void pxa_esp_fs_roots_poll(pxa_esp_fs_roots_t *);
void pxa_esp_fs_roots_destroy(pxa_esp_fs_roots_t *);
#endif
