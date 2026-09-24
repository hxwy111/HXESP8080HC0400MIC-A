/*
 * OV5647 传感器控制层：通过共享 I2C/SCCB 读写寄存器。
 * 这里只控制传感器，不创建 CSI/ISP，不分配图像缓冲，也不调用 LVGL。
 * 图像像素通过 MIPI CSI 输出，不经过这里的 I2C 接口。
 *
 * 典型顺序：板级供电/I2C 就绪 -> configure -> 主控接收就绪
 *          -> stream(true) -> stream(false)。
 * 静态状态仅对应一个传感器，须由同一控制任务串行调用；
 * bsp_i2c 的事务锁不保证整个多步初始化序列的并发安全。
 */
#include "ov5647_camera.h"
#include "bsp_i2c.h"
#include "ov5647_mode.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool detected;   // 最近一次探测是否读到正确 ID；不代表当前正在输出图像。
static bool configured; // 当前模式是否完成配置及关键字段回读；复位会清除此标志。
// 读取一个 8 位寄存器；16 位寄存器地址按高字节、低字节顺序发送。
static esp_err_t read_reg(uint16_t reg, uint8_t *value) {
    uint8_t command[2] = {reg >> 8, reg & 0xFF};
    // SCCB 按厂商驱动习惯分离写地址和读数据，错误原样返回。
    return bsp_i2c_transfer(OV5647_ADDRESS, command, 2, value, 1);
}
// 写入格式：寄存器地址高字节 + 地址低字节 + 8 位数据。
static esp_err_t write_reg(uint16_t reg, uint8_t value) {
    uint8_t command[3] = {reg >> 8, reg & 0xFF, value};
    return bsp_i2c_transfer(OV5647_ADDRESS, command, 3, NULL, 0);
}
// 读取相邻的高/低字节寄存器并拼成 16 位数；不是一次原子快照读取。
// 仅两次读取都成功时更新输出，避免使用未初始化的 hi/lo。
static esp_err_t read_word(uint16_t reg, uint16_t *value) {
    uint8_t hi, lo;
    esp_err_t err = read_reg(reg, &hi);
    if (err == ESP_OK) err = read_reg(reg + 1, &lo);
    if (err == ESP_OK) *value = ((uint16_t)hi << 8) | lo;
    return err;
}
// 先探测 7 位地址 0x36，再读取 0x300A/0x300B 的芯片 ID。
// 地址 ACK 不能证明型号正确，必须与 0x5647 比较。
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
// 软件复位不是 GPIO 硬件复位；只在识别成功后允许写复位寄存器。
esp_err_t ov5647_camera_reset(void) {
    if (!detected) return ESP_ERR_INVALID_STATE;
    detected = false;
    configured = false;
    // 对应本地 Espressif OV5647 reset 表：standby → software reset → 等待 → LP-11。
    esp_err_t err = write_reg(0x0100, 0x00); // 关闭传感器图像流，进入待机。
    if (err == ESP_OK) err = write_reg(0x0103, 0x01); // 触发软件复位。
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10) + 1); // 多留一个 tick，避免实际等待短于 10ms。
    err = write_reg(0x4800, 0x01);
    if (err != ESP_OK) return err;
    uint16_t id;
    return ov5647_camera_detect(&id); // 复位后重新确认寄存器接口可用。
}
// 只读运行寄存器，不修改模式；全部读取成功才提交 info，失败时 out 保持清零。
// 这些值用于核对配置，不能测量物理晶振，也不能证明 CSI 已收到有效图像。
esp_err_t ov5647_camera_read_diagnostics(ov5647_diagnostics_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!detected) return ESP_ERR_INVALID_STATE;
    ov5647_diagnostics_t info = {0};
    esp_err_t err = read_word(0x300A, &info.id);
    if (err == ESP_OK && info.id != 0x5647) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = read_reg(0x0100, &info.stream);
    if (err == ESP_OK) err = read_reg(0x3034, &info.format);
    const uint16_t pll_regs[4] = {0x3035, 0x3036, 0x303C, 0x3106}; // 保持诊断数组顺序。
    for (unsigned i = 0; i < 4 && err == ESP_OK; ++i) err = read_reg(pll_regs[i], &info.pll[i]);
    // 输出宽高与行/帧总时序分别读取；HTS/VTS 不等于可见图像宽高。
    if (err == ESP_OK) err = read_word(0x3808, &info.width);
    if (err == ESP_OK) err = read_word(0x380A, &info.height);
    if (err == ESP_OK) err = read_word(0x380C, &info.hts);
    if (err == ESP_OK) err = read_word(0x380E, &info.vts);
    // 保存完整时序/翻转寄存器原值，不能把整个字节直接当作镜像布尔值。
    if (err == ESP_OK) err = read_reg(0x3820, &info.mirror[0]);
    if (err == ESP_OK) err = read_reg(0x3821, &info.mirror[1]);
    if (err == ESP_OK) err = read_reg(0x4800, &info.mipi_control);
    if (err == ESP_OK) *out = info;
    return err;
}

// 完整模式初始化：识别 -> 复位 -> 按顺序写模式表 -> 待机 -> 回读关键参数。
// 任一步通信失败立即返回，不继续写后续寄存器；调用者负责故障清理。
esp_err_t ov5647_camera_configure(void) {
    uint16_t id;
    esp_err_t err = ov5647_camera_detect(&id);
    if (err == ESP_OK) err = ov5647_camera_reset();
    if (err != ESP_OK) return err;
    // 0xFFFF 为表结束标记，不发送给芯片；表的配置顺序不可随意调整。
    for (unsigned i = 0; ov5647_25m_raw8_800x800[i].reg != 0xFFFF; ++i) {
        err = write_reg(ov5647_25m_raw8_800x800[i].reg, ov5647_25m_raw8_800x800[i].val);
        if (err != ESP_OK) return err;
    }
    // 先保持 standby，CSI/ISP 就绪后才输出。无行首/行尾包，连续时钟。
    err = write_reg(0x4800, 0x00);
    if (err == ESP_OK) err = write_reg(0x0100, 0x00);
    ov5647_diagnostics_t verify;
    if (err == ESP_OK) err = ov5647_camera_read_diagnostics(&verify);
    // 抽查宽高、RAW8格式相关寄存器及PLL倍频值，并非逐项校验整个模式表。
    if (err == ESP_OK && (verify.width != 800 || verify.height != 800 ||
                         verify.format != 0x18 || verify.pll[1] != 0x80))
        err = ESP_ERR_INVALID_RESPONSE;
    configured = err == ESP_OK;
    return err;
}

// 仅控制传感器 stream 位；不会启动/停止主控 CSI、ISP 或切断模组电源。
// 开流必须先配置成功；关流只要求已识别，方便在配置失败后尝试进入待机。
esp_err_t ov5647_camera_stream(bool enable) {
    if (!detected || (enable && !configured)) return ESP_ERR_INVALID_STATE;
    return write_reg(0x0100, enable ? 1 : 0);
}
