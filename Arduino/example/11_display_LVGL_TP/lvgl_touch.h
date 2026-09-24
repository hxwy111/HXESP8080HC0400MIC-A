#pragma once
#include "cst3530.h"
#ifdef __cplusplus
extern "C" {
#endif
// 在显示/LVGL 启动后调用；首次仅接受与屏幕相同的坐标范围，暂不旋转/缩放。
esp_err_t lvgl_touch_init(uint16_t width, uint16_t height);
// 主任务调用，内部使用 LVGL 互斥锁；必须在串口限频判断之前传入每个事件。
void lvgl_touch_submit(cst3530_event_type_t event, const cst3530_point_t *point);
// 获取 LVGL 实际消费的指针状态；内部加锁，不读取硬件。
bool lvgl_touch_get_state(uint16_t *x, uint16_t *y, bool *pressed);
#ifdef __cplusplus
}
#endif
