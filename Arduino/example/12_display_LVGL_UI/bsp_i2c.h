#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
// 单例共享总线。仅在启动阶段初始化一次；相同配置重复调用允许，不接管外部驱动。
esp_err_t bsp_i2c_init(int port, int sda, int scl, uint32_t frequency_hz);
esp_err_t bsp_i2c_probe(uint8_t address);
// 同一事务锁内执行写/读；两段之间带 STOP，与厂家 HYN 示例一致。
// 写或读可单独使用，但不可同时为空。地址为 7 位，接口仅供任务调用。
esp_err_t bsp_i2c_transfer(uint8_t address, const uint8_t *tx, size_t tx_size,
                           uint8_t *rx, size_t rx_size);
#ifdef __cplusplus
}
#endif
