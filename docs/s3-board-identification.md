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

## 对 c6LOS 的影响

`port/s3/panel.c` 现有的 ST77916/QSPI 驱动是给另一块 Waveshare 1.85C 写的，
不适用于这台 ST7789 SPI 屏；RTOS 性能/对比不受影响，但要点亮这块屏需要按
ST7789 SPI 引脚重写面板驱动。
