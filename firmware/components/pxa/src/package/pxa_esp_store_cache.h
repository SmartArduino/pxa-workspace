#ifndef PXA_ESP_STORE_CACHE_H
#define PXA_ESP_STORE_CACHE_H
#include "pxa_esp_package_store.h"
#define PXA_ESP_STORE_DOWNLOAD_SLOTS 8u
typedef struct {
    char filename[20],app_id[65],owner[PXA_ESP_PACKAGE_ID_BYTES];
    uint64_t epoch,size;
    uint8_t digest[32],publisher[32];
} pxa_esp_store_download_entry_t;
int pxa_esp_store_cache_name_valid(const char *);
pxa_status_t pxa_esp_store_cache_path(uint64_t,const char *,char *,size_t);
/* Host control task owns this bounded in-memory view. Each authenticated
 * descriptor and its transport copy live on the same physical volume. */
void pxa_esp_store_cache_load(pxa_esp_store_download_entry_t *,size_t);
/* Before opening the developer transport or any transfer worker. Ordinary
 * discovery never deletes a file that another owner may be publishing. */
void pxa_esp_store_cache_cleanup(void);
pxa_status_t pxa_esp_store_cache_save(const pxa_esp_store_download_entry_t *);
pxa_status_t pxa_esp_store_cache_remove(const pxa_esp_store_download_entry_t *);
/* Trusted Host import of an already-transferred canonical cache file. Full
 * publisher verification precedes descriptor creation. Never accepts a Guest
 * path; the control-plane caller supplies the verified owner identity. */
pxa_status_t pxa_esp_store_cache_adopt(const char *,const char *owner,char *token,size_t);
#endif
