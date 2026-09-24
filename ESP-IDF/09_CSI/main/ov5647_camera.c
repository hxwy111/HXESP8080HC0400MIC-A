#include "ov5647_camera.h"
#include "bsp_i2c.h"
#include "ov5647_mode.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool detected;
static bool configured;
static esp_err_t read_reg(uint16_t reg, uint8_t *value) {
    uint8_t command[2] = {reg >> 8, reg & 0xFF};
    // SCCB 按厂商驱动习惯分离写地址和读数据，错误原样返回。
    return bsp_i2c_transfer(OV5647_ADDRESS, command, 2, value, 1);
}
static esp_err_t write_reg(uint16_t reg, uint8_t value) {
    uint8_t command[3] = {reg >> 8, reg & 0xFF, value};
    return bsp_i2c_transfer(OV5647_ADDRESS, command, 3, NULL, 0);
}
static esp_err_t read_word(uint16_t reg, uint16_t *value) {
    uint8_t hi, lo;
    esp_err_t err = read_reg(reg, &hi);
    if (err == ESP_OK) err = read_reg(reg + 1, &lo);
    if (err == ESP_OK) *value = ((uint16_t)hi << 8) | lo;
    return err;
}
esp_err_t ov5647_camera_detect(uint16_t *id) {
    if (!id) return ESP_ERR_INVALID_ARG;
    *id = 0;
    detected = false;
    esp_err_t err = bsp_i2c_probe(OV5647_ADDRESS);
    if (err == ESP_OK) err = read_word(0x300A, id);
    if (err != ESP_OK) return err;
    if (*id != 0x5647) return ESP_ERR_INVALID_RESPONSE;
    detected = true;
    return ESP_OK;
}
esp_err_t ov5647_camera_reset(void) {
    if (!detected) return ESP_ERR_INVALID_STATE;
    detected = false;
    configured = false;
    // 对应本地 Espressif OV5647 reset 表：standby → software reset → 等待 → LP-11。
    esp_err_t err = write_reg(0x0100, 0x00);
    if (err == ESP_OK) err = write_reg(0x0103, 0x01);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    err = write_reg(0x4800, 0x01);
    if (err != ESP_OK) return err;
    uint16_t id;
    return ov5647_camera_detect(&id);
}
esp_err_t ov5647_camera_read_diagnostics(ov5647_diagnostics_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!detected) return ESP_ERR_INVALID_STATE;
    ov5647_diagnostics_t info = {0};
    esp_err_t err = read_word(0x300A, &info.id);
    if (err == ESP_OK && info.id != 0x5647) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = read_reg(0x0100, &info.stream);
    if (err == ESP_OK) err = read_reg(0x3034, &info.format);
    const uint16_t pll_regs[4] = {0x3035, 0x3036, 0x303C, 0x3106};
    for (unsigned i = 0; i < 4 && err == ESP_OK; ++i) err = read_reg(pll_regs[i], &info.pll[i]);
    if (err == ESP_OK) err = read_word(0x3808, &info.width);
    if (err == ESP_OK) err = read_word(0x380A, &info.height);
    if (err == ESP_OK) err = read_word(0x380C, &info.hts);
    if (err == ESP_OK) err = read_word(0x380E, &info.vts);
    if (err == ESP_OK) err = read_reg(0x3820, &info.mirror[0]);
    if (err == ESP_OK) err = read_reg(0x3821, &info.mirror[1]);
    if (err == ESP_OK) err = read_reg(0x4800, &info.mipi_control);
    if (err == ESP_OK) *out = info;
    return err;
}

esp_err_t ov5647_camera_configure(void) {
    uint16_t id;
    esp_err_t err = ov5647_camera_detect(&id);
    if (err == ESP_OK) err = ov5647_camera_reset();
    if (err != ESP_OK) return err;
    for (unsigned i = 0; ov5647_25m_raw8_800x800[i].reg != 0xFFFF; ++i) {
        err = write_reg(ov5647_25m_raw8_800x800[i].reg, ov5647_25m_raw8_800x800[i].val);
        if (err != ESP_OK) return err;
    }
    // 先保持 standby，CSI/ISP 就绪后才输出。无行首/行尾包，连续时钟。
    err = write_reg(0x4800, 0x00);
    if (err == ESP_OK) err = write_reg(0x0100, 0x00);
    ov5647_diagnostics_t verify;
    if (err == ESP_OK) err = ov5647_camera_read_diagnostics(&verify);
    if (err == ESP_OK && (verify.width != 800 || verify.height != 800 ||
                         verify.format != 0x18 || verify.pll[1] != 0x80))
        err = ESP_ERR_INVALID_RESPONSE;
    configured = err == ESP_OK;
    return err;
}

esp_err_t ov5647_camera_stream(bool enable) {
    if (!detected || (enable && !configured)) return ESP_ERR_INVALID_STATE;
    return write_reg(0x0100, enable ? 1 : 0);
}
