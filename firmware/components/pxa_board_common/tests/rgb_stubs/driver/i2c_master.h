#pragma once
#include <atomic>
#include <cstring>
#include <thread>
#include <cstddef>
#include <cstdint>
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
struct i2c_device_config_t { int dev_addr_length = 0, device_address = 0, scl_speed_hz = 0; };
#define I2C_ADDR_BIT_LEN_7 7
#define ESP_OK 0
inline std::atomic<bool> block_add{false}, block_transfer{false}, entered_driver{false}, release_driver{false};
inline bool fail_add = false, fail_remove = false, fail_transfer = false;
inline unsigned add_calls, remove_calls, transfer_calls;
inline uint8_t last_tx[256];
inline void DriverWait(bool block) {
    if (block) { entered_driver = true; while (!release_driver) std::this_thread::yield(); }
}
inline int i2c_master_bus_add_device(void*, const i2c_device_config_t*, void** out) {
    ++add_calls; DriverWait(block_add);
    if (fail_add) return -1;
    *out = reinterpret_cast<void*>(uintptr_t(0x100 + add_calls)); return 0;
}
inline int i2c_master_bus_rm_device(void*) { ++remove_calls; return fail_remove ? -1 : 0; }
inline int i2c_master_transmit_receive(void*, const uint8_t* tx, size_t n, uint8_t* rx, size_t size, int timeout) {
    if (timeout != 100) return -1;
    ++transfer_calls; DriverWait(block_transfer); std::memcpy(last_tx, tx, n);
    for (size_t i = 0; i < size; ++i) rx[i] = uint8_t(i + 1);
    return fail_transfer ? -1 : 0;
}
inline int i2c_master_transmit(void* h, const uint8_t* tx, size_t n, int timeout) { return i2c_master_transmit_receive(h,tx,n,nullptr,0,timeout); }
inline int i2c_master_receive(void* h, uint8_t* rx, size_t n, int timeout) { return i2c_master_transmit_receive(h,nullptr,0,rx,n,timeout); }
