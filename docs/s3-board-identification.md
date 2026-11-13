# ESP32-S3 板卡/LCD 识别（官方固件残留）

从板上 flash 0x0–0x400000 读回镜像后，用 `strings` 读到原厂应用残留：

- `esp32-s3-touch-lcd-1.85c`
- `./main/boards/waveshare/esp32-s3-touch-lcd-1.85c/esp32-s3-touch-lcd-1.85c.cc`
- `CustomBoard::Initializest77916Display()`
- `esp_lcd_new_panel_st77916(...)`
- `esp_lcd_new_panel_st77916_spi`
- `st77916_spi`

结论：板子是 **Waveshare ESP32-S3-Touch-LCD-1.85C**，LCD 控制器是 **ST77916**（QSPI，360×360 圆屏），不是 ST7789。

注意：当前 c6LOS 镜像写在 0x0，原厂 bootloader 已被覆盖；上述识别来自 flash 中残留的原厂应用字符串。若要完整跑原厂 UI，需要原厂 16MB 整片备份或重新下载官方固件。
