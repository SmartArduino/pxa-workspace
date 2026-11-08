// SPDX-License-Identifier: Apache-2.0
#include <pxa_ext/wifi_location.h>
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstring>
namespace {
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
bool busy = false;
int64_t attempted = 0;
pxa_status_t last_status = PXA_STATUS_UNAVAILABLE;
pxa_wl_snapshot_t latest{};
void scan(void*) {
    std::vector<WifiNetwork> networks;
    pxa_wl_snapshot_t result{};
    auto status = PXA_STATUS_UNAVAILABLE;
    if (WifiManager::GetInstance().ScanNetworks(&networks,true)) {
        std::sort(networks.begin(),networks.end(),[](const auto& a,const auto& b){return a.rssi>b.rssi;});
        const auto now = esp_timer_get_time();
        for (const auto& ap:networks) {
            if (result.count==PXA_WL_MAX_APS) break;
            if (!ap.observed_at_us || now-ap.observed_at_us>30000000 || (ap.bssid[0]&3) ||
                (ap.ssid.size()>=6 && ap.ssid.compare(ap.ssid.size()-6,6,"_nomap")==0)) continue;
            auto& out=result.aps[result.count++];
            std::memcpy(out.bssid,ap.bssid,6); out.rssi=ap.rssi;
            std::memcpy(out.ssid,ap.ssid.data(),std::min(ap.ssid.size(),sizeof(out.ssid)-1));
            if (!result.timestamp_us || uint64_t(ap.observed_at_us)<result.timestamp_us) result.timestamp_us=ap.observed_at_us;
        }
        if (result.count>=2) status=PXA_STATUS_OK;
    }
    portENTER_CRITICAL(&lock); latest=result; last_status=status; attempted=esp_timer_get_time(); busy=false; portEXIT_CRITICAL(&lock);
    vTaskDelete(nullptr);
}
}
extern "C" pxa_status_t pxa_esp_wifi_location_read(void*,pxa_wl_snapshot_t* out) {
    const auto now=esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    if (attempted && now-attempted<10000000) {
        *out=latest; auto status=last_status; portEXIT_CRITICAL(&lock); return status;
    }
    if (busy) {portEXIT_CRITICAL(&lock);return PXA_STATUS_WOULD_BLOCK;}
    busy=true; portEXIT_CRITICAL(&lock);
    if (xTaskCreate(scan,"pxa-wifi-loc",4096,nullptr,2,nullptr)!=pdPASS) {
        portENTER_CRITICAL(&lock);busy=false;portEXIT_CRITICAL(&lock);return PXA_STATUS_RESOURCE_LIMIT;
    }
    return PXA_STATUS_WOULD_BLOCK;
}
