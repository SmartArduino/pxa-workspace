/* NimBLE HCI transport. Shipped as source and built by your ESP-IDF. Do not modify. */

#include <string.h>
#include "esp_log.h"
#include "nimble/transport.h"
#include "os/os_mbuf.h"
#include "p4_wireless.h"

#define H4_CMD  0x01
#define H4_ACL  0x02
#define H4_EVT  0x04

static const char *TAG = "p4w_hci";

static void hci_rx(uint8_t type, uint8_t *data, uint16_t len)
{
    if (type == H4_EVT) {
        if (len > MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE)) {
            ESP_LOGW(TAG, "evt 0x%02x too long: %u", data[0], len);
            return;
        }
        bool discardable = data[0] == 0x3e && data[2] == 0x02;
        uint8_t *buf = ble_transport_alloc_evt(discardable);
        if (buf == NULL) {
            ESP_LOGW(TAG, "evt 0x%02x dropped, no buffer", data[0]);
            return;
        }
        memcpy(buf, data, len);
        ble_transport_to_hs_evt(buf);
    } else if (type == H4_ACL) {
        struct os_mbuf *om = ble_transport_alloc_acl_from_ll();
        if (om == NULL) {
            ESP_LOGW(TAG, "acl dropped, no buffer");
            return;
        }
        if (os_mbuf_append(om, data, len) != 0) {
            os_mbuf_free_chain(om);
            ESP_LOGW(TAG, "acl dropped, len %u", len);
            return;
        }
        ble_transport_to_hs_acl(om);
    }
}

void ble_transport_ll_init(void)
{
    esp_err_t err = p4_wireless_hci_open(hci_rx);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HCI open failed: %s (call nimble_port_init after esp_wifi_init)",
                 esp_err_to_name(err));
    }
}

int ble_transport_to_ll_cmd_impl(void *buf)
{
    uint8_t h4[1 + 3 + 255];
    uint8_t *cmd = buf;
    uint16_t len = 3 + cmd[2];

    h4[0] = H4_CMD;
    memcpy(&h4[1], cmd, len);
#if MYNEWT_VAL(MP_RUNTIME_ALLOC)
    ble_transport_free(BLE_HCI_CMD, buf);
#else
    ble_transport_free(buf);
#endif
    return p4_wireless_hci_send(h4, 1 + len) == ESP_OK ? 0 : -1;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
    uint8_t h4[1 + 4 + MYNEWT_VAL(BLE_TRANSPORT_ACL_SIZE)];
    uint16_t len = OS_MBUF_PKTLEN(om);

    if (len > sizeof(h4) - 1) {
        ESP_LOGW(TAG, "acl tx too long: %u", len);
        os_mbuf_free_chain(om);
        return -1;
    }
    h4[0] = H4_ACL;
    os_mbuf_copydata(om, 0, len, &h4[1]);
    os_mbuf_free_chain(om);
    return p4_wireless_hci_send(h4, 1 + len) == ESP_OK ? 0 : -1;
}
