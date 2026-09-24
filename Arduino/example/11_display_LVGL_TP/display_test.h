#pragma once
// 显示启动独立于纯 C 的 I2C/TP 模块，界面位于 tp_test_ui.cpp。
// begin 仅启动时调用一次；poll 处理串口 r 清零、d/e 背光控制。
void display_test_begin();
void display_test_poll();
bool display_test_is_ready();
