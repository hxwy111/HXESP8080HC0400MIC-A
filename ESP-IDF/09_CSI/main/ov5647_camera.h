#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OV5647_ADDRESS 0x36
#define OV5647_FRAME_WIDTH 800
#define OV5647_FRAME_HEIGHT 800
#define OV5647_LANE_MBPS 400
// 工作假设：模组板载 25MHz；参考表名义约 50fps，真实帧率以统计为准。
esp_err_t ov5647_camera_configure(void);
esp_err_t ov5647_camera_stream(bool enable);
typedef struct {
    uint16_t id, width, height, hts, vts;
    uint8_t stream, pll[4], mipi_control, format, mirror[2];
} ov5647_diagnostics_t;
// 前置条件：共享 I2C 已初始化，模组由 Pin15 供电。
// 不操作未确认映射的 CAM-GPIO、LED-ON，不产生外部 XCLK。
esp_err_t ov5647_camera_detect(uint16_t *id);
esp_err_t ov5647_camera_read_diagnostics(ov5647_diagnostics_t *out);
// 仅检测 ID 匹配后允许软件复位；复位会丢失当前运行配置，不会启动采集。
esp_err_t ov5647_camera_reset(void);
#ifdef __cplusplus
}
#endif
