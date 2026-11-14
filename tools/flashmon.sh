#!/bin/sh
# 刷机 + 立刻抓串口（相当于 idf.py flash monitor）。
# 用法: ./tools/flashmon.sh <bin> [抓取秒数]
set -e
BIN="$1"
SECS="${2:-8}"
[ -n "$BIN" ] || { echo "用法: $0 <bin> [秒数]" >&2; exit 1; }

for i in $(seq 1 20); do
    D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
    [ -n "$D" ] && break
    sleep 1
done
[ -n "$D" ] || { echo "没有串口" >&2; exit 1; }

# --after watchdog-reset：用 RTC 看门狗复位（不经过 USB-JTAG 的"下载模式"通道）
ok=0
for t in 1 2 3; do
    sudo -n esptool --chip esp32s3 -p "$D" --before default-reset --after watchdog-reset \
         write-flash 0x0 "$BIN" >/tmp/flashmon.log 2>&1 && { ok=1; break; }
    sudo -n python3 "$(dirname "$0")/usbreset.py" >/dev/null 2>&1
    sleep 3
    D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
done
[ "$ok" = 1 ] || { echo "刷机失败：" >&2; tail -3 /tmp/flashmon.log >&2; exit 1; }
echo "已刷入，开始抓取 ${SECS}s ..."
sudo -n python3 "$(dirname "$0")/cap.py" -t "$SECS"
