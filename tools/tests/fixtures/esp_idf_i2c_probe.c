/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
// Probe regression fixture from ESP-IDF i2c_master.c (reviewed source hash
// a235f01f9bb2dd665624fb0fde57998a1c7446db1aaa32a75c487bba82723e49).
static void transfer_start(i2c_master_bus_t *i2c_master) {
    i2c_master->rx_cnt = 0;
    i2c_master->read_len_static = 0;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "i2c handle not initialized");
    TickType_t ticks_to_wait = (xfer_timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(xfer_timeout_ms);
    if (xSemaphoreTake(bus_handle->bus_lock_mux, ticks_to_wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_OK;

    bus_handle->cmd_idx = 0;
    bus_handle->trans_idx = 0;
    bus_handle->trans_done = false;
    bus_handle->status = I2C_STATUS_IDLE;
    bus_handle->bypass_nack_log = true;
    i2c_hal_context_t *hal = &bus_handle->base->hal;
    i2c_operation_t i2c_ops[] = {
        {.hw_cmd = I2C_TRANS_START_COMMAND},
        {.hw_cmd = I2C_TRANS_STOP_COMMAND},
    };

    bus_handle->i2c_trans = (i2c_transaction_t) {
        .device_address = address,
        .ops = i2c_ops,
        .cmd_count = DIM(i2c_ops),
    };

    // I2C probe does not have i2c device module. So set the clock parameter independently
    // This will not influence device transaction.
    PERIPH_RCC_ATOMIC() {
        i2c_ll_set_source_clk(hal->dev, bus_handle->base->clk_src);
        i2c_hal_set_bus_timing(hal, 100000, bus_handle->base->clk_src, bus_handle->base->clk_src_freq_hz);
    }
    i2c_ll_txfifo_rst(hal->dev);
    i2c_ll_rxfifo_rst(hal->dev);
    i2c_ll_master_set_fractional_divider(hal->dev, 0, 0);
    i2c_ll_enable_intr_mask(hal->dev, I2C_LL_MASTER_EVENT_INTR);
    // 20ms is sufficient for stretch, since there is no device config on probe operation.
    i2c_hal_master_set_scl_timeout_val(hal, 20 * 1000, bus_handle->base->clk_src_freq_hz);
    i2c_ll_update(hal->dev);

    s_i2c_send_commands(bus_handle, ticks_to_wait);
    if (bus_handle->status == I2C_STATUS_ACK_ERROR) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (bus_handle->status == I2C_STATUS_TIMEOUT) {
        ESP_LOGE(TAG, "probe device timeout. Please check if xfer_timeout_ms and pull-ups are correctly set up");
        ret = ESP_ERR_TIMEOUT;
    }

    // Reset the status to done, in order not influence next time transaction.
    bus_handle->status = I2C_STATUS_DONE;
    bus_handle->bypass_nack_log = false;
    i2c_ll_disable_intr_mask(hal->dev, I2C_LL_MASTER_EVENT_INTR);
    xSemaphoreGive(bus_handle->bus_lock_mux);
    return ret;
}

esp_err_t fixture_end(void) {return 0;}
