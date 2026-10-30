# Seeed SSCMA SPI transport

Source: [SenseCAP Watcher firmware](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/tree/8e37f7ccd3d836bb99f483e0f6e74e8252406644/components/sscma_client), Apache-2.0; the original license is retained in LICENSE.

Only the SPI IO implementation and its IO interface are included. Local changes:
remove unused cJSON and IO-expander dependencies; keep a SPI-only declaration;
leave bus initialization/freeing with the board because SD and Himax share SPI2;
use the ESP-IDF-required portMAX_DELAY bus acquisition on the dedicated vision
worker (never on the Host event loop); fix the size_t log format and the GPIO
shift to 64 bits. Every acquired region releases the bus on success or error. Polling uses sync_gpio_num=-1. No flasher, UART, model update or reset
implementation is included. The enclosing driver exposes bounded JPEG and
inference reads, never arbitrary commands.
