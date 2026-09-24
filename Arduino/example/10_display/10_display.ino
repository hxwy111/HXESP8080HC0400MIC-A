// Arduino 基础功能，以及 ESP-IDF 的 GPIO、显示、电源和缓存接口。
#include <Arduino.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_idf_version.h"
#include "soc/dw_gdma_struct.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "jd9365_init.h" // 4.0 寸厂家初始化表，按两条 DSI 通道适配，待实屏验证

// 仅支持具有 MIPI-DSI 外设的 ESP32-P4 目标。
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#error "Select an ESP32-P4 board."
#endif

// 新硬件：R43 已取消，C39 改为 10kΩ 下拉电阻；BL_EN 无上拉，必须由软件明确控制。
static constexpr gpio_num_t BL_EN = GPIO_NUM_33; // AP3032 CTRL/EN，高电平开启背光
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26; // LCD_BL_PWM 调光输入，低电平为本板全亮
static constexpr gpio_num_t LCD_RESET = GPIO_NUM_27; // 屏幕复位，低电平有效
// 暂假设核心板 LDO_VO3 为 VDD_MIPI_DPHY 供电。
// 仍需核对核心板内部原理图，底板原理图未展示这段连接。
static constexpr int PHY_LDO_CHANNEL = 3; // LDO 通道编号，不是 GPIO 编号
static constexpr int PHY_MV = 2500; // PHY 电源设定值，单位 mV

// 全局保存资源句柄，防止后续误释放；显示运行期间必须持续保持供电。
static esp_ldo_channel_handle_t phy_power = nullptr; // PHY 电源句柄
static esp_lcd_dsi_bus_handle_t dsi_bus = nullptr; // DSI 总线句柄
static esp_lcd_panel_io_handle_t command_io = nullptr; // 面板寄存器命令通道
static esp_lcd_panel_handle_t video_panel = nullptr; // 持续视频输出面板
static uint16_t *frame_buffer = nullptr; // RGB565 帧缓冲，每个像素占 2 字节
static constexpr int WIDTH = 720; // 水平方向像素数
static constexpr int HEIGHT = 720; // 垂直方向像素数
static bool display_ready = false; // 完成初始化后才允许切换画面
static bool auto_cycle = true; // 上电自动轮播；手动选图暂停，a 恢复轮播
static bool hardware_pattern = false; // DSI 主机硬件图案绕过 PSRAM 像素数据
static uint8_t pattern = 0; // 当前画面编号：0～6
static uint32_t last_pattern_ms = 0; // 上次切换画面的毫秒时间
static uint32_t refresh_count = 0; // ISR 与主任务通过原子操作访问
static uint32_t last_report_ms = 0;
static uint32_t last_report_count = 0;
// 临时降速对照：24 MHz / 800 / 760 ≈ 39.47 Hz，不是厂家推荐的最终刷新率。
// 若降速恢复正常，再恢复 36.48f 验证；本次不改 lane 速率、格式或厂家寄存器。
static constexpr float PIXEL_CLOCK_MHZ = 24.0f;
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr), "ISR counter requires lock-free atomics");

static bool IRAM_ATTR on_video_refresh(esp_lcd_panel_handle_t,
                                     esp_lcd_dpi_panel_event_data_t *, void *) {
  __atomic_fetch_add(&refresh_count, 1U, __ATOMIC_RELAXED);
  return false;
}

// 仅读取硬件状态，不接管驱动、不清除中断；寄存器快照可能随 DMA 运行变化。
static void report_dma_state() {
  Serial.printf("BRIDGE: enabled=%08lX dpi=%08lX fifo=%lu raw_irq=%08lX flow=%08lX\n",
      (unsigned long)MIPI_DSI_BRIDGE.en.val,
      (unsigned long)MIPI_DSI_BRIDGE.dpi_misc_config.val,
      (unsigned long)MIPI_DSI_BRIDGE.fifo_flow_status.raw_buf_depth,
      (unsigned long)MIPI_DSI_BRIDGE.int_raw.val,
      (unsigned long)MIPI_DSI_BRIDGE.dma_flow_ctrl.val);
  Serial.printf("DMA: cfg=%08lX enabled=%08lX common_irq=%08lX fb=%p bytes=%lu\n",
      (unsigned long)DW_GDMA.cfg0.val, (unsigned long)DW_GDMA.chen0.val,
      (unsigned long)DW_GDMA.common_int_st0.val, (void *)frame_buffer,
      (unsigned long)(WIDTH * HEIGHT * sizeof(uint16_t)));
  // 打印全部通道，避免假定显示驱动一定分配到 channel 0。
  for (unsigned i = 0; i < sizeof(DW_GDMA.ch) / sizeof(DW_GDMA.ch[0]); ++i) {
    const volatile auto &ch = DW_GDMA.ch[i];
    Serial.printf("DMA%u: src=%08lX dst=%08lX block=%lu status=%08lX/%08lX irq=%08lX/%08lX llp=%08lX\n",
        i, (unsigned long)ch.sar0.val, (unsigned long)ch.dar0.val,
        (unsigned long)ch.block_ts0.val, (unsigned long)ch.status0.val,
        (unsigned long)ch.status1.val, (unsigned long)ch.int_st0.val,
        (unsigned long)ch.int_st1.val, (unsigned long)ch.llp0.val);
  }
}

static void report_video_refresh() {
  const uint32_t now = millis();
  if (now - last_report_ms < 2000) return;
  const uint32_t count = __atomic_load_n(&refresh_count, __ATOMIC_RELAXED);
  Serial.printf("DIAG: source=%s, refresh_events=%lu, delta=%lu in %lu ms, fb0=0x%04X\n",
      hardware_pattern ? "HOST_PATTERN" : "FRAME_BUFFER",
      (unsigned long)count, (unsigned long)(count - last_report_count),
      (unsigned long)(now - last_report_ms), (unsigned)frame_buffer[0]);
  // 每次烧录最多自动打印两份停滞快照；s 可随时再读，避免串口刷屏。
  static unsigned stalled_reports = 0;
  if (!hardware_pattern && count == last_report_count && stalled_reports < 2) {
    report_dma_state();
    ++stalled_reports;
  }
  last_report_ms = now;
  last_report_count = count;
}

// 新硬件背光初始化：GPIO33 先配置为低电平关闭，GPIO26 先保持高电平关闭调光。
// 外部 10kΩ 下拉保证复位/高阻期间 BL_EN 默认关闭，避免上电误亮。
static esp_err_t backlight_init() {
  esp_err_t err = gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT);
  if (err == ESP_OK) err = gpio_set_level(BL_EN, 0);
  if (err == ESP_OK) err = gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT);
  if (err == ESP_OK) err = gpio_set_level(BL_PWM, 1);
  return err;
}

// 控制背光：先设定调光脚，再切换 EN，减少打开瞬间的不确定状态。
static esp_err_t backlight_set(bool enable) {
  esp_err_t err = gpio_set_level(BL_PWM, enable ? 0 : 1);
  if (err == ESP_OK) err = gpio_set_level(BL_EN, enable ? 1 : 0);
  return err;
}

// 统一处理接口返回值：出错时打印错误、关闭背光并停止画面更新。
// 返回成功仅表示接口操作成功，不代表屏幕已经实际显示。
static bool check_result(const char *step, esp_err_t result) {
  if (result == ESP_OK) return true;
  Serial.printf("ERROR: %s: %s (0x%x)\n", step, esp_err_to_name(result),
                static_cast<unsigned>(result));
  gpio_set_level(BL_EN, 0);
  gpio_set_level(BL_PWM, 1);
  display_ready = false;
  Serial.println("Stopped. Backlight remains OFF. Reset board to retry.");
  return false;
}

// 经 DSI 命令通道发送一条 8 位命令和一个 8 位参数。
static bool write_register(uint8_t command, uint8_t value) {
  const esp_err_t result = esp_lcd_panel_io_tx_param(command_io, command, &value, 1);
  if (result != ESP_OK) Serial.printf("Command 0x%02X failed\n", command);
  return check_result("panel register", result);
}

// 仅在 DPI 初始化后调用；硬件图案用于定位视频链路，不是面板内部自检。
static bool set_video_pattern(mipi_dsi_pattern_type_t mode) {
  if (!check_result("DSI host pattern", esp_lcd_dpi_panel_set_pattern(video_panel, mode))) return false;
  hardware_pattern = mode != MIPI_DSI_PATTERN_NONE;
  return true;
}

// 根据编号绘制完整画面：0～4 纯色，5 彩条（暂不绘制边框），6 棋盘格中心十字。
// 调用前必须已经取得帧缓冲，index 由调用方保证在 0～6 范围内。
static bool show_pattern(uint8_t index) {
  // RGB565：红色占高 5 位，绿色占中间 6 位，蓝色占低 5 位。
  const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
  // 从左到右依次为白、黄、青、绿、品红、红、蓝、黑。
  const uint16_t bars[] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
                           0xF81F, 0xF800, 0x001F, 0x0000};
  const char *names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK",
                         "COLOR BARS", "CHECKER + CENTER CROSS"};
  // 逐行、逐像素填充；数组偏移 = 行号 × 宽度 + 列号。
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      uint16_t color;
      if (index < 5) color = colors[index];
      else if (index == 5) {
        color = bars[x * 8 / WIDTH]; // 按宽度等分 8 条；720 像素时每条宽 90 像素
        // 暂时停用两像素白框，用于确认黑色彩条中的白线是否来自边框。
        // if (x < 2 || y < 2 || x >= WIDTH - 2 || y >= HEIGHT - 2) color = 0xFFFF;
      } else {
        // 每格 40×40 像素，相邻格黑白交替，再叠加红色中心十字。
        color = ((x / 40 + y / 40) & 1) ? 0xFFFF : 0x0000;
        if (x == WIDTH / 2 || y == HEIGHT / 2) color = 0xF800;
      }
      frame_buffer[y * WIDTH + x] = color;
    }
    if ((y % 100) == 0) yield(); // 定期让出执行机会
  }
  // DMA 从 PSRAM 读取像素；绘制完后把 CPU 数据缓存写回内存。
  // C2M 表示缓存到内存，TYPE_DATA 表示同步数据缓存。
  if (!check_result("frame cache flush", esp_cache_msync(frame_buffer,
      WIDTH * HEIGHT * sizeof(uint16_t),
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA))) return false;
  // 首帧在 init 前只填充和同步；启动后统一通过驱动提交自有帧缓冲。
  if (display_ready && !check_result("submit frame", esp_lcd_panel_draw_bitmap(
      video_panel, 0, 0, WIDTH, HEIGHT, frame_buffer))) return false;
  Serial.printf("Pattern %u: %s\n", index, names[index]);
  return true;
}

// 上电后执行一次：关闭背光 → 电源 → DSI → 复位 → 初始化 → 视频 → 背光。
void setup() {
  // 先设置输出电平，再配置输出方向：关闭升压，GPIO26 高电平降低电流。
  // 此处使用固定电平，无需启用 LEDC PWM。
  Serial.begin(115200); // 串口监视器设置为 115200 波特率
  delay(300);
  Serial.println("\nHXESP8080HC0400MIC-A: JD9365 720x720 display test [DSI DIAG 3 - 24MHz trial]");
  Serial.printf("SDK: %s; requested pixel clock %.3f MHz\n", esp_get_idf_version(), PIXEL_CLOCK_MHZ);
  if (!check_result("backlight init", backlight_init())) return;
  Serial.println("[1/7] Backlight OFF: GPIO33 LOW, GPIO26 HIGH; external 10k pulldown fitted");

  // 新屏 FPC 4 脚为 VDD_3V3；21 脚 IOVCC 标称 1.8 V。
  // 电气表给出 IOVCC 1.65~3.6 V；若底板接 3.3 V，需厂家确认模块兼容性。
  // 延时仅用于等待稳定，实际电压仍需用万用表测量。
  Serial.println("[2/7] Wait 200 ms for LCD supply settling");
  Serial.println("Check FPC pin 4: 3.3 V; pin 21: IOVCC nominal 1.8 V. Confirm board compatibility.");
  delay(200);
  Serial.println("PHY supply assumption: LDO3 -> VDD_MIPI_DPHY, 2500 mV");
  // 结构体清零后指定通道和电压；成功申请后保留句柄。
  esp_ldo_channel_config_t power_config = {};
  power_config.chan_id = PHY_LDO_CHANNEL;
  power_config.voltage_mv = PHY_MV;
  if (!check_result("acquire PHY LDO",
      esp_ldo_acquire_channel(&power_config, &phy_power))) return;
  delay(20); // 给 PHY 电源预留稳定时间
  Serial.println("[2/7] LDO configured; actual voltage not measured");

  // DSI 使用芯片专用差分引脚；不通过普通 GPIO 指定数据和时钟线。
  esp_lcd_dsi_bus_config_t bus_config = {};
  bus_config.bus_id = 0; // 使用第 0 个 DSI 控制器
  bus_config.num_data_lanes = 2; // 与底板实际连接的 D0、D1 一致
  bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT; // SDK 默认 PHY 时钟源
  // RGB565、24 MHz 对照时每条通道像素负载约需 192 Mbps；36.48 MHz 时约 292 Mbps。
  // 设为 500 Mbps 预留带宽；这是待上板验证的链路参数。
  bus_config.lane_bit_rate_mbps = 500;
  if (!check_result("create DSI bus",
      esp_lcd_new_dsi_bus(&bus_config, &dsi_bus))) return;

  // 创建用于寄存器读写的命令通道，与视频输出共用 DSI 总线。
  esp_lcd_dbi_io_config_t io_config = {};
  io_config.virtual_channel = 0; // 命令和视频都使用虚拟通道 0
  io_config.lcd_cmd_bits = 8; // 命令宽度为 8 位
  io_config.lcd_param_bits = 8; // 单个参数宽度为 8 位
  if (!check_result("create command IO",
      esp_lcd_new_panel_io_dbi(dsi_bus, &io_config, &command_io))) return;
  Serial.println("[3/7] DSI + command IO ready: 2 lanes, 500 Mbps/lane, VC0");

  // 厂家复位时序：先高 5 ms，再低 10 ms，释放为高后等待 120 ms。
  if (!check_result("RESET high", gpio_set_level(LCD_RESET, 1)) ||
      !check_result("RESET output",
          gpio_set_direction(LCD_RESET, GPIO_MODE_OUTPUT))) return;
  delay(5);
  if (!check_result("RESET low", gpio_set_level(LCD_RESET, 0))) return;
  delay(10);
  if (!check_result("RESET release", gpio_set_level(LCD_RESET, 1))) return;
  delay(120);
  Serial.println("[4/7] GPIO27 reset complete: HIGH 5 / LOW 10 / HIGH 120 ms");
  // 单帧 RGB565 需要 720×720×2 = 1,036,800 字节，必须启用 PSRAM。
  Serial.printf("PSRAM total=%lu, free=%lu bytes\n",
                (unsigned long)ESP.getPsramSize(), (unsigned long)ESP.getFreePsram());
  if (!psramFound()) {
    check_result("PSRAM missing: enable PSRAM in Arduino Tools", ESP_ERR_NO_MEM);
    return;
  }

  // 厂家 H:20/20/40、V:4/12/24；水平总周期 800，垂直总周期 760。
  // 目标 60 Hz：36.48 MHz ÷ 800 ÷ 760 = 60 Hz，实际时钟可能被硬件量化。
  // 36.48 是适配计算值，待实屏验证；厂家 PLL_CLOCK=219 不是此处的像素时钟。
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.virtual_channel = 0; // 与命令通道一致
  dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT; // 默认像素时钟源
  dpi.dpi_clock_freq_mhz = PIXEL_CLOCK_MHZ; // 像素时钟，单位 MHz，不是 Lane 速率
  dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565; // 与 0x3A 和缓冲区格式一致
  dpi.num_fbs = 1; // 一个完整帧缓冲，由驱动分配
  dpi.video_timing.h_size = WIDTH; // 有效宽度，单位像素
  dpi.video_timing.v_size = HEIGHT; // 有效高度，单位行
  dpi.video_timing.hsync_pulse_width = 20; // 水平同步宽度，单位像素时钟
  dpi.video_timing.hsync_back_porch = 20; // 水平后沿消隐
  dpi.video_timing.hsync_front_porch = 40; // 水平前沿消隐
  dpi.video_timing.vsync_pulse_width = 4; // 垂直同步宽度，单位行
  dpi.video_timing.vsync_back_porch = 12; // 垂直后沿消隐
  dpi.video_timing.vsync_front_porch = 24; // 垂直前沿消隐
  if (!check_result("create DPI panel",
      esp_lcd_new_panel_dpi(dsi_bus, &dpi, &video_panel))) return;
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {};
  callbacks.on_refresh_done = on_video_refresh;
  if (!check_result("register refresh callback",
      esp_lcd_dpi_panel_register_event_callbacks(video_panel, &callbacks, nullptr))) return;
  // 取得驱动分配的帧缓冲地址；转成 16 位像素指针供绘图使用。
  void *buffer = nullptr;
  if (!check_result("get frame buffer",
      esp_lcd_dpi_panel_get_frame_buffer(video_panel, 1, &buffer))) return;
  frame_buffer = static_cast<uint16_t *>(buffer);

  delay(10); // 厂家初始化表开始前要求的等待时间
  // 逐条发送厂家寄存器设置；记录页号，便于定位初始化失败位置。
  uint8_t page = 0;
  for (const auto &reg : PANEL_REGISTERS) {
    if (reg.command == 0xE0) page = reg.value;
    if (!write_register(reg.command, reg.value)) {
      Serial.printf("Initialization stopped on page %u\n", page);
      return;
    }
  }
  // 必须先切回 Page 0 再发送标准显示命令。
  // Page 1 的 0x80 属于 Gamma 参数，不能误当作 Lane 配置修改。
  if (!write_register(0xE0, 0x00) ||
      !write_register(0x80, 0x01) ||  // 两条数据通道
      !write_register(0x36, 0x00) ||  // RGB 颜色顺序
      !write_register(0x3A, 0x55)) return; // RGB565，与视频输出及帧缓冲一致
  Serial.println("[5/7] Manufacturer registers sent, 2 lanes, RGB565");
  // 0x11：退出休眠，无参数；等待至少 120 ms 后再开启显示。
  if (!check_result("Sleep Out",
      esp_lcd_panel_io_tx_param(command_io, 0x11, nullptr, 0))) return;
  delay(120);
  // TE 引脚未连接，因此不依赖 TE 中断。
  // 0x29：开启面板显示，无参数。
  if (!check_result("Display On",
      esp_lcd_panel_io_tx_param(command_io, 0x29, nullptr, 0))) return;
  delay(5);
  // 先准备红色首帧，再启动持续视频，避免首帧出现未初始化数据。
  if (!show_pattern(0)) return;
  if (!check_result("start DPI video", esp_lcd_panel_init(video_panel))) return;
  Serial.printf("[6/7] Video started: 720x720 RGB565, requested pixel clock %.3f MHz\n", PIXEL_CLOCK_MHZ);
  delay(100);
  // 视频启动后再开启背光，采用此前串口 f 测试成功的固定低电平调光。
  // 不代表亮度百分比已经校准。
  if (!check_result("backlight enable", backlight_set(true))) return;
  display_ready = true; // 后续 loop 才开始响应画面切换
  last_pattern_ms = millis();
  last_report_ms = millis();
  Serial.println("[7/7] Backlight ON. Verify image on the physical screen.");
  Serial.println("Serial: 0..6 select/pause; a auto; n next; d backlight off; e on");
  Serial.println("DIAG: t host vertical bars; h host horizontal bars; f frame buffer");
  Serial.println("DIAG: o panel Display OFF; p panel Display ON (backlight unchanged)");
  Serial.println("Auto cycle: every 2 seconds; refresh counter is host-side evidence only.");
  Serial.println("DIAG: s read DMA/bridge registers; 24 MHz is a temporary bandwidth test.");
}

// 主循环：处理单字符串口命令，并按毫秒计时自动切换测试画面。
void loop() {
  if (!display_ready) { delay(100); return; } // 初始化或绘图失败后保持停止
  report_video_refresh();
  if (Serial.available()) {
    const char c = Serial.read(); // 一次处理一个字符，换行等未知字符会忽略
    if (c == 's') { report_dma_state(); return; }
    bool redraw = false; // 仅在画面编号变化时请求重绘
    if (c == 't' || c == 'h' || c == 'f') {
      auto_cycle = false;
      const mipi_dsi_pattern_type_t mode = c == 't' ? MIPI_DSI_PATTERN_BAR_VERTICAL :
          c == 'h' ? MIPI_DSI_PATTERN_BAR_HORIZONTAL : MIPI_DSI_PATTERN_NONE;
      if (!set_video_pattern(mode)) return;
      Serial.println(c == 't' ? "DIAG: host VERTICAL bars enabled; PSRAM pixels bypassed" :
                     c == 'h' ? "DIAG: host HORIZONTAL bars enabled; PSRAM pixels bypassed" :
                                "DIAG: frame buffer output restored");
      if (c == 'f' && !show_pattern(pattern)) return;
      Serial.println("DIAG: API succeeded; observe the screen to verify reception.");
      return;
    }
    if (c == 'o' || c == 'p') {
      auto_cycle = false;
      if (!write_register(0xE0, 0x00)) return;
      if (!check_result("panel display command", esp_lcd_panel_io_tx_param(
          command_io, c == 'o' ? 0x28 : 0x29, nullptr, 0))) return;
      delay(20);
      Serial.println(c == 'o' ? "DIAG: Display OFF sent; backlight unchanged" :
                                "DIAG: Display ON sent; backlight unchanged");
      return;
    }
    // 手动选色或恢复轮播时退出硬件图案，否则 PSRAM 更新不会出现在屏幕上。
    if (hardware_pattern && ((c >= '0' && c <= '6') || c == 'n' || c == 'a')) {
      if (!set_video_pattern(MIPI_DSI_PATTERN_NONE)) return;
    }
    // 0～6：选择固定画面并暂停轮播。
    if (c >= '0' && c <= '6') {
      pattern = c - '0'; auto_cycle = false; redraw = true;
    } else if (c == 'n') { // 下一张，取模保证编号循环在 0～6
      pattern = (pattern + 1) % 7; auto_cycle = false; redraw = true;
    } else if (c == 'a') { // 恢复自动轮播，并重新开始计时
      auto_cycle = true; last_pattern_ms = millis();
    } else if (c == 'd' || c == 'e') { // d 关闭背光，e 打开；视频仍运行
      if (!check_result("backlight switch", backlight_set(c == 'e'))) return;
    }
    if (redraw) { // 手动绘制后更新时间，防止紧接着发生自动切换
      if (!show_pattern(pattern)) return;
      last_pattern_ms = millis();
    }
  }
  // 无符号时间差可处理 millis() 回绕；每张画面保持约 2 秒。
  if (auto_cycle && millis() - last_pattern_ms >= 2000) {
    pattern = (pattern + 1) % 7;
    if (!show_pattern(pattern)) return;
    last_pattern_ms = millis();
  }
  // 单帧缓冲在扫描时直接更新，切换瞬间可能短暂撕裂；静态图案用于验收。
  delay(10); // 降低主循环空转占用
}
