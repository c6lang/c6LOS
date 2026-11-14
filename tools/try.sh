#!/bin/sh
# 用法: ./tools/try.sh <用例.c6> [秒数]
# 一键完成：构建 -> 生成 IDF 镜像 -> 烧 bootloader+分区表+app -> 复位 -> 抓串口 -> 读信标
set -e
SRC="${1:?用法: $0 <tests/xxx.c6> [秒数]}"
SECS="${2:-5}"
NAME=$(basename "$SRC" .c6)
BUILD="build/try-$NAME"
BOOTLOADER=${BOOTLOADER:-/media/winger/inst_26/esp/xiaozhi/build/bootloader/bootloader.bin}
PART=${PART:-/tmp/p2.bin}

echo "== 构建 $SRC"
make C6_ROOT=/home/winger/c6 BUILD="$BUILD" S3_MAIN="$SRC" s3 > "$BUILD.log" 2>&1 || {
    echo "构建失败:"; grep -aiE "error|undefined" "$BUILD.log" | head -10; exit 1; }

echo "== 生成镜像"
python3 tools/mkimg.py "$BUILD/c6os.elf" "$BUILD/app.bin" | head -3

echo "== 烧录"
pkill -x openocd 2>/dev/null || true
pkill -f "tools/cap.py" 2>/dev/null || true
sleep 1
D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
[ -n "$D" ] || { echo "找不到串口"; exit 1; }
sudo -n esptool --chip esp32s3 -p "$D" write-flash 0x0     "$BOOTLOADER" >/dev/null 2>&1
sudo -n esptool --chip esp32s3 -p "$D" write-flash 0x8000  "$PART"       >/dev/null 2>&1
sudo -n esptool --chip esp32s3 -p "$D" write-flash 0x20000 "$BUILD/app.bin" >/dev/null 2>&1
echo "   三样都写完了"

echo "== 复位并抓串口 ${SECS}s"
sudo -n python3 tools/reset.py >/dev/null 2>&1 || true
sleep 1
sudo -n python3 tools/cap.py -t "$SECS" 2>&1 \
    | grep -avE "^load:|^SPIWP|^mode:|^ESP-ROM|^Build:|^rst:" | tail -25

echo "== 信标"
./tools/beacon.sh 2>&1 | head -4
