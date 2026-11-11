// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_usb.h"
#include "espp4_a_usb_download.h"
#include <cstdio>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_private/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/usb_utmi_ll.h>
#include <pxa/pxa_host.h>
#include <pxa_system_clock.h>
#include <pxadb/pxadb_service.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_default_config.h>

namespace {
constexpr char kTag[] = "espp4_usb";
TaskHandle_t download_task;
espp4_a::UsbDownloadTrigger trigger;

void Download(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Finish the host's control transfer outside the TinyUSB callback.
        vTaskDelay(pdMS_TO_TICKS(150));
        if (!pxa_host_prepare_shutdown(1500)) {
            ESP_LOGE(kTag, "USB download cancelled: application shutdown not acknowledged");
            continue;
        }
        (void)pxa_system_clock_save();
        pxadb::Stop();
        if (tinyusb_driver_uninstall() != ESP_OK) {
            ESP_LOGE(kTag, "USB download cancelled: controller still busy");
            (void)pxadb::Start();
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        PERIPH_RCC_ATOMIC() {
            usb_utmi_ll_enable_bus_clock(true);
            usb_utmi_ll_reset_register();
        }
        Espp4ARestartToUsbDownload();
    }
}
}

bool Espp4AInitializeUsb() {
    static char serial[18];
    static const char language[] = {0x09, 0x04};
    static const char* strings[] = {language, "SmartArduino", "ESPP4-A", serial, "PXA Debug"};
    uint8_t mac[6];
    // P4 has no on-chip Wi-Fi MAC; the external radio has a separate identity.
    // ROM USB uses the P4 eFuse base MAC, which must match across re-enumeration.
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
    std::snprintf(serial, sizeof(serial), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (xTaskCreatePinnedToCore(Download, "p4_usb_download", 4096, nullptr, 3,
                              &download_task, 0) != pdPASS) return false;
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.task.xCoreID = 0;
    config.descriptor.string = strings;
    config.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    if (tinyusb_driver_install(&config) != ESP_OK) {
        vTaskDelete(download_task); download_task = nullptr; return false;
    }
    const tinyusb_config_cdcacm_t cdc = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_line_state_changed = [](int, cdcacm_event_t* event) {
            if (trigger.SetDtr(event->line_state_changed_data.dtr)) xTaskNotifyGive(download_task);
        },
        .callback_line_coding_changed = [](int, cdcacm_event_t* event) {
            trigger.SetBitRate(event->line_coding_changed_data.p_line_coding->bit_rate);
        },
    };
    if (tinyusb_cdcacm_init(&cdc) != ESP_OK) {
        (void)tinyusb_driver_uninstall();
        vTaskDelete(download_task); download_task = nullptr; return false;
    }
    ESP_LOGI(kTag, "USB HS CDC ready: ESPP4-A %s; 1200-baud DTR falling edge enters ROM", serial);
    return true;
}
