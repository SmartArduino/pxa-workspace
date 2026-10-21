#ifndef CLOCK_TEST_ESP_SYSTEM_H
#define CLOCK_TEST_ESP_SYSTEM_H

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)

esp_err_t esp_register_shutdown_handler(void (*handler)(void));

#endif
