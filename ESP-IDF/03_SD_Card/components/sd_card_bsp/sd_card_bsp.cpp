#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sd_card_bsp.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/*
 * ESP32-P4 SDMMC Slot 0 wiring from the schematic:
 *   CLK=GPIO43, CMD=GPIO44, DAT0=GPIO39
 * DAT1=GPIO40, DAT2=GPIO41 and DAT3=GPIO42 are also connected, but this
 * example intentionally uses the more tolerant 1-bit mode.
 */
#define SDMMC_U
#define PIN_NUM_D0    (gpio_num_t)39
#define PIN_NUM_CMD   (gpio_num_t)44
#define PIN_NUM_CLK   (gpio_num_t)43
#define PIN_NUM_POWER (gpio_num_t)45
#define SDMMC_LDO_CHANNEL 4
#define SDMMC_POWER_ON_LEVEL 0
/* Legacy names retained only for the disabled SDSPI fallback below. */
#define PIN_NUM_MISO  PIN_NUM_D0
#define PIN_NUM_MOSI  PIN_NUM_CMD
#define SDlist "/sd_card" // SD 卡在虚拟文件系统中的挂载目录
#ifndef SDMMC_U
#define PIN_NUM_CS    
#define SD_SPI SPI3_HOST
#endif


sdmmc_card_t *card = NULL; // SD 卡设备句柄
static sd_pwr_ctrl_handle_t sd_power_handle = NULL;


esp_err_t SD_card_Init(void)
{
#ifdef SDMMC_U
  /* GPIO45 drives the P-channel MOSFET supplying SD1_VDD: low means on. */
  gpio_config_t power_gpio_config = {};
  power_gpio_config.pin_bit_mask = 1ULL << PIN_NUM_POWER;
  power_gpio_config.mode = GPIO_MODE_OUTPUT;
  power_gpio_config.pull_up_en = GPIO_PULLUP_DISABLE;
  power_gpio_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  power_gpio_config.intr_type = GPIO_INTR_DISABLE;

  esp_err_t ret = gpio_config(&power_gpio_config);
  if (ret != ESP_OK) {
    printf("SD power GPIO config failed: %s (0x%x)\n",
           esp_err_to_name(ret), ret);
    return ret;
  }

  /* Power-cycle the socket before starting card identification. */
  gpio_set_level(PIN_NUM_POWER, !SDMMC_POWER_ON_LEVEL);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(PIN_NUM_POWER, SDMMC_POWER_ON_LEVEL);
  vTaskDelay(pdMS_TO_TICKS(200));

  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,     // 挂载失败时不自动创建分区表和格式化 SD 卡
    .max_files = 5,                      // 允许同时打开的最大文件数
    .allocation_unit_size = 512,         // 文件系统分配单元大小，作用类似扇区大小
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.flags = SDMMC_HOST_FLAG_1BIT;
  //host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; // 使用高速模式

  /*
   * Slot 0 uses the ESP32-P4 fixed IO_MUX pins listed above. For this IDF
   * driver, GPIO_NUM_0 means "use the fixed IO_MUX pin"; SD signals are not
   * routed to physical GPIO0.
   */
  sdmmc_slot_config_t slot_config = {};
  slot_config.clk = GPIO_NUM_0;
  slot_config.cmd = GPIO_NUM_0;
  slot_config.d0 = GPIO_NUM_0;
  slot_config.d1 = GPIO_NUM_0;
  slot_config.d2 = GPIO_NUM_0;
  slot_config.d3 = GPIO_NUM_0;
  slot_config.d4 = GPIO_NUM_0;
  slot_config.d5 = GPIO_NUM_0;
  slot_config.d6 = GPIO_NUM_0;
  slot_config.d7 = GPIO_NUM_0;
  slot_config.cd = SDMMC_SLOT_NO_CD;
  slot_config.wp = SDMMC_SLOT_NO_WP;
  slot_config.width = 1;
  slot_config.flags = 0;

  if (sd_power_handle == NULL) {
    sd_pwr_ctrl_ldo_config_t ldo_config = {
      .ldo_chan_id = SDMMC_LDO_CHANNEL,
    };
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &sd_power_handle);
    if (ret != ESP_OK) {
      printf("SDMMC LDO channel %d init failed: %s (0x%x)\n",
             SDMMC_LDO_CHANNEL, esp_err_to_name(ret), ret);
      return ret;
    }
  }
  host.pwr_ctrl_handle = sd_power_handle;

  esp_err_t mount_ret = esp_vfs_fat_sdmmc_mount(
      SDlist, &host, &slot_config, &mount_config, &card);
  if (mount_ret != ESP_OK) {
    printf("SDMMC mount failed: %s (0x%x)\n",
           esp_err_to_name(mount_ret), mount_ret);
    card = NULL;
    return mount_ret;
  }

  sdmmc_card_print_info(stdout, card); // 打印 SD 卡信息
  printf("practical_size:%.2fG\n",(float)(card->csd.capacity)/2048/1024);// 容量单位：GB
  return ESP_OK;
#endif

#ifndef SDMMC_U
  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,    // 挂载失败时创建分区表并格式化 SD 卡
    .max_files = 5,                    // 允许同时打开的最大文件数
    .allocation_unit_size = 512        // 文件系统分配单元大小，作用类似扇区大小
  };                                                                                                      
  spi_bus_config_t bus_cfg = 
  {
    .mosi_io_num = PIN_NUM_MOSI,
    .miso_io_num = PIN_NUM_MISO,
    .sclk_io_num = PIN_NUM_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4000,   // SPI 单次传输的最大字节数
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(SD_SPI, &bus_cfg, SDSPI_DEFAULT_DMA));
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = SD_SPI;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_SPI;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdspi_mount(SDlist, &host, &slot_config, &mount_config, &card)); 
  if(card != NULL)
  {
    sdmmc_card_print_info(stdout, card); // 打印 SD 卡信息
    printf("practical_size:%.2fG\n",(float)(card->csd.capacity)/2048/1024);// 容量单位：GB
    return ESP_OK;
  }
  return ESP_FAIL;
#endif
}
float sd_cadr_get_value(void)
{
  if(card != NULL)
  {
    return (float)(card->csd.capacity)/2048/1024; // 返回容量，单位：GB
  }
  else
  return 0;
}

/* 写入数据
path：文件路径
data：待写入的数据
*/
esp_err_t s_example_write_file(const char *path, const char *data)
{
  if (path == NULL || data == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if(card == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  esp_err_t err = sdmmc_get_status(card); // 首先检查 SD 卡是否存在且状态正常
  if(err != ESP_OK)
  {
    return err;
  }
  FILE *f = fopen(path, "w"); // 以写入方式打开指定路径的文件
  if(f == NULL)
  {
    printf("path:Write Wrong path\n");
    return ESP_ERR_NOT_FOUND;
  }

  size_t data_length = strlen(data);
  size_t written = fwrite(data, 1, data_length, f);
  if (written != data_length) {
    printf("Write incomplete: %u/%u bytes\n",
           (unsigned int)written, (unsigned int)data_length);
    fclose(f);
    return ESP_FAIL;
  }

  if (fclose(f) != 0) {
    printf("Close file after writing failed\n");
    return ESP_FAIL;
  }
  return ESP_OK;
}
/*
读取数据
path：文件路径
*/
esp_err_t s_example_read_file(const char *path,
                              char *buffer,
                              size_t buffer_size,
                              size_t *out_len)
{
  if (path == NULL || buffer == NULL || buffer_size == 0 || out_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  buffer[0] = '\0';
  *out_len = 0;

  if(card == NULL)
  {
    printf("path:card == NULL\n");
    return ESP_ERR_NOT_FOUND;
  }
  esp_err_t err = sdmmc_get_status(card); // 首先检查 SD 卡是否存在且状态正常
  if(err != ESP_OK)
  {
    printf("path:card == NO\n");
    return err;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    printf("path:Read Wrong path\n");
    return ESP_ERR_NOT_FOUND;
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return ESP_FAIL;
  }
  long file_size = ftell(f);
  if (file_size < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return ESP_FAIL;
  }

  size_t bytes_to_read = (size_t)file_size;
  if (bytes_to_read >= buffer_size) {
    bytes_to_read = buffer_size - 1;
    printf("Read buffer is smaller than file; output will be truncated\n");
  }

  size_t bytes_read = fread(buffer, 1, bytes_to_read, f);
  if (ferror(f)) {
    fclose(f);
    return ESP_FAIL;
  }
  buffer[bytes_read] = '\0';
  *out_len = bytes_read;

  printf("File size: %u bytes, read: %u bytes\n",
         (unsigned int)file_size, (unsigned int)bytes_read);
  fclose(f);
  return ESP_OK;
}
