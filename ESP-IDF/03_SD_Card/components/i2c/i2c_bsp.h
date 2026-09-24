#ifndef I2C_BSP_H
#define I2C_BSP_H
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif
/* 初始化板级 I2C 主机。 */
void I2C_master_Init(void);
/* 向指定从设备寄存器连续写入 len 字节。 */
uint8_t I2C_writr_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len);
/* 从指定从设备寄存器连续读取 len 字节。 */
uint8_t I2C_read_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len);
/* 通用写后读接口，适用于需要自定义命令头的 I2C 设备。 */
uint8_t I2C_master_write_read_device(uint8_t addr,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen);
/**
 * @brief 扫描并打印 I2C 总线上的设备地址
 */
void I2C_scan_devices(void);
#ifdef __cplusplus
}
#endif
#endif
