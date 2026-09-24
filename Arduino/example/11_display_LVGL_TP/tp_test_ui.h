#pragma once
// 仅在持有 LVGL 锁时调用；界面独立于 I2C 和 CST3530 协议层。
bool tp_test_ui_create();
void tp_test_ui_reset();
