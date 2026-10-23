#include <stdint.h>
#include "epd_board.h"
#include "epdiy.h"

#include "../output_common/render_method.h"
#include "../output_lcd/lcd_driver.h"
#include "esp_log.h"
#include "pca9555.h"
#include "tps65185.h"

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <sdkconfig.h>

// Make this compile von the ESP32 without ifdefing the whole file
#ifndef CONFIG_IDF_TARGET_ESP32S3
#define GPIO_NUM_40 -1
#define GPIO_NUM_41 -1
#define GPIO_NUM_42 -1
#define GPIO_NUM_43 -1
#define GPIO_NUM_44 -1
#define GPIO_NUM_45 -1
#define GPIO_NUM_46 -1
#define GPIO_NUM_47 -1
#define GPIO_NUM_48 -1
#endif

// EPDiy V7 schematic: U6 40-pin EPD connector footprint + U8 ESP32-S3 + U2 PCA9535PW.
#define CFG_SCL GPIO_NUM_40   // U2 SCL, ESP GPIO40
#define CFG_SDA GPIO_NUM_39   // U2 SDA, ESP GPIO39
#define CFG_INTR GPIO_NUM_38  // U2 INT / ~GPIO_INTR, ESP GPIO38
#define EPDIY_I2C_PORT I2C_NUM_0

#define CFG_PIN_OE (PCA_PIN_PC10 >> 8)         // U2 IO1_0 -> EP_OE -> U6 pin 33 XOE
#define CFG_PIN_MODE (PCA_PIN_PC11 >> 8)       // U2 IO1_1 -> EP_MODE -> U6 pin 6 Mode / pin 28 MODE2
#define CFG_PIN_PWRUP (PCA_PIN_PC13 >> 8)      // U2 IO1_3 -> TPS_PWRUP
#define CFG_PIN_VCOM_CTRL (PCA_PIN_PC14 >> 8)  // U2 IO1_4 -> TPS_VCOM_CTRL
#define CFG_PIN_WAKEUP (PCA_PIN_PC15 >> 8)     // U2 IO1_5 -> TPS_WAKEUP
#define CFG_PIN_PWRGOOD (PCA_PIN_PC16 >> 8)    // U2 IO1_6 <- TPS_PWR_GOOD
#define CFG_PIN_INT (PCA_PIN_PC17 >> 8)        // U2 IO1_7 <- TPS_nINT

#define D15 GPIO_NUM_47  // U6 pin 30 D15
#define D14 GPIO_NUM_21  // U6 pin 29 D14
#define D13 GPIO_NUM_14  // U6 pin 28 D13
#define D12 GPIO_NUM_13  // U6 pin 27 D12
#define D11 GPIO_NUM_12  // U6 pin 26 D11
#define D10 GPIO_NUM_11  // U6 pin 25 D10
#define D9 GPIO_NUM_10   // U6 pin 24 D9
#define D8 GPIO_NUM_9    // U6 pin 23 D8

#define D7 GPIO_NUM_8    // U6 pin 21 D7
#define D6 GPIO_NUM_18   // U6 pin 20 D6
#define D5 GPIO_NUM_17   // U6 pin 19 D5
#define D4 GPIO_NUM_16   // U6 pin 18 D4
#define D3 GPIO_NUM_15   // U6 pin 17 D3
#define D2 GPIO_NUM_7    // U6 pin 16 D2
#define D1 GPIO_NUM_6    // U6 pin 15 D1
#define D0 GPIO_NUM_5    // U6 pin 14 D0

/* Control Lines */
#define CKV GPIO_NUM_48  // U6 pin 7 CKV
#define STH GPIO_NUM_41  // U6 pin 31 XSTL
#define LEH GPIO_NUM_42  // U6 pin 32 LEH
#define STV GPIO_NUM_45  // U6 pin 8 SPV / EP_STV

/* Edges */
#define CKH GPIO_NUM_4   // U6 pin 13 XCL

typedef struct {
    i2c_port_t port;
    bool pwrup;
    bool vcom_ctrl;
    bool wakeup;
    bool others[8];
} epd_config_register_t;

/** The VCOM voltage to use. */
static int vcom = 1600;

static epd_config_register_t config_reg;

static bool interrupt_done = false;

static void IRAM_ATTR interrupt_handler(void* arg) {
    interrupt_done = true;
}

static lcd_bus_config_t lcd_config = {
    .clock = CKH,
    .ckv = CKV,
    .leh = LEH,
    .start_pulse = STH,
    .stv = STV,
    .data[0] = D0,
    .data[1] = D1,
    .data[2] = D2,
    .data[3] = D3,
    .data[4] = D4,
    .data[5] = D5,
    .data[6] = D6,
    .data[7] = D7,
    .data[8] = D8,
    .data[9] = D9,
    .data[10] = D10,
    .data[11] = D11,
    .data[12] = D12,
    .data[13] = D13,
    .data[14] = D14,
    .data[15] = D15,
};

static void epd_board_init(uint32_t epd_row_width) {
    gpio_hold_dis(CKH);  // free CKH after wakeup

    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = CFG_SDA;
    conf.scl_io_num = CFG_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    conf.clk_flags = 0;
    ESP_ERROR_CHECK(i2c_param_config(EPDIY_I2C_PORT, &conf));

    ESP_ERROR_CHECK(i2c_driver_install(EPDIY_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    config_reg.port = EPDIY_I2C_PORT;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;
    for (int i = 0; i < 8; i++) {
        config_reg.others[i] = false;
    }

    gpio_set_direction(CFG_INTR, GPIO_MODE_INPUT);
    gpio_set_intr_type(CFG_INTR, GPIO_INTR_NEGEDGE);

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));

    ESP_ERROR_CHECK(gpio_isr_handler_add(CFG_INTR, interrupt_handler, (void*)CFG_INTR));

    // set all epdiy lines to output except TPS interrupt + PWR good
    ESP_ERROR_CHECK(pca9555_set_config(config_reg.port, CFG_PIN_PWRGOOD | CFG_PIN_INT, 1));

    const EpdDisplay_t* display = epd_get_display();

    LcdEpdConfig_t config = {
        // 16-bit bus sends a line in half the clock cycles; scale clock so line
        // rate matches an 8-bit configuration at bus_speed MHz.
        .pixel_clock = display->bus_speed * 1000 * 1000 * 8 / display->bus_width,
        .ckv_high_time = 131,
        .line_front_porch = 4,
        .le_high_time = 4,
        .bus_width = display->bus_width,
        .bus = lcd_config,
    };
    epd_lcd_init(&config, display->width, display->height);

    ESP_LOGI("epdiy", "EPDiy V7 40-pin GPIO/PCA9555 pin map initialized");
}

static void epd_board_deinit() {
    epd_lcd_deinit();

    ESP_ERROR_CHECK(pca9555_set_config(
        config_reg.port, CFG_PIN_PWRGOOD | CFG_PIN_INT | CFG_PIN_VCOM_CTRL | CFG_PIN_PWRUP, 1
    ));

    int tries = 0;
    while (!((pca9555_read_input(config_reg.port, 1) & 0xC0) == 0x80)) {
        if (tries >= 50) {
            ESP_LOGE("epdiy", "failed to shut down TPS65185!");
            break;
        }
        tries++;
        vTaskDelay(1);
    }

    // Not sure why we need this delay, but the TPS65185 seems to generate an interrupt after some
    // time that needs to be cleared.
    vTaskDelay(50);
    pca9555_read_input(config_reg.port, 0);
    pca9555_read_input(config_reg.port, 1);
    i2c_driver_delete(EPDIY_I2C_PORT);

    gpio_uninstall_isr_service();
}

static void epd_board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv) {
        uint8_t value = 0x00;

        if (state->ep_output_enable)
            value |= CFG_PIN_OE;
        if (state->ep_mode)
            value |= CFG_PIN_MODE;
        if (config_reg.pwrup)
            value |= CFG_PIN_PWRUP;
        if (config_reg.vcom_ctrl)
            value |= CFG_PIN_VCOM_CTRL;
        if (config_reg.wakeup)
            value |= CFG_PIN_WAKEUP;

        ESP_ERROR_CHECK(pca9555_set_value(config_reg.port, value, 1));
    }
}

static void epd_board_poweron(epd_ctrl_state_t* state) {
    // 1. 初始化 Mask
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };

    // ★ 关键 1：确保输出关闭，减轻负载
    state->ep_output_enable = false; 
    state->ep_stv = true;
    state->ep_mode = false;

    // 2. 只有 WAKEUP 先拉高 (唤醒芯片，但还没开启升压)
    config_reg.wakeup = true;
    config_reg.pwrup = false;   // 先不开启 Power Up
    config_reg.vcom_ctrl = false; 
    epd_board_set_ctrl(state, &mask);
    
    vTaskDelay(pdMS_TO_TICKS(10)); // 给它一点唤醒时间

    // ========================================================
    // ★ 关键 2：重试循环机制 (最多试 3 次)
    // ========================================================
    int retry_count = 0;
    bool success = false;

    while (retry_count < 3 && !success) {
        if (retry_count > 0) {
            ESP_LOGW("epdiy", "TPS startup failed, retrying... (%d/3)", retry_count + 1);
            // 失败后先拉低一下 PWRUP 复位状态
            config_reg.pwrup = false;
            epd_board_set_ctrl(state, &mask);
            vTaskDelay(pdMS_TO_TICKS(100)); // 歇 100ms 让电源电压回升
        }

        // --- 尝试启动 ---
        config_reg.pwrup = true; // 拉高 PWRUP，开始升压序列
        config_reg.vcom_ctrl = true;
        epd_board_set_ctrl(state, &mask);

        // 等待 Power Good (带超时)
        int wait_pwr_good = 0;
        bool pg_ok = false;
        
        // 循环检测 PWRGOOD 引脚
        while (wait_pwr_good < 200) { // 200ms 超时
            if (pca9555_read_input(config_reg.port, 1) & CFG_PIN_PWRGOOD) {
                pg_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            wait_pwr_good++;
        }

        if (pg_ok) {
            success = true; // 硬件信号好了
        } else {
            retry_count++; // 没好，准备下一次重试
        }
    }

    // ========================================================
    // 3. 结果判断
    // ========================================================
    if (!success) {
        ESP_LOGE("epdiy", "TPS PowerOn Failed after 3 retries!");
        // 读取错误码看看
        uint8_t status = tps_read_register(config_reg.port, 0x01);
        ESP_LOGE("epdiy", "Final Status Reg: 0x%02X", status);
        return; // 放弃治疗
    }

    // 4. 配置 TPS 寄存器
    ESP_ERROR_CHECK(tps_write_register(config_reg.port, TPS_REG_ENABLE, 0x3F));
    tps_set_vcom(config_reg.port, vcom);

    state->ep_sth = true;
    epd_ctrl_state_t sth_mask = { .ep_sth = true };
    epd_board_set_ctrl(state, &sth_mask);

    // 5. 等待 PG (Power Good) 寄存器标志
    int tries = 0;
    while (!((tps_read_register(config_reg.port, TPS_REG_PG) & 0xFA) == 0xFA)) {
        if (tries >= 200) {
             ESP_LOGE("epdiy", "TPS Register PG Timeout!");
             return;
        }
        tries++;
        vTaskDelay(1);
    }

    // ★ 关键 3：一切就绪后，终于开启屏幕输出
    state->ep_output_enable = true; 
    epd_ctrl_state_t out_mask = { .ep_output_enable = true };
    epd_board_set_ctrl(state, &out_mask);

    ESP_LOGI("epdiy", "TPS Startup Success!");
}

static void epd_board_measure_vcom(epd_ctrl_state_t* state) {
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };
    state->ep_stv = true;
    state->ep_mode = false;
    state->ep_output_enable = true;
    config_reg.wakeup = true;
    epd_board_set_ctrl(state, &mask);
    config_reg.pwrup = true;
    epd_board_set_ctrl(state, &mask);

    // give the IC time to powerup and set lines
    vTaskDelay(1);
    state->ep_sth = true;
    mask = (const epd_ctrl_state_t){
        .ep_sth = true,
    };
    epd_board_set_ctrl(state, &mask);

    while (!(pca9555_read_input(config_reg.port, 1) & CFG_PIN_PWRGOOD)) {
    }
    ESP_LOGI("epdiy", "Power rails enabled");

    state->ep_sth = true;
    mask = (const epd_ctrl_state_t){
        .ep_sth = true,
    };
    epd_board_set_ctrl(state, &mask);

    int tries = 0;
    while (!((tps_read_register(config_reg.port, TPS_REG_PG) & 0xFA) == 0xFA)) {
        if (tries >= 500) {
            ESP_LOGE(
                "epdiy",
                "Power enable failed! PG status: %X",
                tps_read_register(config_reg.port, TPS_REG_PG)
            );
            return;
        }
        tries++;
        vTaskDelay(1);
    }
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {   
    epd_ctrl_state_t mask = {
        .ep_stv = true,
        .ep_output_enable = true,
        .ep_mode = true,
    };
    config_reg.vcom_ctrl = false;
    config_reg.pwrup = false;
    state->ep_stv = false;
    state->ep_output_enable = false;
    state->ep_mode = false;
    epd_board_set_ctrl(state, &mask);
    vTaskDelay(1);
    config_reg.wakeup = false;
    epd_board_set_ctrl(state, &mask);

}

static float epd_board_ambient_temperature() {
    return 20;
}

static void set_vcom(int value) {
    vcom = value;
}

const EpdBoardDefinition epd_board_v7 = {
    .init = epd_board_init,
    .deinit = epd_board_deinit,
    .set_ctrl = epd_board_set_ctrl,
    .poweron = epd_board_poweron,
    .poweroff = epd_board_poweroff,

    .measure_vcom = epd_board_measure_vcom,
    .get_temperature = epd_board_ambient_temperature,
    .set_vcom = set_vcom,

    // unimplemented for now, but shares v6 implementation
    .gpio_set_direction = NULL,
    .gpio_read = NULL,
    .gpio_write = NULL,
};
