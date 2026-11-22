/* SPDX-License-Identifier: MIT
 * Single-owner Aster Xtensa AOT mappings using public ESP-IDF MMU APIs.
 * Keep data allocation and executable aliases paired across load failures.
 */
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>
typedef struct {uint8_t *data,*code;size_t bytes;} mapping;
static mapping maps[8];
void *os_mmap(void *hint,size_t size,int prot,int flags,os_file_handle file)
{
    (void)hint;(void)flags;(void)file;
    if(!size || size>SIZE_MAX-CONFIG_MMU_PAGE_SIZE)return NULL;
    if(!(prot&MMAP_PROT_EXEC))return heap_caps_aligned_calloc(64,1,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    unsigned i;for(i=0;i<8;i++)if(!maps[i].data)break;if(i==8)return NULL;
    size_t bytes=(size+CONFIG_MMU_PAGE_SIZE-1)&~(size_t)(CONFIG_MMU_PAGE_SIZE-1);
    void *data=heap_caps_aligned_calloc(CONFIG_MMU_PAGE_SIZE,1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),*code=NULL;
    if(!data)return NULL;
    esp_paddr_t address;mmu_target_t target;
    if(esp_mmu_vaddr_to_paddr(data,&address,&target)!=ESP_OK || target!=MMU_TARGET_PSRAM0 ||
       esp_mmu_map(address,bytes,target,MMU_MEM_CAP_EXEC,ESP_MMU_MMAP_FLAG_PADDR_SHARED,&code)!=ESP_OK){heap_caps_free(data);return NULL;}
    maps[i]=(mapping){data,code,bytes};
    ESP_LOGI("aster_aot","PSRAM mapping: code=%p data=%p bytes=%u",code,data,(unsigned)bytes);
    return code;
}
void *os_mremap(void *old,size_t previous,size_t next){return os_mremap_slow(old,previous,next);}
void os_munmap(void *address,size_t size)
{
    (void)size;
    for(unsigned i=0;i<8;i++)if(maps[i].code==address){
        if(esp_mmu_unmap(address)==ESP_OK){heap_caps_free(maps[i].data);maps[i]=(mapping){0};}return;
    }
    heap_caps_free(address);
}
int os_mprotect(void *address,size_t size,int prot){(void)address;(void)size;(void)prot;return 0;}
void *os_get_dbus_mirror(void *code)
{
    for(unsigned i=0;i<8;i++)if(maps[i].code && (uintptr_t)code>=(uintptr_t)maps[i].code &&
        (uintptr_t)code-(uintptr_t)maps[i].code<maps[i].bytes)
        return maps[i].data+((uintptr_t)code-(uintptr_t)maps[i].code);
    return code;
}
void os_dcache_flush(void)
{
    for(unsigned i=0;i<8;i++)if(maps[i].data){
        ESP_ERROR_CHECK(esp_cache_msync(maps[i].data,maps[i].bytes,ESP_CACHE_MSYNC_FLAG_DIR_C2M));
        ESP_ERROR_CHECK(esp_cache_msync(maps[i].code,maps[i].bytes,ESP_CACHE_MSYNC_FLAG_DIR_M2C|ESP_CACHE_MSYNC_FLAG_TYPE_INST));
    }
}
void os_icache_flush(void *code,size_t size){(void)code;(void)size;os_dcache_flush();}
