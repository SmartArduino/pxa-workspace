#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=1, ESP_ERR_INVALID_STATE=2;
constexpr int ESP_ERR_NO_MEM=3, ESP_ERR_INVALID_RESPONSE=4;
inline const char* esp_err_to_name(int) { return "test error"; }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
inline int64_t test_now_us=0;
inline int64_t esp_timer_get_time() { return test_now_us; }
inline void esp_rom_delay_us(uint32_t us) { test_now_us+=us; }
#define pdMS_TO_TICKS(ms) (ms)
constexpr int portMAX_DELAY=-1;
inline void vTaskDelay(uint32_t ticks) { test_now_us+=int64_t(ticks)*1000; }
struct TestI2c { bool present=false, fail=false; uint8_t reg[256]{}; };
inline TestI2c test_i2c[128];
using i2c_master_bus_handle_t=void*;
using i2c_master_dev_handle_t=TestI2c*;
constexpr int I2C_ADDR_BIT_LEN_7=7;
struct i2c_device_config_t { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; };
inline int test_i2c_removed=0;
inline int i2c_master_probe(void*,uint16_t addr,int) { return test_i2c[addr].present?ESP_OK:ESP_FAIL; }
inline int i2c_master_bus_add_device(void*,const i2c_device_config_t* c,TestI2c** out) {
    assert(c->scl_speed_hz==400000); *out=&test_i2c[c->device_address]; return ESP_OK;
}
inline int i2c_master_bus_rm_device(TestI2c*) { ++test_i2c_removed; return ESP_OK; }
inline int i2c_master_transmit_receive(TestI2c* d,const uint8_t* reg,size_t n,uint8_t* out,size_t bytes,int timeout) {
    assert(n==1 && timeout==20); if(d->fail)return ESP_FAIL;
    assert(size_t(*reg)+bytes<=256); std::memcpy(out,d->reg+*reg,bytes); return ESP_OK;
}
inline int i2c_master_transmit(TestI2c* d,const uint8_t* data,size_t n,int timeout) {
    assert(n<=129 && n>=1 && timeout==20); if(d->fail)return ESP_FAIL;
    if(data[0]!=0x5e) { assert(size_t(data[0])+n-1<=256); std::memcpy(d->reg+data[0],data+1,n-1); }
    return ESP_OK;
}
using gpio_num_t=int;
constexpr int GPIO_NUM_8=8, GPIO_MODE_OUTPUT=1, GPIO_PULLUP_ENABLE=1;
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; };
inline int test_fail_stage=0;
inline bool test_nand_control_high[2]={}, test_nand_control_configured[2]={};
inline int test_motor_level=0;
inline int gpio_set_level(int pin,int level) {
    if(pin==8) {test_motor_level=level;return ESP_OK;}
    assert((pin==24 || pin==25) && level==1);
    if(test_fail_stage==11)return ESP_FAIL;
    test_nand_control_high[pin-24]=true;return ESP_OK;
}
inline int gpio_config(const gpio_config_t* c) {
    if(c->pin_bit_mask==256)return ESP_OK;
    const int index=c->pin_bit_mask==(uint64_t(1)<<24)?0:1;
    assert(c->pin_bit_mask==(uint64_t(1)<<(24+index)));
    assert(test_nand_control_high[index] && c->mode==GPIO_MODE_OUTPUT && c->pull_up_en==GPIO_PULLUP_ENABLE);
    test_nand_control_configured[index]=true;return ESP_OK;
}
using SemaphoreHandle_t=int*;
inline int test_mutex=0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return &test_mutex; }
inline void xSemaphoreTake(SemaphoreHandle_t,int) { assert(!test_mutex);test_mutex=1; }
inline void xSemaphoreGive(SemaphoreHandle_t) { assert(test_mutex);test_mutex=0; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
struct esp_timer_create_args_t { void(*callback)(void*); const char* name; };
struct TestTimer { void(*callback)(void*); bool running; int64_t due_us; };
using esp_timer_handle_t=TestTimer*;
inline TestTimer test_timer{};
inline bool test_timer_fail=false;
inline int esp_timer_create(const esp_timer_create_args_t* c,esp_timer_handle_t* out) {
    test_timer.callback=c->callback;*out=&test_timer;return ESP_OK;
}
inline int esp_timer_start_once(TestTimer* timer,uint64_t us) {
    if(test_timer_fail)return ESP_FAIL;
    timer->running=true;timer->due_us=test_now_us+us;return ESP_OK;
}
inline int esp_timer_stop(TestTimer* timer) { timer->running=false;return ESP_OK; }

constexpr int SPI3_HOST=3, SPI_DMA_CH_AUTO=1, SPI_DEVICE_HALFDUPLEX=16;
using spi_device_handle_t=void*;
struct spi_bus_config_t { int mosi_io_num,miso_io_num,sclk_io_num,quadhd_io_num,quadwp_io_num,max_transfer_sz,data4_io_num,data5_io_num,data6_io_num,data7_io_num; };
struct spi_device_interface_config_t { int clock_speed_hz,spics_io_num,queue_size,flags; };
constexpr int SPI_TRANS_VARIABLE_ADDR=1,SPI_TRANS_VARIABLE_CMD=2,SPI_TRANS_VARIABLE_DUMMY=4;
constexpr int SPI_TRANS_USE_RXDATA=8,SPI_TRANS_USE_TXDATA=16;
struct spi_transaction_t {
    int flags=0;uint16_t cmd=0;uint64_t addr=0;size_t length=0,rxlength=0;
    uint8_t rx_data[4]{},tx_data[4]{};
};
struct spi_transaction_ext_t { spi_transaction_t base;uint8_t command_bits=0,address_bits=0,dummy_bits=0; };
inline int test_bus_free=0,test_spi_removed=0,test_bdl_released=0,test_unmounted=0;
inline bool test_spi_remove_fail=false, test_spi_free_fail=false, test_spi_live=false, test_spi_device_live=false;
inline bool test_spi_quad=false, test_bootstrap_remove_fail=false, test_bootstrap_free_fail=false;
inline uint8_t test_nand_config=0x10;
inline int test_spi_initializes=0, test_quad_writes=0;
inline int spi_bus_initialize(int host,const spi_bus_config_t* c,int) {
    assert(host==3 && c->sclk_io_num==20 && c->mosi_io_num==21 && c->miso_io_num==22);
    const bool quad=c->quadhd_io_num==24;
    assert(c->quadhd_io_num==(quad?24:-1) && c->quadwp_io_num==(quad?25:-1));
    assert(test_nand_control_configured[0] && test_nand_control_configured[1]);
    if(quad)assert(test_nand_config&1); // Reproduces a cold chip held by IO3 before QE.
    assert(c->data4_io_num==-1 && c->data5_io_num==-1 && c->data6_io_num==-1 && c->data7_io_num==-1);
    ++test_spi_initializes; assert(!test_spi_live);
    if(test_fail_stage==1 || (test_fail_stage==18 && quad))return ESP_FAIL;
    test_spi_quad=quad;test_spi_live=true; return ESP_OK;
}
inline int spi_bus_add_device(int,const spi_device_interface_config_t* c,void** out) {
    assert(c->spics_io_num==23 && c->clock_speed_hz==20000000 && c->flags==16);
    if(test_fail_stage==2 || (test_fail_stage==19 && test_spi_quad))return ESP_FAIL;
    assert(test_spi_live && !test_spi_device_live); test_spi_device_live=true;
    *out=reinterpret_cast<void*>(1);return ESP_OK;
}
inline int spi_bus_free(int) {
    ++test_bus_free; assert(test_spi_live && !test_spi_device_live);
    if(test_spi_quad?test_spi_free_fail:test_bootstrap_free_fail)return ESP_FAIL;
    test_spi_live=false; return ESP_OK;
}
inline int spi_bus_remove_device(void*) {
    ++test_spi_removed; assert(test_spi_device_live);
    if(test_spi_quad?test_spi_remove_fail:test_bootstrap_remove_fail)return ESP_FAIL;
    test_spi_device_live=false; return ESP_OK;
}
inline int spi_device_transmit(void*,spi_transaction_t* t) {
    assert(test_spi_live && test_spi_device_live && !test_spi_quad);
    auto* ext=reinterpret_cast<spi_transaction_ext_t*>(t);
    assert(ext->command_bits==8 && ext->address_bits==8 && !ext->dummy_bits);
    const int variable=SPI_TRANS_VARIABLE_ADDR|SPI_TRANS_VARIABLE_CMD|SPI_TRANS_VARIABLE_DUMMY;
    if(t->cmd==0x1f) {
        assert(t->flags==(variable|SPI_TRANS_USE_TXDATA) && t->addr==0xb0 && t->length==8 && !t->rxlength);
        assert(t->tx_data[0]==(test_nand_config|1)); // Preserve ECC and all unrelated feature bits.
        ++test_quad_writes;
        if(test_fail_stage==17)return ESP_FAIL;
        if(test_fail_stage!=14)test_nand_config=t->tx_data[0];
    } else {
        assert(t->flags==(variable|SPI_TRANS_USE_RXDATA) && !t->length);
        if(t->cmd==0x9f) {
            assert(!t->addr && t->rxlength==16);
            if(test_fail_stage==16)return ESP_FAIL;
            t->rx_data[0]=test_fail_stage==12?0xff:0xc8;t->rx_data[1]=0x91;
        } else {
            assert(t->cmd==0x0f && t->addr==0xb0 && t->rxlength==8);
            if(test_fail_stage==13)return ESP_FAIL;
            t->rx_data[0]=test_nand_config;
        }
    }
    return ESP_OK;
}
struct TestBdl;
struct TestBdlOps { int(*release)(TestBdl*); };
struct TestBdl { TestBdlOps* ops; };
using esp_blockdev_handle_t=TestBdl*;
inline int ReleaseBdl(TestBdl*) { ++test_bdl_released;return ESP_OK; }
inline TestBdlOps test_bdl_ops{ReleaseBdl};
inline TestBdl test_bdl{&test_bdl_ops};
constexpr int SPI_NAND_IO_MODE_QIO=4;
struct spi_nand_flash_config_t { void* device_handle; uint8_t gc_factor;int io_mode;uint8_t flags; };
inline int spi_nand_flash_init_with_layers(spi_nand_flash_config_t* c,TestBdl** out) {
    assert(c->device_handle && c->io_mode==4 && c->flags==16);
    assert(test_spi_quad && (test_nand_config&1));
    if(test_fail_stage==3)return ESP_FAIL;
    *out=&test_bdl;return ESP_OK;
}
struct esp_vfs_fat_mount_config_t { bool format_if_mount_failed;int max_files;size_t allocation_unit_size; };
inline int test_mount_calls=0, test_format_calls=0, test_probe_result=13;
inline bool test_probe_drive_fail=false, test_probe_register_fail=false, test_probe_unmount_fail=false;
inline bool test_probe_registered=false;
inline int esp_vfs_fat_bdl_mount(const char* path,TestBdl*,const esp_vfs_fat_mount_config_t* c) {
    assert(!std::strcmp(path,"/pxa/nand") && c->max_files==16);
#ifdef CONFIG_PXA_MOSAICO_NAND_FORMAT_IF_MOUNT_FAILED
    assert(!test_probe_registered);
    assert(c->format_if_mount_failed==(test_probe_result==13));
#else
    assert(!c->format_if_mount_failed);
#endif
    if(c->format_if_mount_failed)++test_format_calls;
    ++test_mount_calls;return test_fail_stage==4?ESP_FAIL:ESP_OK;
}
inline int esp_vfs_fat_bdl_unmount(const char*,TestBdl*) { ++test_unmounted;return test_fail_stage==8?ESP_FAIL:ESP_OK; }
inline int esp_vfs_fat_info(const char*,uint64_t* total,uint64_t* free) {
    if(test_fail_stage==5)return ESP_FAIL;
    *total=100*1024*1024;*free=80*1024*1024;return ESP_OK;
}
inline int esp_efuse_mac_get_default(uint8_t* out) {
    if(test_fail_stage==6)return ESP_FAIL;
    for(int i=0;i<6;++i)out[i]=i+1;
    return ESP_OK;
}
#define FF_MIN_SS 512
#define FF_MAX_SS 4096
using DWORD=uint32_t;
using BYTE=unsigned char;
struct FATFS { uint32_t csize;uint32_t ssize; };
inline FATFS test_fat{32,2048};
inline FATFS* test_probe_fs=nullptr;
constexpr int FR_OK=0, FR_NO_FILESYSTEM=13, FR_DISK_ERR=1, FR_INT_ERR=2;
inline int ff_diskio_get_drive(BYTE* out) {
    *out=1;return test_probe_drive_fail?ESP_FAIL:ESP_OK;
}
inline int ff_diskio_register_bdl(BYTE drive,TestBdl* bdl) {
    assert(drive==1 && bdl==&test_bdl && !test_probe_registered);
    if(test_probe_register_fail)return ESP_FAIL;
    test_probe_registered=true;return ESP_OK;
}
inline void ff_diskio_clear_pdrv_bdl(TestBdl* bdl) {
    assert(bdl==&test_bdl && test_probe_registered && !test_probe_fs);
    test_probe_registered=false;
}
inline void ff_diskio_unregister(BYTE drive) {
    assert(drive==1 && !test_probe_registered && !test_probe_fs);
}
inline int f_mount(FATFS* fs,const char* drive,BYTE immediate) {
    assert(!std::strcmp(drive,"1:") && test_probe_registered);
    if(fs) {
        assert(immediate==1 && !test_probe_fs);test_probe_fs=fs;return test_probe_result;
    }
    assert(!immediate && test_probe_fs);
    if(test_probe_unmount_fail)return FR_INT_ERR;
    test_probe_fs=nullptr;return FR_OK;
}
inline unsigned char ff_diskio_get_pdrv_bdl(TestBdl*) { return 1; }
inline int f_getfree(const char* drive,DWORD* free,FATFS** out) {
    assert(!std::strcmp(drive,"1:"));*free=40;*out=&test_fat;return FR_OK;
}
