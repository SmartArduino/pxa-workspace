#include <cstdlib>
#include <ctime>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "pxa_board.h"
#include "pxa_board_api.h"
#include "pxa_integration.h"
#include <pxa/pxa_esp_system_font.h>
#include <pxa/pxa_esp_security.h>
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "pxa_platform";
}

extern "C" void app_main(void) {
    setenv("TZ", "CST-8", 1);
    tzset();

    const esp_err_t result = nvs_flash_init();
    if(result!=ESP_OK) {
        // NVS also owns the fixed device authentication key. Erasing it here
        // would invalidate every volume's signed control records and grants.
        ESP_LOGE(kTag,"NVS initialization failed: %s; contents preserved",esp_err_to_name(result));
        return;
    }
    const auto security=pxa_esp_security_initialize_early();
    if(security!=PXA_STATUS_OK)
        ESP_LOGW(kTag,"Device record authentication unavailable: %ld",long(security));

    if (!pxa_board_register_selected()) {
        ESP_LOGE(kTag, "Unable to register selected board port");
        return;
    }

    pxa_product_profile_t profile;
    pxa_product_profile_init(&profile);
    profile.locale = "zh-CN";
    profile.font_path = pxa_esp_system_font_path();
    profile.display_scheme = PXSYS_COLOR_SCHEME_DARK;
    profile.mount_storage = true;

    if (!pxa_integration_start(&profile)) {
        ESP_LOGE(kTag, "PXA integration did not start");
        return;
    }
}
