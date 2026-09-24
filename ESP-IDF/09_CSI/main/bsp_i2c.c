#include "bsp_i2c.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t bus_mutex;
static int bus_port = -1, bus_sda = -1, bus_scl = -1;
static uint32_t bus_frequency;
#define BUS_TIMEOUT_MS 100
static TickType_t timeout_ticks(void) {
    TickType_t ticks = pdMS_TO_TICKS(BUS_TIMEOUT_MS);
    return ticks ? ticks : 1;
}

esp_err_t bsp_i2c_init(int port, int sda, int scl, uint32_t frequency_hz) {
    if (port < 0 || port >= I2C_NUM_MAX || sda == scl ||
        !GPIO_IS_VALID_OUTPUT_GPIO(sda) || !GPIO_IS_VALID_OUTPUT_GPIO(scl) ||
        frequency_hz == 0 || frequency_hz > 400000) return ESP_ERR_INVALID_ARG;
    if (bus_mutex) {
        return port == bus_port && sda == bus_sda && scl == bus_scl &&
               frequency_hz == bus_frequency ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    // 保持与当前 Display_Panel / HYN 示例的 IDF legacy I2C 驱动兼容。
    // 后续其他器件复用本模块，禁止同时用 Wire 或新 I2C 驱动接管此总线。
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda, .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = frequency_hz,
        .clk_flags = 0,
    };
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return ESP_ERR_NO_MEM;
    esp_err_t err = i2c_param_config((i2c_port_t)port, &config);
    if (err == ESP_OK) err = i2c_driver_install((i2c_port_t)port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) { vSemaphoreDelete(mutex); return err; }
    bus_port = port; bus_sda = sda; bus_scl = scl; bus_frequency = frequency_hz;
    bus_mutex = mutex;
    return ESP_OK;
}

esp_err_t bsp_i2c_probe(uint8_t address) {
    if (address < 0x08 || address > 0x77) return ESP_ERR_INVALID_ARG;
    if (!bus_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(bus_mutex, timeout_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (cmd) {
        err = i2c_master_start(cmd);
        if (err == ESP_OK) err = i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
        if (err == ESP_OK) err = i2c_master_stop(cmd);
        if (err == ESP_OK) err = i2c_master_cmd_begin((i2c_port_t)bus_port, cmd, timeout_ticks());
        i2c_cmd_link_delete(cmd);
    }
    xSemaphoreGive(bus_mutex);
    return err;
}

esp_err_t bsp_i2c_transfer(uint8_t address, const uint8_t *tx, size_t tx_size,
                           uint8_t *rx, size_t rx_size) {
    if (address < 0x08 || address > 0x77 || (!tx_size && !rx_size) ||
        (tx_size && !tx) || (rx_size && !rx)) return ESP_ERR_INVALID_ARG;
    if (!bus_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(bus_mutex, timeout_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (tx_size) err = i2c_master_write_to_device((i2c_port_t)bus_port, address,
                                                 tx, tx_size, timeout_ticks());
    // 写失败则不继续读，避免错误数据被当作有效信息。
    if (err == ESP_OK && rx_size)
        err = i2c_master_read_from_device((i2c_port_t)bus_port, address, rx, rx_size, timeout_ticks());
    xSemaphoreGive(bus_mutex);
    return err;
}
