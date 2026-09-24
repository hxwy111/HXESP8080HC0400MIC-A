// 主程序只编排调用；OV5647 → CSI → ISP RGB565 → JD9365 实时预览。
#include "camera_bringup.h"

void setup() { camera_bringup_begin(); }
void loop() { camera_bringup_poll(); }
