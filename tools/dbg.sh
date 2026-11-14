#!/bin/sh
# 用法: ./tools/dbg.sh <elf> <gdb脚本>  —— 通过内置 USB-JTAG（不需要 UART）调试
set -e
ELF="$1"; SCRIPT="$2"
[ -n "$ELF" ] && [ -n "$SCRIPT" ] || { echo "用法: $0 <elf> <gdb脚本>" >&2; exit 1; }
OC=$(ls -d ~/.espressif/tools/openocd-esp32/*/openocd-esp32/bin/openocd 2>/dev/null | head -1)
GDB=$(ls -d ~/.espressif/tools/xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb 2>/dev/null | head -1)
pkill -x openocd 2>/dev/null || true
sleep 1
"$OC" -f board/esp32s3-builtin.cfg >/tmp/oc.log 2>&1 &
OCPID=$!
i=0
while [ $i -lt 25 ]; do
    grep -q "Listening on port 3333" /tmp/oc.log 2>/dev/null && break
    sleep 1
    i=$((i+1))
done
grep -q "Listening on port 3333" /tmp/oc.log 2>/dev/null || {
    echo "openocd 起不来：" >&2; tail -3 /tmp/oc.log >&2; kill $OCPID 2>/dev/null; exit 1; }
timeout 120 "$GDB" --batch -nx -x "$SCRIPT" "$ELF" 2>&1
kill $OCPID 2>/dev/null || true
