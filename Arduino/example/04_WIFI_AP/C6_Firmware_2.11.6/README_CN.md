# ESP32-C6 ESP-Hosted 2.11.6 固件

该目录用于恢复 HXESP8080HC0340MIC-A 板载 ESP32-C6 的 Wi-Fi 协处理器固件，匹配当前 Arduino-ESP32 Core 3.3.7 中的 ESP-Hosted 主机版本 2.11.6。

## 固件文件

- `esp32c6_hosted_2.11.6_full.bin`：推荐使用的首次串口烧录合并镜像，烧录地址为 `0x0`。
- `bootloader.bin`：烧录地址 `0x0`。
- `partition-table.bin`：烧录地址 `0x8000`。
- `ota_data_initial.bin`：烧录地址 `0xD000`。
- `esp32c6_hosted_2.11.6_app.bin`：烧录地址 `0x10000`。

合并镜像已经包含上述四个分离镜像，因此正常情况下只需要烧录 `esp32c6_hosted_2.11.6_full.bin`。

## C6-UART接线

使用3.3V USB转串口模块，不要把5V接到任何C6信号脚。

| USB转串口 | 开发板C6-UART接口 |
| --- | --- |
| TX | U3 pin 1，C6_U0RXD |
| RX | U3 pin 2，C6_U0TXD |
| GND | U3 pin 4，GND |

开发板由自身USB接口供电，不要连接USB转串口模块的VCC。

## 进入C6下载模式

1. 断开开发板全部电源。
2. 将U3 pin 3（C6_IO9）连接到GND。
3. 接好USB转串口的TX、RX和GND。
4. 重新给开发板供电。
5. 运行烧录脚本。这里进入的是板载C6下载模式，不是P4下载模式。

假设USB转串口端口为 `COM8`：

```powershell
powershell -ExecutionPolicy Bypass -File .\flash_c6.ps1 -Port COM8
```

如果需要先完整擦除C6 Flash：

```powershell
powershell -ExecutionPolicy Bypass -File .\flash_c6.ps1 -Port COM8 -EraseFlash
```

如果460800波特率不稳定，可以降低到115200：

```powershell
powershell -ExecutionPolicy Bypass -File .\flash_c6.ps1 -Port COM8 -Baud 115200
```

## 烧录完成

1. 完断电。
2. 移除C6_IO9与GND之间的连接。
3. 重新上电。
4. 再运行P4端Arduino Wi-Fi示例。

C6正常启动后，不应再出现以下连续超时：

```text
sdmmc_init_ocr: send_op_cond (1) returned 0x107
H_SDIO_DRV: card init failed
```

如果重新上电后仍出现相同错误，请读取C6_U0TXD的115200波特率启动日志，并检查C6供电、GPIO54复位线和P4到C6的SDIO线路。

## 校验值

```text
bootloader.bin
SHA256 9A96CA5BEFD74F6A145E35544CFC3BD5CE8C88E2E5D0C1E17CDBDE2A0D6A7F70

partition-table.bin
SHA256 0241FA0D2E573DEE86756E39FB4181E61FF7218087619CAB565B602DF55954D6

ota_data_initial.bin
SHA256 7D2C7AC4888BFD75CD5F56E8D61F69595121183AFC81556C876732FD3782C62F

esp32c6_hosted_2.11.6_app.bin
SHA256 E32FBA3864AB4DB82C287A922DB83B7093D7D8592730D7A620887B7CFDF401E0

esp32c6_hosted_2.11.6_full.bin
SHA256 7CDECD71D4E2CF7014023C849E5394D9384AD625A1F066B0C6D8289D9D19232A
```

应用镜像信息：ESP32-C6、4MB Flash、DIO、80MHz、ESP-Hosted 2.11.6，编译自ESP-IDF 5.5系列。
