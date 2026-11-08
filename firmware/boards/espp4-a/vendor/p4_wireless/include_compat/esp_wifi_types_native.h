/* Used on IDF 5.x / 6.0 only. Promiscuous mode / CSI are not implemented, these types only keep the build compatible. */

#pragma once

#include "sdkconfig.h"
#include "esp_wifi_types_generic.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_ESP_WIFI_REMOTE_MAX_CONN_NUM
#define ESP_WIFI_MAX_CONN_NUM  (CONFIG_ESP_WIFI_REMOTE_MAX_CONN_NUM)
#else
#define ESP_WIFI_MAX_CONN_NUM  (15)
#endif

typedef struct wifi_sta_list_t {
    wifi_sta_info_t sta[ESP_WIFI_MAX_CONN_NUM];
    int             num;
} wifi_sta_list_t;

typedef struct {
    signed   rssi: 8;
    unsigned rate: 5;
    unsigned : 1;
    unsigned sig_mode: 2;
    unsigned : 16;
    unsigned mcs: 7;
    unsigned cwb: 1;
    unsigned : 16;
    unsigned smoothing: 1;
    unsigned not_sounding: 1;
    unsigned : 1;
    unsigned aggregation: 1;
    unsigned stbc: 2;
    unsigned fec_coding: 1;
    unsigned sgi: 1;
    unsigned ampdu_cnt: 8;
    unsigned channel: 4;
    unsigned secondary_channel: 4;
    unsigned : 8;
    unsigned timestamp: 32;
    unsigned : 32;
    unsigned : 31;
    unsigned ant: 1;
    unsigned sig_len: 12;
    unsigned : 12;
    unsigned rx_state: 8;
} wifi_pkt_rx_ctrl_t;

typedef struct wifi_csi_config_t {
    bool     lltf_en;
    bool     htltf_en;
    bool     stbc_htltf2_en;
    bool     ltf_merge_en;
    bool     channel_filter_en;
    bool     manu_scale;
    uint8_t  shift;
    bool     dump_ack_en;
} wifi_csi_config_t;

#ifdef __cplusplus
}
#endif
