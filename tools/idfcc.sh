#!/bin/sh
# 用我们的工具链 + IDF 源码树，把 IDF 的 esp_wifi 源码层编译成 windowed 对象。
# 用法: ./tools/idfcc.sh <idf源文件.c> <输出.o>
# 说明：头文件靠 IDF 的 components/*/include 全量搜索；已经补了两个兼容头：
#   port/s3/idf_compat/sdkconfig.h       —— IDF 配置
#   port/s3/idf_compat/FreeRTOSConfig.h  —— FreeRTOS 配置
set -e
IDF=${IDF_PATH:-/media/winger/inst_26/esp/esp-idf}
XT=~/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc
SRC="$1"; OUT="$2"
[ -n "$SRC" ] && [ -n "$OUT" ] || { echo "用法: $0 <src.c> <out.o>" >&2; exit 1; }
INCS="-I$IDF/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos \
-I$IDF/components/freertos/FreeRTOS-Kernel/portable/xtensa/include \
$(find $IDF/components -maxdepth 4 -type d -name include 2>/dev/null \
  | grep -vE "/linux/|/test|/esp32c|/esp32h|/esp32p|/esp32[^s]|/riscv|/mock|/portable/" \
  | sed 's/^/-I/' | tr '\n' ' ')"
exec $XT -mabi=windowed -Os -ffreestanding -fno-builtin -fno-common \
    -mlongcalls -mtext-section-literals $INCS \
    -I$IDF/components/esp_wifi/esp32s3 -Iport/s3/idf_compat \
    -I$IDF/components/soc/esp32s3/register -I$IDF/components/soc/esp32s3/include \
    -I$IDF/components/soc/include \
    -c "$SRC" -o "$OUT"
