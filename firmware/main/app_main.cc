#include <cstdlib>
#include <ctime>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "pxa_board.h"
#include "pxa_integration.h"
#include "pxadb/pxadb_service.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "pxa_platform";
constexpr char kPxaFontPath[] =
    CONFIG_PXA_MOUNT_POINT "/system/fonts/noto_sans_cjk_common.ttf";
}

extern "C" void app_main(void) {
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    if (!pxa_board_register_selected()) {
        ESP_LOGE(kTag, "Unable to register selected board port");
        return;
    }

    pxa_product_profile_t profile;
    pxa_product_profile_init(&profile);
    profile.locale = "zh-CN";
    profile.font_path = kPxaFontPath;
    profile.display_scheme = PXSYS_COLOR_SCHEME_DARK;
    profile.mount_storage = true;

    if (!pxa_integration_start(&profile)) {
        ESP_LOGE(kTag, "PXA integration did not start");
        return;
    }

#if CONFIG_PXADB_AUTOSTART
    if (pxadb::StartAutostart() != ESP_OK)
        ESP_LOGW(kTag, "PXADB autostart task could not be created");
#endif
}
