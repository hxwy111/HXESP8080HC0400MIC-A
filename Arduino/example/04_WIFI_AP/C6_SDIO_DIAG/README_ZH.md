# ESP32-C6 SDIO物理链路诊断固件

该临时固件在ESP32-C6侧统计以下输入信号的上升沿：

- C6 GPIO19：P4送来的SDIO CLK（P4 GPIO51）
- C6 GPIO18：双向SDIO CMD（P4 GPIO52）

它不会启动ESP-Hosted，也不会驱动GPIO18或GPIO19。串口需要使用C6 UART，波特率为115200。

## 烧录

使用Flash Download Tool选择ESP32-C6，将`c6_sdio_diag_full.bin`设置在地址`0x0`：

- SPI SPEED：80 MHz
- SPI MODE：DIO
- FLASH SIZE：4 MB

完整镜像SHA-256：

```text
3FBFC1E69CE09CC8CEC31FE981DD4C5A1BC4BFAF0A7D5E17C1D7DF7E1B5F8776
```

烧录时临时将C6 GPIO9接GND并复位。显示`FINISH`后，必须先断电，再断开GPIO9与GND，随后重新上电。

## 串口连接与测试

- C6 GPIO16/U0TXD -> USB-TTL RX
- GND -> USB-TTL GND
- USB-TTL TX可以不接
- 不要连接USB-TTL的5V或3.3V供电脚

P4继续运行`04_WIFI_AP.ino`。C6启动后应先打印：

```text
DIAG_READY CLK=0 CMD=1
```

检测到P4初始化波形后应打印类似：

```text
SDIO_ACTIVITY t=1234ms CLK_rise=+400(total=400) CMD_rise=+12(total=12) levels CLK=0 CMD=1
```

C6每次被P4复位时，计数会重新从0开始。

## 结果判断

- `CLK_rise`非0且`CMD_rise`也非0：CLK/CMD均已从P4到达C6，内部连线基本正常。
- `CLK_rise`非0、`CMD_rise=+0`：重点检查CMD（P4 GPIO52到C6 GPIO18）。
- `CLK_rise=+0`、`CMD_rise`非0：重点检查CLK（P4 GPIO51到C6 GPIO19）。
- 两者始终为0：波形没有到达C6，或者C6在P4发送前被复位。

诊断完成后，必须重新烧回ESP-Hosted 2.11.6固件，Wi-Fi才能工作。
