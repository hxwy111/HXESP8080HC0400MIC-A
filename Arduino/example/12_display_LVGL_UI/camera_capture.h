#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t frames, wrong_size, last_bytes; bool running; } camera_capture_stats_t;
// 前置：I2C 和 PHY LDO3 2.5V 已由板级初始化并保持；单任务串行调用。
// 正常 stop 销毁 CSI/ISP 并释放采集帧，再次 start 全新构建；失败请复位。
// stop 前必须归还借出的帧。共享 I2C/PHY 电源由板级保持，不在此释放。
esp_err_t camera_capture_start(void);
esp_err_t camera_capture_stop(void);
void camera_capture_stats(camera_capture_stats_t *out);
const char *camera_capture_stage(void);
// 同控制任务调用，打印采集计数及缓冲状态，不在 ISR 调用。
void camera_capture_log(void);
// 主任务取得最新完成帧后必须 release；借出期间 DMA 不覆盖该缓冲。
esp_err_t camera_capture_acquire(const void **buffer, size_t *size);
void camera_capture_release(const void *buffer);
#ifdef __cplusplus
}
#endif
