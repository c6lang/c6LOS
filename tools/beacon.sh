#!/bin/sh
# 一条命令读出 4 个 RTC 信标（0x600080C0..CC）。
# 用途：串口不可靠时定位"固件跑到哪一步/卡在哪"——这些寄存器在掉电域，
# CPU 复位、看门狗复位都不会清（只有真正断电才丢）。
set -e
D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
if [ -z "$D" ]; then
    echo "没有串口：板子不在总线上（试试 tools/usbreset.py 或检查供电）" >&2
    exit 1
fi
for a in 0x600080C0 0x600080C4 0x600080C8 0x600080CC; do
    sudo -n esptool --chip esp32s3 -p "$D" --before default-reset read-mem "$a" 2>&1 | grep -E "^0x"
done
