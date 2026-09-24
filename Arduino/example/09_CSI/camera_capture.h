#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t frames, wrong_size, last_bytes; bool running; } camera_capture_stats_t;
// 单次启动；失败保留资源以便诊断，不重复启动，请复位后重试。
esp_err_t camera_capture_start(void);
esp_err_t camera_capture_stop(void);
void camera_capture_stats(camera_capture_stats_t *out);
const char *camera_capture_stage(void);
// 主任务取得最新完成帧后必须 release；借出期间 DMA 不覆盖该缓冲。
esp_err_t camera_capture_acquire(const void **buffer, size_t *size);
void camera_capture_release(const void *buffer);
#ifdef __cplusplus
}
#endif
