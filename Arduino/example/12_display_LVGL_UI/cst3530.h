#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CST3530_I2C_ADDRESS 0x58
typedef struct {
    uint32_t chip_type;       // 正常模式返回的类型字段；不是写死的芯片型号。
    uint32_t firmware_version;
    uint32_t project_id;
    uint16_t resolution_x;
    uint16_t resolution_y;
    uint8_t key_count, tx_channels, rx_channels;
    uint8_t raw[50];          // 保留原始数据，便于比对厂家协议。
} cst3530_info_t;

// 调用前初始化 bsp_i2c；接口限单一 TP 任务/启动流程调用，不可并发或在 ISR 调用。
// 完成 RST 低 10ms、高后等待 200ms，再检查 0x58。
// INT 仅配置为输入，第一阶段不启用中断、不读触摸帧。
esp_err_t cst3530_init(int reset_gpio, int interrupt_gpio);
esp_err_t cst3530_read_info(cst3530_info_t *info);

// 厂家样例最多处理 5 条触点/按键记录；不是芯片物理能力的声明。
#define CST3530_MAX_RECORDS 5
typedef struct {
    uint8_t id, pressure;
    bool pressed;
    uint16_t x, y;
} cst3530_point_t;
typedef struct {
    uint8_t count;
    cst3530_point_t points[CST3530_MAX_RECORDS];
} cst3530_frame_t;
typedef enum {
    CST3530_DOWN, CST3530_MOVE, CST3530_UP, CST3530_CANCEL
} cst3530_event_type_t;
typedef void (*cst3530_callback_t)(cst3530_event_type_t event,
                                  const cst3530_point_t *point, void *user);

// 纯解析接口：检查完整长度、0x55 累加校验、记录数、重复 ID 和坐标范围。
esp_err_t cst3530_parse_frame(const uint8_t *raw, size_t size, uint16_t width,
                             uint16_t height, cst3530_frame_t *frame);
// 启用第二阶段。回调在调用 poll 的任务执行，不在 ISR 执行；不可重入驱动。
esp_err_t cst3530_start(const cst3530_info_t *info, cst3530_callback_t callback, void *user);
// 主循环频繁调用；消费下降沿通知并检查 INT 低电平，空闲不进行 I2C。
// ISR 仅通知，实际读帧/回调仍在调用者任务；无需新增一个触摸任务。
// 无就绪报告返回 ESP_OK，不生成 UP、不计失败；真实错误退避 20ms。
// 连续 5 次实际读取失败产生 CANCEL；INT 高电平绝不直接代表松手。
esp_err_t cst3530_poll(void);

// 最近一次采样的两次尝试。只保留确认成功读取的字节，不把失败缓冲当作有效数据。
typedef struct {
    uint8_t raw[4 + CST3530_MAX_RECORDS * 5];
    uint8_t received_size;
    int int_before, int_after_read, int_after_ack; // 电平采样，不代表捕获了所有边沿。
    uint16_t checksum_received, checksum_calculated;
    bool checksum_available, ack_attempted;
    esp_err_t result, ack_result;
} cst3530_attempt_diag_t;
typedef struct {
    uint32_t sequence;
    uint8_t attempts;
    cst3530_attempt_diag_t attempt[2];
} cst3530_diagnostics_t;
// 与 poll 同任务调用；纯复制快照，不触发额外 I2C、不清除报告。
esp_err_t cst3530_get_diagnostics(cst3530_diagnostics_t *out);
#ifdef __cplusplus
}
#endif
