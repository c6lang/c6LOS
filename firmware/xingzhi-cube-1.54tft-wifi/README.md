# Xingzhi Cube 1.54TFT WiFi 可用固件

这是这台机器实际能点亮屏幕、稳定运行的 XiaoZhi 固件（用户确认“这个可以”）。

## 上游出处

- 仓库：<https://github.com/78/xiaozhi-esp32>（**小智 AI / xiaozhi-esp32**，作者 78/虾哥）
- 版本：tag `v2.5.0`，release 资产 `v2.5.0_xingzhi-cube-1.54tft-wifi.bin`
- 板级源码：`main/boards/nologo/xingzhi-cube-1.54tft-wifi/`
  （`config.h` / `config.json` / `xingzhi-cube-1.54tft-wifi.cc`）

它是开源固件按板级配置编出来的原厂 release，因此板子的权威参数（引脚、SPI
模式/时钟、初始化流程、电源保持脚）都能在上游源码里查到，详见
`docs/s3-board-identification.md` 的「上游固件出处」一节。

## 板级配置要点（自研驱动对齐用）

- LCD：ST7789 240×240，16bpp，RGB 元素顺序，`invert_color(true)`（INVON 0x21）
- 引脚：SDA=10 SCL=9 DC=8 CS=14 RES=18 BL=13（BL 用 PWM 调光，非反相）
- SPI：上游用 `SPI3_HOST` + **mode 3** + **80MHz**，单次最大传输 115200B（整帧）
- 电源保持：启动时把 **GPIO21**（RTC GPIO）拉高；关机时拉 0 再 deep sleep
- 充电检测：GPIO38（仅输入）

- 文件：`v2.5.0_xingzhi-cube-1.54tft-wifi.bin`
- 板卡：Xingzhi Cube 1.54TFT WiFi
- LCD：ST7789，240×240，SPI
- 引脚：SDA=10 SCL=9 DC=8 CS=14 RES=18 BL=13
- 烧写：整片写 0x0

```sh
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --after watchdog-reset write-flash --flash-mode dio --flash-size 16MB \
  --flash-freq 80m 0x0 v2.5.0_xingzhi-cube-1.54tft-wifi.bin
```

注意：Waveshare ESP32-S3-Touch-LCD-1.85B/1.85C 的固件不适用这台机器：
1.85C 会卡在 TCA9554，1.85B 虽初始化 ST77916 但与实际 ST7789 屏不匹配。
