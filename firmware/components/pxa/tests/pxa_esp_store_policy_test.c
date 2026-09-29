#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pxa_esp_store_policy.h"

int main(void) {
    pxa_esp_package_record_t record = {0};
    const char identity[] = "signed-publisher-root:pxa-store";
    strcpy(record.id, identity);
    strcpy(record.app_id, "pxa-store");
    record.built_in = true;
    record.installed = true;
    record.enabled = true;
    assert(pxa_esp_store_installer_authorized(&record, identity));
    assert(!pxa_esp_store_installer_authorized(&record,
                                              "different-root:pxa-store"));
    record.built_in = false;
    assert(!pxa_esp_store_installer_authorized(&record, identity));
    record.built_in = true;
    record.enabled = false;
    assert(!pxa_esp_store_installer_authorized(&record, identity));
    record.enabled = true;
    strcpy(record.app_id, "pxa-store-copy");
    assert(!pxa_esp_store_installer_authorized(&record, identity));
    puts("pxa_esp_store_policy_test OK");
    return 0;
}
