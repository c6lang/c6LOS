# c6LOS WiFi/HTTPS 真板基线（官方 IDF station 例程 + HTTPS GET）

本目录是从 ESP-IDF v6.0.1 `examples/wifi/getting_started/station` 拷贝出的
参考工程，仅用于**真板基线验证**：在**同一块 ESP32-S3**、同一路由器下证明
硬件/天线/DNS/TLS/HTTP 全部正常，给 c6LOS 侧提供可对照的寄存器与日志。

`main/station_example_main.c` 增加了 `https_qq_task()`：
连接 `https://qq.com/`（CA bundle、自动跳转、事件回调抓前 600 字节 body）。

真板实测（§529）：
```
I wifi:connected with weforpay.com, aid = 2, channel 6, 40U
I wifi station: got ip:192.168.68.120
I https: HTTP status=200 content_length=132932 BODY(600):
  <html> ... <title>腾讯网</title> ...
```

构建/烧录（要点）：
```sh
export IDF_PATH=/media/winger/inst_26/esp/esp-idf
export ESP_IDF_VERSION=6.0.0
export IDF_PYTHON_ENV_PATH=/home/winger/.espressif/python_env/idf6.0_py3.12_env
export IDF_MAINTAINER=1
export PATH=/home/winger/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/bin:$IDF_PYTHON_ENV_PATH/bin:$PATH
python $IDF/tools/idf.py build
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset write-flash @flash_args
```
注意：**必须用 stub 模式（460800）**；`--no-stub` 写出的 app 会被 bootloader
判为 `Image hash failed`（即便 esptool 自校验通过）。若 stub 卡在
`Running stub flasher...`，先跑 `tools/ocboot.sh` 起 OpenOCD 再重试。

本参考工程 **不是** c6LOS；c6LOS 侧扫描仍 0 AP，目标未达成。
