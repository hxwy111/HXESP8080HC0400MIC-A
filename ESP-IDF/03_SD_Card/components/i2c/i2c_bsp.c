#include <stdio.h>
#include "i2c_bsp.h"

#define TEST_I2C_PORT I2C_NUM_0
//#define EXAMPLE_USE_TOUCH // 触摸屏模块接管共享 I2C 总线时启用

#ifndef EXAMPLE_USE_TOUCH

#define I2C_MASTER_SCL_IO 8
#define I2C_MASTER_SDA_IO 7

/* 初始化 I2C0：SDA 使用 GPIO41，SCL 使用 GPIO40，总线速率为 300 kHz。 */
void I2C_master_Init(void)
{
  i2c_config_t conf =
  {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = I2C_MASTER_SDA_IO,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_io_num = I2C_MASTER_SCL_IO,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100 * 1000,
    .clk_flags = 0, // 使用默认 I2C 时钟源
  };
  ESP_ERROR_CHECK(i2c_param_config(TEST_I2C_PORT, &conf));
  /* 主机模式不需要配置接收和发送缓冲区。 */
  ESP_ERROR_CHECK(i2c_driver_install(TEST_I2C_PORT, conf.mode,0,0,0));
}
#endif

/* 发送格式：[寄存器地址][数据...]。 */
uint8_t I2C_writr_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len)
{
  uint8_t ret;
  uint8_t *pbuf = (uint8_t*)malloc(len+1);
  pbuf[0] = reg;
  for(uint8_t i = 0; i<len; i++)
  {
    pbuf[i+1] = buf[i];
  }
  ret = i2c_master_write_to_device(TEST_I2C_PORT,addr,pbuf,len+1,1000);
  free(pbuf);
  pbuf = NULL;
  return ret;
}

/* 先写入寄存器地址，再通过重复起始条件连续读取数据。 */
uint8_t I2C_read_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len)
{
  uint8_t ret;
  ret = i2c_master_write_read_device(TEST_I2C_PORT,addr,&reg,1,buf,len,1000);
  return ret;
}

/* 通用 I2C“先写后读”接口，适用于带自定义命令头的设备。 */
uint8_t I2C_master_write_read_device(uint8_t addr,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen)
{
  esp_err_t ret;
  ret = i2c_master_write_read_device(TEST_I2C_PORT,addr,writeBuf,writeLen,readBuf,readLen,1000);
  return ret;
}

/**
 * @brief 扫描 I2C 总线上的设备地址
 *
 * 扫描范围为 0x08～0x77，跳过 I2C 规范中的保留地址。
 * 当从设备返回 ACK 时，打印对应的 7 位 I2C 地址。
 */
void I2C_scan_devices(void)
{
  uint8_t device_count = 0;

  printf("\nStart scanning I2C devices...\n");

  /* 跳过 0x00～0x07 和 0x78～0x7F 两段保留地址。 */
  for(uint8_t addr = 0x08; addr <= 0x77; addr++)
  {
    /* 创建一次 I2C 命令事务。 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    /* 发送起始信号。 */
    i2c_master_start(cmd);

    /*
     * 发送 7 位从机地址和写方向位。
     * enable_ack_check=true：检查从设备是否返回 ACK。
     */
    i2c_master_write_byte(cmd,
                          (addr << 1) | I2C_MASTER_WRITE,
                          true);

    /* 发送停止信号。 */
    i2c_master_stop(cmd);

    /* 执行本次探测，超时时间为 20 ms。 */
    esp_err_t ret = i2c_master_cmd_begin(TEST_I2C_PORT,
                                         cmd,
                                         pdMS_TO_TICKS(20));

    /* 使用完成后释放命令资源。 */
    i2c_cmd_link_delete(cmd);

    if(ret == ESP_OK)
    {
      printf("I2C device detected, address:0x%02X\n", addr);
      device_count++;
    }
  }

  if(device_count == 0)
  {
    printf("No I2C devices found. Please check the wiring, power supply and pull-up resistors.\n");
  }
  else
  {
    printf("I2C scan completed. %u device(s) found in total.\n\n",
           (unsigned int)device_count);
  }
}
