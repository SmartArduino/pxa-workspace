#include "../src/services/pxa_esp_storage_path_policy.h"
#include <assert.h>
int main(void) {
    const char *denied[]={"", "PXA", "pxa", "PXA/", "PXA/system", "pxa/SYSTEM",
        "PXA/SYSTEM~1/a", "pxa/system~12/fonts/font.ttf",
        "PXA./system/a", "PXA/system./a", "PXA/system /a",
        "PXA/media/foo.", "PXA/media/foo ", "PXA//media/a"};
    const char *allowed[]={"GAME~1.NES", "PXA/media/GAME~1.NES",
        "PXA/media", "PXA/media/key/pak0.pak", "PXA/systematic/a", "documents/a.txt",
        "PXA/system-backup/a", "PXA/media/游戏/金庸群侠传.书剑江山.nes",
        "游戏/金庸群侠传.书剑江山.nes"};
    const char *system[]={"PXA/system/fonts/font.ttf", "PxA/SyStEm/packages/test/manifest.pxm",
        "pxa/SYSTEM/中文.bin"};
    for(unsigned unlocked=0;unlocked<2;++unlocked) {
        assert(!pxa_esp_storage_debug_primary_writable(NULL,unlocked));
        assert(pxa_esp_storage_debug_primary_writable("PXA/media",unlocked));
        assert(pxa_esp_storage_debug_primary_writable("PXA/media/author/game/a.gbc",unlocked));
        const char *private_paths[]={"PXA", "PXA/apps/author/game/data", "pxa-state/data/app",
            "PXA/volume.0.pxv", "PXA/medialog/a", "PXA/system", "PXA/system/"};
        for(unsigned i=0;i<sizeof(private_paths)/sizeof(private_paths[0]);++i)
            assert(!pxa_esp_storage_debug_primary_writable(private_paths[i],unlocked));
        assert(pxa_esp_storage_debug_primary_writable("PXA/system/fonts/a.ttf",unlocked)==(bool)unlocked);
        assert(!pxa_esp_storage_debug_child_writable(NULL,unlocked));
        for(unsigned i=0;i<sizeof(denied)/sizeof(denied[0]);++i)
            assert(!pxa_esp_storage_debug_child_writable(denied[i],unlocked));
        for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)
            assert(pxa_esp_storage_debug_child_writable(allowed[i],unlocked));
        for(unsigned i=0;i<sizeof(system)/sizeof(system[0]);++i)
            assert(pxa_esp_storage_debug_child_writable(system[i],unlocked)==(bool)unlocked);
    }
}
