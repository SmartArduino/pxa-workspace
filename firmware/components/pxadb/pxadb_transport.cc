#include "sdkconfig.h"
#if CONFIG_PXADB_ENABLED
#include "pxadb_transport.h"
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
#include <driver/gpio.h>
#include <driver/uart.h>
extern "C" unsigned pxadb_uart_rx_buffer_capacity(void);
#endif
#if CONFIG_PXADB_TRANSPORT_USB_CDC
#include <tinyusb_cdc_acm.h>
#elif CONFIG_PXADB_TRANSPORT_USB_SERIAL_JTAG
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#endif

namespace pxadb {
namespace {
constexpr TickType_t kIoTimeout = pdMS_TO_TICKS(1000);
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
constexpr auto kUart = static_cast<uart_port_t>(CONFIG_PXADB_UART_PORT);
// Keep interrupt latency headroom even at a board's optional high UART rate.
constexpr int kRxThreshold = UART_HW_FIFO_LEN(kUart) / 4;
esp_err_t StartUart() {
    esp_err_t status;
    if (uart_is_driver_installed(kUart)) {
        status = uart_set_baudrate(kUart, CONFIG_PXADB_UART_BAUD);
    } else {
        const uart_config_t config = {
            .baud_rate = CONFIG_PXADB_UART_BAUD,
            .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0, .source_clk = UART_SCLK_DEFAULT, .flags = {},
        };
        status = uart_param_config(kUart, &config);
        if (status == ESP_OK)
            status = uart_driver_install(kUart, 4096, 4096, 0, nullptr, 0);
    }
    if (status != ESP_OK) return status;
    if (CONFIG_PXADB_UART_TX_GPIO >= 0 || CONFIG_PXADB_UART_RX_GPIO >= 0) {
        status = uart_set_pin(kUart,
            CONFIG_PXADB_UART_TX_GPIO >= 0 ? CONFIG_PXADB_UART_TX_GPIO : UART_PIN_NO_CHANGE,
            CONFIG_PXADB_UART_RX_GPIO >= 0 ? CONFIG_PXADB_UART_RX_GPIO : UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (status != ESP_OK) return status;
    }
    return uart_set_rx_full_threshold(kUart, kRxThreshold);
}
#endif
}  // namespace

bool TransportIsUart(Transport transport) {
#if CONFIG_PXADB_TRANSPORT_UART
    (void)transport;
    return true;
#else
    return transport == Transport::kUart;
#endif
}

esp_err_t TransportStart(Transport transport) {
    if (TransportIsUart(transport)) {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
        return StartUart();
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
#if CONFIG_PXADB_TRANSPORT_USB_CDC
    return tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) ? ESP_OK : ESP_ERR_INVALID_STATE;
#elif CONFIG_PXADB_TRANSPORT_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 512, .rx_buffer_size = 4096};
        const auto status = usb_serial_jtag_driver_install(&config);
        if (status != ESP_OK) return status;
    }
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int TransportRead(Transport transport, uint8_t* bytes, size_t size, uint32_t timeout_ms) {
    if (!size) return 0;
    if (TransportIsUart(transport)) {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
        // Wait for only the first byte; a short command must not wait for a full block.
        const int first = uart_read_bytes(kUart, bytes, 1, pdMS_TO_TICKS(timeout_ms));
        if (first <= 0 || size == 1) return first;
        return 1 + std::max(uart_read_bytes(kUart, bytes + 1, size - 1, 0), 0);
#else
        return -1;
#endif
    }
#if CONFIG_PXADB_TRANSPORT_USB_CDC
    size_t received = 0;
    (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, bytes, size, &received) != ESP_OK) return -1;
    if (!received && timeout_ms) vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    return static_cast<int>(received);
#elif CONFIG_PXADB_TRANSPORT_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(bytes, size, pdMS_TO_TICKS(timeout_ms));
#else
    return -1;
#endif
}

int TransportWrite(Transport transport, const uint8_t* bytes, size_t size) {
    if (TransportIsUart(transport)) {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
        return uart_write_bytes(kUart, bytes, size);
#else
        return -1;
#endif
    }
#if CONFIG_PXADB_TRANSPORT_USB_CDC
    const size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, bytes, size);
    (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return static_cast<int>(written);
#elif CONFIG_PXADB_TRANSPORT_USB_SERIAL_JTAG
    return usb_serial_jtag_write_bytes(bytes, size, kIoTimeout);
#else
    return -1;
#endif
}

bool TransportTxDone(Transport transport) {
    if (TransportIsUart(transport)) {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
        return uart_wait_tx_done(kUart, kIoTimeout) == ESP_OK;
#else
        return false;
#endif
    }
#if CONFIG_PXADB_TRANSPORT_USB_CDC
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, kIoTimeout) == ESP_OK;
#elif CONFIG_PXADB_TRANSPORT_USB_SERIAL_JTAG
    return usb_serial_jtag_wait_tx_done(kIoTimeout) == ESP_OK;
#else
    return false;
#endif
}

size_t TransportWriteChunk(Transport transport) { return TransportIsUart(transport) ? 1024 : 256; }
const char* TransportName(Transport transport) {
    if (TransportIsUart(transport)) return "uart";
#if CONFIG_PXADB_TRANSPORT_USB_CDC
    return "usb-cdc";
#else
    return "usb-jtag";
#endif
}
unsigned TransportUartRxCapacity() {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
    return pxadb_uart_rx_buffer_capacity();
#else
    return 0;
#endif
}
int TransportUartRxFifoThreshold() {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
    return kRxThreshold;
#else
    return 0;
#endif
}
unsigned TransportBaudRate(Transport transport) {
#if CONFIG_PXADB_TRANSPORT_UART || CONFIG_PXADB_UART_SECONDARY
    return TransportIsUart(transport) ? CONFIG_PXADB_UART_BAUD : 0;
#else
    (void)transport;
    return 0;
#endif
}
}  // namespace pxadb
#endif
