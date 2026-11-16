#!/bin/bash
# §289：用 **IDF 真正的二级 bootloader** 引导 c6os，而不是让我们的镜像自己当 bootloader。
#   0x000000  bootloader.bin   （IDF 的二级 bootloader：时钟树、cache/MMU、PSRAM 全配好）
#   0x008000  分区表（参照工程自带：ota_0 在 0x20000）
#   0x00d000  擦掉 otadata：没有 otadata 时 bootloader 直接启 ota_0，行为确定
#   0x020000  c6os.bin（构建时传 S3_APP_BASE=0x20000）
# 用法：tools/flashbl.sh <c6os.bin> [秒数]
set -e
BIN=${1:?用法: flashbl.sh <c6os.bin> [秒数]}
SECS=${2:-15}
REF=/media/winger/inst_26/esp/xiaozhi/build
D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
[ -n "$D" ] || { echo "没有 /dev/ttyACM*，板子掉了吧？"; exit 1; }
echo "== 写 bootloader + 分区表 + 应用 @0x20000 =="
sudo -n esptool --chip esp32s3 -p "$D" --before default-reset --after watchdog-reset \
    write-flash 0x0 "$REF/bootloader/bootloader.bin" \
                0x8000 "$REF/partition_table/partition-table.bin" \
                0x20000 "$BIN" >>/tmp/flashbl.log 2>&1 || { tail -5 /tmp/flashbl.log; exit 1; }
echo "== 擦 otadata =="
sudo -n esptool --chip esp32s3 -p "$D" --before default-reset --after watchdog-reset \
    erase-region 0xd000 0x2000 >>/tmp/flashbl.log 2>&1 || true
echo "== 串口 $SECS 秒 =="
timeout $((SECS+5)) python3 tools/cap.py "$SECS" 2>/dev/null | tail -40
