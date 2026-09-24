#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
// 单任务启动阶段调用，CSI/DSI 共用一次 LDO3 2.5V 申请。
esp_err_t mipi_power_init(void);
#ifdef __cplusplus
}
#endif
