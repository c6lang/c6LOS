# Xingzhi Cube 1.54TFT WiFi 可用固件

这是这台机器实际能点亮屏幕、稳定运行的 XiaoZhi 固件（用户确认“这个可以”）。

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
