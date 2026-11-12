// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int esp_err_t;
typedef unsigned TickType_t;
enum {ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND};
enum {I2C_STATUS_IDLE, I2C_STATUS_DONE, I2C_STATUS_ACK_ERROR, I2C_STATUS_TIMEOUT};
enum {I2C_LL_MASTER_EVENT_INTR = 255, pdTRUE = 1, portMAX_DELAY = 1000000};
#define pdMS_TO_TICKS(n) (n)
#define DIM(a) (sizeof(a) / sizeof((a)[0]))
#define I2C_TRANS_START_COMMAND {1}
#define I2C_TRANS_STOP_COMMAND {2}
#define ESP_RETURN_ON_FALSE(c, e, ...) do {if (!(c)) return e;} while (0)
#define ESP_LOGE(...) ((void)0)
#define PERIPH_RCC_ATOMIC() for (int once = 1; once; once = 0)
typedef struct {void *dev;} i2c_hal_context_t;
typedef struct {struct {unsigned op;} hw_cmd; uint8_t *data;} i2c_operation_t;
typedef struct {unsigned device_address; i2c_operation_t *ops; unsigned cmd_count;} i2c_transaction_t;
typedef struct {i2c_hal_context_t hal; unsigned clk_src, clk_src_freq_hz;} Base;
typedef struct {
    Base *base;
    int bus_lock_mux, status;
    unsigned cmd_idx, trans_idx, rx_cnt, read_len_static, read_buf_pos;
    bool trans_done, contains_read, bypass_nack_log;
    i2c_operation_t i2c_ops[16];
    i2c_transaction_t i2c_trans;
} i2c_master_bus_t;
typedef i2c_master_bus_t *i2c_master_bus_handle_t;
static bool lock_available = true, cleared;
static int outcome;
static i2c_master_bus_t *current_bus;
static int xSemaphoreTake(int lock, TickType_t timeout) {(void)lock; (void)timeout; return lock_available;}
static void xSemaphoreGive(int lock) {(void)lock;}
static void i2c_ll_set_source_clk(void *d, unsigned c) {(void)d; (void)c;}
static void i2c_hal_set_bus_timing(i2c_hal_context_t *h, unsigned a, unsigned b, unsigned c) {(void)h; (void)a; (void)b; (void)c;}
static void i2c_ll_txfifo_rst(void *d) {(void)d;}
static void i2c_ll_rxfifo_rst(void *d) {(void)d;}
static void i2c_ll_master_set_fractional_divider(void *d, unsigned a, unsigned b) {(void)d; (void)a; (void)b;}
static void i2c_ll_clear_intr_mask(void *d, unsigned m) {(void)d; assert(m == 255); cleared = true;}
static void i2c_ll_enable_intr_mask(void *d, unsigned m) {
    (void)d; (void)m;
    /* A completion arriving as soon as IRQs are enabled must not drain RX
     * through the previous transfer's now-dead receive buffer. */
    assert(!current_bus->contains_read);
    assert(!current_bus->read_len_static && !current_bus->rx_cnt && !current_bus->read_buf_pos);
    assert(cleared);
}
static void i2c_ll_disable_intr_mask(void *d, unsigned m) {(void)d; (void)m;}
static void i2c_hal_master_set_scl_timeout_val(i2c_hal_context_t *h, unsigned t, unsigned f) {(void)h; (void)t; (void)f;}
static void i2c_ll_update(void *d) {(void)d;}
static void s_i2c_send_commands(i2c_master_bus_handle_t bus, TickType_t t) {
    (void)t;
    assert(bus->i2c_trans.ops == bus->i2c_ops);
    assert(bus->i2c_trans.cmd_count == 2);
    assert(!bus->i2c_trans.ops[0].data && !bus->i2c_trans.ops[1].data);
    bus->status = outcome;
}
#include PXA_I2C_PROBE_FIXTURE
int main(void) {
    Base base = {0};
    i2c_master_bus_t bus = {.base = &base}; current_bus = &bus;
    for (unsigned i = 0; i < 3000; ++i) {
        bus.contains_read = true; bus.read_len_static = 31;
        bus.rx_cnt = 31; bus.read_buf_pos = 5; cleared = false;
        outcome = i % 3 == 0 ? I2C_STATUS_DONE : i % 3 == 1 ? I2C_STATUS_ACK_ERROR : I2C_STATUS_TIMEOUT;
        int expected = i % 3 == 0 ? ESP_OK : i % 3 == 1 ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
        assert(i2c_master_probe(&bus, 0x55, 2) == expected);
        /* The interrupt-visible table still belongs to the bus after return. */
        assert(bus.i2c_trans.ops == bus.i2c_ops && bus.i2c_ops[1].hw_cmd.op == 2);
        bus.contains_read = true; transfer_start(&bus); assert(!bus.contains_read);
    }
    lock_available = false; bus.contains_read = true;
    assert(i2c_master_probe(&bus, 0x55, 2) == ESP_ERR_TIMEOUT);
    assert(bus.contains_read); /* No mutation without owning the bus. */
    assert(i2c_master_probe(NULL, 0x55, 2) == ESP_ERR_INVALID_ARG);
    return 0;
}
