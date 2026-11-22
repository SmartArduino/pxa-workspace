/* SPDX-License-Identifier: MIT
 * Pai Touch hardware port for the unmodified Aster application/service model.
 * JD9853 supplier sequence is generated from the existing board integration.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aster_idf_runtime.h"
#include "aster_idf_apps.h"
#include "aster_idf_wifi.h"
#include "aster_storage.h"
#include "aster_device.h"
#include "aster_os.h"
#include "aster_services.h"
#include "aster_network.h"

#define W 296
#define H 240
static esp_lcd_panel_io_handle_t panel_io;
static esp_lcd_panel_handle_t panel;
static i2c_master_dev_handle_t touch_device;
static uint16_t *logical, *rotated;
static uint64_t completed, rotate_us, transfer_us;
static const char *TAG="pai_aster";
typedef struct {uint8_t command,data[32],length;uint16_t delay_ms;} LcdCommand;
static const LcdCommand commands[]={
#include "lcd_commands.inc"
};

static void display_init(void)
{
    gpio_config_t pins={.pin_bit_mask=(1ULL<<41),.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&pins));gpio_set_level(41,0);
    spi_bus_config_t bus={.mosi_io_num=38,.miso_io_num=-1,.sclk_io_num=47,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=H*16*2};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io={.cs_gpio_num=48,.dc_gpio_num=40,
        .spi_mode=3,.pclk_hz=80000000,.trans_queue_depth=2,
        .lcd_cmd_bits=8,.lcd_param_bits=8};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST,&io,&panel_io));
    esp_lcd_panel_dev_config_t config={.reset_gpio_num=21,
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io,&config,&panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));vTaskDelay(pdMS_TO_TICKS(120));
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io,commands[i].command,
            commands[i].length?commands[i].data:NULL,commands[i].length));
        if(commands[i].delay_ms)vTaskDelay(pdMS_TO_TICKS(commands[i].delay_ms));
    }
    logical=heap_caps_calloc(W*H,2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    rotated=heap_caps_aligned_alloc(64,W*H*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    assert(logical && rotated);
    gpio_set_level(41,1);
}

static int present(const uint8_t *source,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    if(!source || x>=W || y>=H || w>W-x || h>H-y || pitch<w)return -1;
    int64_t start=esp_timer_get_time();
    for(unsigned row=0;row<h;row++)
        memcpy(logical+(y+row)*W+x,source+(size_t)row*pitch*2,w*2);
    /* Same native 240x296 scan direction as the PXA board: physical=(239-y,x).
     * Tile traversal bounds PSRAM cache misses during the 90-degree rotation. */
    for(unsigned bx=0;bx<W;bx+=16)for(unsigned by=0;by<H;by+=16)
        for(unsigned xx=bx;xx<W && xx<bx+16;xx++)
            for(unsigned yy=by;yy<H && yy<by+16;yy++)
                rotated[xx*H+H-1-yy]=__builtin_bswap16(logical[yy*W+xx]);
    rotate_us+=esp_timer_get_time()-start;start=esp_timer_get_time();
    for(unsigned row=0;row<W;row+=16) {
        unsigned lines=W-row<16?W-row:16;
        if(esp_lcd_panel_draw_bitmap(panel,0,row,H,row+lines,rotated+row*H)!=ESP_OK)return -1;
    }
    /* Drain every queued transaction before exposing a reusable source. */
    if(esp_lcd_panel_io_tx_param(panel_io,-1,NULL,0)!=ESP_OK)return -1;
    transfer_us+=esp_timer_get_time()-start;completed++;return 0;
}
static void perf_reset(void){completed=rotate_us=transfer_us=0;}
static void perf_report(void)
{printf("DISPLAY completed=%llu rotate_us=%llu transfer_us=%llu spi_hz=80000000 width=%u height=%u\n",completed,rotate_us,transfer_us,W,H);}
static int app_command(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"ui") && !strcmp(argv[2],"display-perf")){perf_report();return 0;}
    if(argc==3 && !strcmp(argv[1],"ui") && !strcmp(argv[2],"memory")){
        printf("MEMORY internal_free=%u psram_free=%u internal_min=%u psram_min=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));return 0;
    }
    return aster_idf_apps_command(argc,argv);
}
static const uint8_t *capture(void){return (const uint8_t *)logical;}

static int touch_frame(aster_idf_touch_point_t points[ASTER_IDF_MAX_TOUCH])
{
    if(!touch_device)return -1;
    uint8_t reg=0,raw[15];
    if(i2c_master_transmit_receive(touch_device,&reg,1,raw,sizeof(raw),20)!=ESP_OK)return -1;
    unsigned count=raw[2]&15;if(count>2)return -1;
    int mask=0;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *p=raw+3+i*6;
        unsigned x=((p[0]&15)<<8)|p[1],y=((p[2]&15)<<8)|p[3];
        unsigned slot=p[2]>>4;if(slot>=2)slot=i;
        if(x<H && y<W){points[slot]=(aster_idf_touch_point_t){.x=y,.y=H-1-x};mask|=1<<slot;}
    }
    return mask;
}
static void touch_init(void)
{
    gpio_config_t reset={.pin_bit_mask=1ULL<<46,.mode=GPIO_MODE_OUTPUT};gpio_config(&reset);
    gpio_set_level(46,0);vTaskDelay(pdMS_TO_TICKS(10));gpio_set_level(46,1);vTaskDelay(pdMS_TO_TICKS(120));
    i2c_master_bus_config_t config={.i2c_port=1,.sda_io_num=45,.scl_io_num=42,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t bus;ESP_ERROR_CHECK(i2c_new_master_bus(&config,&bus));
    i2c_device_config_t device={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x15,.scl_speed_hz=400000};
    if(i2c_master_probe(bus,0x15,100)==ESP_OK) {
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&device,&touch_device));
        ESP_LOGI(TAG,"CST826 touch detected, two stable slots");
    }else ESP_LOGW(TAG,"CST826 unavailable");
}
static void storage_init(void)
{
    wl_handle_t handle=WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t config={.format_if_mount_failed=false,.max_files=32,.allocation_unit_size=4096};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/SD","storage",&config,&handle));
    int result=aster_storage_init();ESP_LOGI(TAG,"Flash FAT Aster storage=%d",result);
}
void app_main(void)
{
    ESP_LOGI(TAG,"Aster Pai Touch IDF port; 240MHz, 80MHz PSRAM/SPI; audio/camera not exposed");
    display_init();touch_init();storage_init();
    static const aster_system_info_t identity={.platform="esp-idf",.board="pai-touch",.soc="esp32s3",
        .cpu_arch=ASTER_ARCH_XTENSA,.cpu_freq_hz=240000000,.cpu_count=2,
        .ram_internal=512*1024,.ram_external=8*1024*1024,.flash_size=16*1024*1024};
    aster_idf_io_t io={.width=W,.height=H,.present=present,.touch_frame=touch_frame,.max_touch_points=2,
        .display_perf_reset=perf_reset,.display_perf_report=perf_report,.system_info=&identity};
    aster_idf_set_io(&io);aster_idf_set_capture(capture);
    aster_device_seal();aster_os_seal();aster_services_seal();aster_network_seal();
    ESP_ERROR_CHECK(aster_idf_apps_start());
    aster_idf_set_app_command(app_command);aster_idf_network_start();
    extern int aster_idf_transfer_register(void);
    ESP_ERROR_CHECK(aster_idf_transfer_register());
    ESP_LOGI(TAG,"Pai Touch ready: real Aster desktop and AOT runtime");
}
