# 这块 ESP32-S3 的实际板卡/LCD 识别（最终）

## 结论

这台不是 Waveshare ESP32-S3-Touch-LCD-1.85C（ST77916 360×360 QSPI），而是
**Xingzhi Cube 1.54TFT WiFi**：

- LCD：**ST7789，240×240，SPI**
- SDA=GPIO10，SCL=GPIO9，DC=GPIO8，CS=GPIO14，RES=GPIO18，BL=GPIO13
- 音频 I2S：MIC WS4/SCK5/DIN6，SPK DOUT7/BCLK15/LRCK16
- 按键：BOOT=0，VOL+=40，VOL-=39

## 证据

- 原厂 1.85C 固件启动即 `TCA9554 create returned error`，在 `InitializeTca9554()`
  `ESP_ERROR_CHECK` abort，LCD 还没初始化，所以黑屏。
- 1.85B 固件虽然能初始化 ST77916/背光，但与实际 ST7789 面板不匹配。
- 刷 `v2.5.0_xingzhi-cube-1.54tft-wifi.bin` 后系统稳定运行、屏幕点亮；串口持续输出
  `SystemInfo: free sram ...`。
- 该固件内字符串：`xingzhi-cube-1.54tft-wifi`、
  `InitializeSt7789Display()`、`esp_lcd_new_panel_st7789(...)`、
  `SpiLcdDisplay`。

## 上游固件出处（xiaozhi-esp32）

`firmware/xingzhi-cube-1.54tft-wifi/` 里那个可点亮整机的 bin，是开源项目
**小智 AI（xiaozhi-esp32，作者 78/虾哥）** 的官方 release，不是某家板厂的闭源包：

- 仓库：<https://github.com/78/xiaozhi-esp32>
- 版本：tag `v2.5.0`，release 资产 `v2.5.0_xingzhi-cube-1.54tft-wifi.bin`
- 板级源码：`main/boards/nologo/xingzhi-cube-1.54tft-wifi/`
  （`config.h`、`config.json`、`xingzhi-cube-1.54tft-wifi.cc`；`type` 即
  `xingzhi-cube-1.54tft-wifi`，`manufacturer` 为 `nologo`）

它的板级配置与本机丝印/实测完全一致，可以作为 ST7789 驱动的权威参考：

| 项 | 上游取值 | 备注 |
| --- | --- | --- |
| 面板 | ST7789 240×240，`rgb_ele_order = RGB`，`bits_per_pixel = 16` | IDF `esp_lcd_new_panel_st7789` |
| 引脚 | SDA=10 SCL=9 DC=8 CS=14 RES=18 BL=13 | 与实测一致 |
| SPI | **`SPI3_HOST`，`spi_mode = 3`，`pclk_hz = 80MHz`** | 我们用 SPI2 + mode 0，尚未对齐 |
| 传输 | `max_transfer_sz = 240*240*2 = 115200`，`SPI_DMA_CH_AUTO`，队列深度 10 | 正好一个整帧 |
| 方向/颜色 | swap_xy=false，mirror_x/y=false，offset 0/0，`invert_color(true)`（发 INVON 0x21） | 我们已发 INVON |
| 背光 | `PwmBacklight(13)`，`output_invert = false` | 原厂用 LEDC PWM 调光 |
| 电源保持 | 启动即 `rtc_gpio_init(21)` + 输出 + **电平 1**；关机时置 0 再 deep sleep | GPIO21 是板级电源保持/使能脚 |
| 充电检测 | `PowerManager(GPIO_NUM_38)` | 输入脚，驱动不需要 |
| 音频/按键 | MIC WS4/SCK5/DIN6，SPK DOUT7/BCLK15/LRCK16；BOOT=0，VOL+=40，VOL-=39 | 与 M7 无关，记录备查 |

## 对 c6os 的影响

`port/s3/panel.c` 现有的 ST77916/QSPI 驱动是给另一块 Waveshare 1.85C 写的，
不适用于这台 ST7789 SPI 屏；RTOS 性能/对比不受影响，但要点亮这块屏需要按
ST7789 SPI 引脚重写面板驱动。
