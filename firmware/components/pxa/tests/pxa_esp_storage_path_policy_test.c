#include "../src/services/pxa_esp_storage_path_policy.h"
#include <assert.h>
int main(void) {
    const char *denied[]={"", "PXA", "PXA/", "pxa", "PXA/system", "pxa/SYSTEM/fonts/font.ttf",
        "PXA./system/a", "PXA/system./a", "PXA/system /a", "PXA/SYSTEM~1/a",
        "PXA/media/foo.", "PXA/media/foo ", "PXA//media/a"};
    const char *allowed[]={"PXA/media", "PXA/media/key/pak0.pak", "PXA/systematic/a", "documents/a.txt"};
    assert(!pxa_esp_storage_debug_child_writable(NULL));
    for(unsigned i=0;i<sizeof(denied)/sizeof(denied[0]);++i)
        assert(!pxa_esp_storage_debug_child_writable(denied[i]));
    for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)
        assert(pxa_esp_storage_debug_child_writable(allowed[i]));
}
