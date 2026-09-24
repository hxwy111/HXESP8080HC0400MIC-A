#pragma once
#include <lvgl.h>
// LVGL 适配层：bind/request 在 LVGL 锁内调用，poll 仅在主循环任务调用。
void camera_preview_bind(lv_obj_t *image, lv_obj_t *status, lv_obj_t *button_text);
void camera_preview_request(bool enable);
bool camera_preview_requested();
bool camera_preview_busy();
void camera_preview_poll();
