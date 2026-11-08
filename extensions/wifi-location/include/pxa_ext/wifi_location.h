// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/permission.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PXA_WL_SERVICE_ID 33027
#define PXA_WL_PERMISSION "org.doit.wifi-location"
#define PXA_WL_SCOPE "wifi.scan"
#define PXA_WL_IDENTITY {0x2b,0xdd,0x0e,0x91,0xe1,0xc4,0x48,0xda,0x99,0xe2,0x6a,0x70,0xf3,0x11,0x39,0x85}
#define PXA_WL_MAX_APS 10
typedef struct { uint8_t bssid[6]; int8_t rssi; char ssid[33]; } pxa_wl_ap_t;
typedef struct { uint64_t timestamp_us; uint16_t count; pxa_wl_ap_t aps[PXA_WL_MAX_APS]; } pxa_wl_snapshot_t;
typedef pxa_status_t (*pxa_wl_read_fn)(void *, pxa_wl_snapshot_t *);
typedef struct pxa_wl_service pxa_wl_service_t;
size_t pxa_wl_workspace_size(void);
pxa_status_t pxa_wl_init(void *, size_t, pxa_runtime_t *, pxa_permission_service_t *,
    pxa_wl_read_fn, void *, pxa_wl_service_t **);
pxa_status_t pxa_wl_register(pxa_wl_service_t *);
#ifdef __cplusplus
}
#endif
