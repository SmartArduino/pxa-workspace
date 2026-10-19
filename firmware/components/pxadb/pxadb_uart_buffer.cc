/* Keep the console's installed driver and VFS bindings. IDF initializes its
 * console RX ring with 256 bytes; PXADB shares that port and needs a complete
 * upload command to survive a filesystem scheduling delay. */
#include "sdkconfig.h"
#include <atomic>
#include <driver/uart.h>

namespace {
std::atomic<unsigned> installed_rx_bytes{0};
constexpr int minimum_rx_bytes = 4096;
}

extern "C" esp_err_t __real_uart_driver_install(
    uart_port_t port, int rx_bytes, int tx_bytes, int queue_size,
    QueueHandle_t *queue, int interrupt_flags);

extern "C" esp_err_t __wrap_uart_driver_install(
    uart_port_t port, int rx_bytes, int tx_bytes, int queue_size,
    QueueHandle_t *queue, int interrupt_flags) {
    const bool shared_port = static_cast<int>(port) == CONFIG_PXADB_UART_PORT;
    if (shared_port && rx_bytes > 0 && rx_bytes < minimum_rx_bytes)
        rx_bytes = minimum_rx_bytes;
    const esp_err_t result = __real_uart_driver_install(
        port, rx_bytes, tx_bytes, queue_size, queue, interrupt_flags);
    if (shared_port && result == ESP_OK)
        installed_rx_bytes.store(static_cast<unsigned>(rx_bytes));
    return result;
}

extern "C" unsigned pxadb_uart_rx_buffer_capacity(void) {
    return installed_rx_bytes.load();
}
