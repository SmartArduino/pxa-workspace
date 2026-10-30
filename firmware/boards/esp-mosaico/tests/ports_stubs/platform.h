#pragma once
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <thread>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NOT_FOUND=1;
constexpr int GPIO_NUM_0=0, GPIO_NUM_1=1, GPIO_NUM_3=3, GPIO_NUM_58=58, GPIO_NUM_59=59;
constexpr int GPIO_MODE_INPUT=1, GPIO_MODE_OUTPUT=2, GPIO_MODE_INPUT_OUTPUT=3;
constexpr int I2C_NUM_1=1, I2C_CLK_SRC_DEFAULT=0, I2C_ADDR_BIT_LEN_7=7;
using gpio_num_t=int;
struct gpio_config_t { uint64_t pin_bit_mask; int mode; };
inline int levels[64]{}, resets[64]{}, modes[64]{};
inline int fail_level_pin=-1, fail_reset_pin=-1, fail_config_pin=-1;
inline int gpio_set_level(int pin, int level) { if(pin==fail_level_pin)return ESP_FAIL; levels[pin]=level; return ESP_OK; }
inline int gpio_get_level(int pin) { return levels[pin]; }
inline int gpio_reset_pin(int pin) { ++resets[pin]; if(pin==fail_reset_pin)return ESP_FAIL; modes[pin]=0; return ESP_OK; }
inline int gpio_config(const gpio_config_t* c) { if(fail_config_pin>=0 && (c->pin_bit_mask & (1ULL<<fail_config_pin)))return ESP_FAIL; for(int i=0;i<64;++i) if(c->pin_bit_mask & (1ULL<<i)) modes[i]=c->mode; return ESP_OK; }
struct I2c { uint8_t data[256]{}; bool present=false, fail=false; };
inline I2c i2c[128];
inline bool probe_errors[128]{};
inline int add_failure_address=-1, add_calls[128]{}, block_add_address=-1;
inline std::atomic<bool> add_entered{false}, release_add{false};
using i2c_master_bus_handle_t=void*;
using i2c_master_dev_handle_t=I2c*;
struct i2c_master_bus_config_t { int i2c_port,sda_io_num,scl_io_num,clk_source,glitch_ignore_cnt; struct { bool enable_internal_pullup; } flags; };
struct i2c_device_config_t { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; };
inline bool remove_fails=false;
inline int probes=0, bus_creates=0, last_timeout=0;
inline int i2c_new_master_bus(const i2c_master_bus_config_t* c, void** out) {
    assert(c->i2c_port==1 && c->sda_io_num==0 && c->scl_io_num==1); ++bus_creates; *out=reinterpret_cast<void*>(2); return ESP_OK;
}
inline int i2c_del_master_bus(void*) { return ESP_OK; }
inline int i2c_master_bus_add_device(void*, const i2c_device_config_t* c, I2c** out) { ++add_calls[c->device_address];
    if(c->device_address==block_add_address) {
        add_entered=true; while(!release_add.load()) std::this_thread::yield();
    }
    if(c->device_address==add_failure_address) return ESP_FAIL;
    *out=&i2c[c->device_address]; return ESP_OK;
}
inline int i2c_master_bus_rm_device(I2c*) { return remove_fails ? ESP_FAIL : ESP_OK; }
inline int i2c_master_probe(void*, uint16_t address, int timeout) { assert(timeout==5); ++probes; if(probe_errors[address])return ESP_FAIL; return i2c[address].present ? ESP_OK : ESP_ERR_NOT_FOUND; }
inline int i2c_master_transmit_receive(I2c* d, const uint8_t* tx, size_t n, uint8_t* rx, size_t bytes, int timeout) {
    assert(n>=1 && (timeout==5 || (timeout>=20 && timeout<=500)) && size_t(tx[0])+bytes<=256);
    last_timeout=timeout;
    if(d->fail) return ESP_FAIL;
    std::memcpy(rx,d->data+tx[0],bytes); return ESP_OK;
}
inline int i2c_master_transmit(I2c* d, const uint8_t* tx, size_t n, int timeout) {
    assert(timeout>=20 && timeout<=500 && n); last_timeout=timeout; if(d->fail) return ESP_FAIL; assert(size_t(tx[0])+n-1<=256);
    std::memcpy(d->data+tx[0],tx+1,n-1); return ESP_OK;
}
inline int i2c_master_receive(I2c* d, uint8_t* rx, size_t n, int) { std::memcpy(rx,d->data,n); return ESP_OK; }
using SemaphoreHandle_t=int*;
struct StaticSemaphore_t { int lock=0; };
constexpr int pdTRUE=1, portMAX_DELAY=-1;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) { return &s->lock; }
inline int xSemaphoreTake(SemaphoreHandle_t s, int) { assert(!*s); *s=1; return 1; }
inline void xSemaphoreGive(SemaphoreHandle_t s) { assert(*s); *s=0; }
struct StaticTask_t {};
using StackType_t=uint32_t;
using TickType_t=uint32_t;
using TaskHandle_t=void*;
inline int64_t now_us=0;
inline int64_t esp_timer_get_time() { return now_us; }
inline void vTaskDelay(int ticks) { now_us+=ticks*1000; }
#define pdMS_TO_TICKS(x) (x)
inline TaskHandle_t xTaskCreateStatic(void(*)(void*),const char*,size_t,void*,unsigned,StackType_t*,StaticTask_t*) { return reinterpret_cast<void*>(1); }
inline unsigned notify_calls=0;
inline bool worker_deleted=false;
inline void xTaskNotifyGive(void*) { assert(!worker_deleted); ++notify_calls; }
inline void vTaskDelete(void*) {}
inline int ulTaskNotifyTake(int,int) { return 0; }
#define ESP_LOGW(...) ((void)0)
constexpr int UART_NUM_1=1, UART_DATA_8_BITS=8, UART_PARITY_DISABLE=0, UART_STOP_BITS_1=1, UART_HW_FLOWCTRL_DISABLE=0, UART_SCLK_DEFAULT=0, UART_PIN_NO_CHANGE=-1;
struct uart_config_t { uint32_t baud_rate; int data_bits,parity,stop_bits,flow_ctrl,source_clk; };
inline bool uart_live=false, fail_uart_install=false, fail_uart_delete=false;
inline unsigned uart_deletes=0;
inline bool uart_is_driver_installed(int) { return uart_live; }
inline int uart_param_config(int,const uart_config_t*) { return ESP_OK; }
inline int uart_set_pin(int,int tx,int rx,int,int) { assert(tx==58 && rx==59); return ESP_OK; }
inline int uart_driver_install(int,int,int,int,void*,int) { if(fail_uart_install)return ESP_FAIL; uart_live=true; return ESP_OK; }
inline int uart_driver_delete(int) { ++uart_deletes; assert(uart_live); if(fail_uart_delete)return ESP_FAIL; uart_live=false; return ESP_OK; }
inline int uart_read_bytes(int,uint8_t*,size_t,int timeout) { assert(timeout==0); return 0; }
inline int uart_tx_chars(int,const char*,size_t size) { return size; }
