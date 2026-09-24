#pragma once
/*
 * OV5647 传感器控制接口，与 Arduino/LVGL 无关，方便复用于 ESP-IDF。
 * 前置：模组供电和共享 bsp_i2c 总线已初始化；所有接口由同一任务串行调用，
 * 不可在中断中调用。成功返回 ESP_OK，失败返回通信或参数/状态错误码。
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
// 允许 C++ 主程序调用由 C 文件实现的函数，避免链接时符号名称不一致。
extern "C" {
#endif
#define OV5647_ADDRESS 0x36       // 7 位 I2C/SCCB 地址，不含读写位。
#define OV5647_FRAME_WIDTH 800    // 当前模式输出宽度，单位：像素。
#define OV5647_FRAME_HEIGHT 800   // 当前模式输出高度，单位：像素。
#define OV5647_LANE_MBPS 400      // 当前模式每条 MIPI 数据 Lane 的配置速率，单位：Mbps。
// 工作假设：模组板载 25MHz；参考表名义约 50fps，真实帧率以统计为准。
// 识别、复位并加载 RAW8 800×800 模式，完成后保持待机。
// 主控 CSI/ISP 接收端就绪后，再调用 stream(true)，此函数本身不创建接收端。
esp_err_t ov5647_camera_configure(void);
// enable=true 开始输出，false 进入待机；不控制电源或释放主控资源。
// 未识别时返回 ESP_ERR_INVALID_STATE；开启还要求已成功配置模式。
esp_err_t ov5647_camera_stream(bool enable);
// 寄存器诊断快照：字段保存原始值，不代表实测帧率或物理时钟频率。
typedef struct {
    // id: 0x300A/B；width/height: 0x3808～0x380B；
    // hts/vts: 0x380C～0x380F，分别为行总时序和帧总时序配置值。
    uint16_t id, width, height, hts, vts;
    // stream: 0x0100；pll[0..3]: 0x3035/0x3036/0x303C/0x3106；
    // mipi_control: 0x4800；format: 0x3034；mirror[0..1]: 0x3820/0x3821。
    uint8_t stream, pll[4], mipi_control, format, mirror[2];
} ov5647_diagnostics_t;
// 前置条件：共享 I2C 已初始化，模组由 Pin15 供电。
// 不操作未确认映射的 CAM-GPIO、LED-ON，不产生外部 XCLK。
// id 必须非空，入口清零；读到非 0x5647 返回 ESP_ERR_INVALID_RESPONSE。
esp_err_t ov5647_camera_detect(uint16_t *id);
// out 必须非空且芯片已识别；失败时输出结构清零，成功时提交完整快照。
esp_err_t ov5647_camera_read_diagnostics(ov5647_diagnostics_t *out);
// 仅检测 ID 匹配后允许软件复位；复位会丢失当前运行配置，不会启动采集。
esp_err_t ov5647_camera_reset(void);
#ifdef __cplusplus
}
#endif
