#pragma once
// 启动诊断与串口应用层；传感器和 I2C 模块为纯 C，便于迁移 ESP-IDF。
void camera_bringup_begin();
void camera_bringup_poll();
