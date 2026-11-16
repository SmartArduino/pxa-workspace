#include "../src/services/pxa_esp_resource_policy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    const size_t mib=1024u*1024u;
    size_t limit,temporary;
    pxa_esp_resource_psram_limits(8*mib,1,2*mib,mib/2,&limit,&temporary);
    assert(limit==4*mib && temporary==2*mib);
    pxa_esp_resource_psram_limits(32*mib,1,2*mib,mib/2,&limit,&temporary);
    assert(limit==16*mib && temporary==8*mib);
    pxa_esp_resource_psram_limits(32*mib,0,2*mib,mib/2,&limit,&temporary);
    assert(limit==2*mib && temporary==mib/2);
    pxa_esp_resource_psram_limits(0,1,2*mib,mib/2,&limit,&temporary);
    assert(!limit && !temporary);
    pxa_esp_resource_psram_limits(SIZE_MAX,1,0,0,&limit,&temporary);
    assert(limit==SIZE_MAX/2 && temporary==SIZE_MAX/4);
    assert(!pxa_esp_resource_psram_available(8*mib,mib));
    assert(pxa_esp_resource_psram_available(8*mib,5*mib)==4*mib);
    assert(pxa_esp_resource_psram_available(32*mib,20*mib)==16*mib);
    assert(!pxa_esp_resource_psram_available(0,0));
    puts("PSRAM resource policy PASS: 8/32MiB, manual ceilings, zero, overflow, headroom");
}
