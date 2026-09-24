#pragma once
// 仅在持有 LVGL 锁时调用；界面独立于 I2C 和 CST3530 协议层。
// 创建开机页、主页、显示测试、触摸测试和演示控件；所有 UI 实现在同名 cpp 中。
bool tp_test_ui_create();
// 只清零触摸测试统计，不改变当前页面或控件页设置。
void tp_test_ui_reset();
