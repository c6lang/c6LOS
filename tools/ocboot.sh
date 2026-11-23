#!/bin/sh
# 后台起 openocd（telnet 4444 / gdb 3333），给 tools/ocstep.py 用。
# 与 tools/dbg.sh 用同一套路径查找逻辑，但**不杀进程、不跑 gdb**，留在后台。
ESP_TOOLS=${ESP_TOOLS:-/media/winger/inst_26/esp/.espressif}
OC=$(ls -d "$ESP_TOOLS"/tools/openocd-esp32/*/openocd-esp32/bin/openocd 2>/dev/null | head -1)
[ -n "$OC" ] || OC=$(ls -d "$HOME"/.espressif/tools/openocd-esp32/*/openocd-esp32/bin/openocd 2>/dev/null | head -1)
[ -n "$OC" ] || { echo "找不到 openocd（试过 \$ESP_TOOLS 和 ~/.espressif）" >&2; exit 1; }
pkill -x openocd 2>/dev/null || true
sleep 1
# gdb_breakpoint_override hard：闭源库在 flash 里，软件断点写不进去，
# 必须强制用硬件断点（Xtensa 有 2 个 IBRK）。
# ESP_ONLYCPU 1：只暴露 cpu0。否则 gdb 会看到 cpu1 一直 "running"
# （单核固件根本不给它上电，examine 永远失败），于是 continue/stepi 全部报
# "Cannot execute this command while the target is running"，等于没法调试。
setsid "$OC" -c "set ESP_ONLYCPU 1" -c "gdb_breakpoint_override hard" \
    -f board/esp32s3-builtin.cfg >/tmp/oc.log 2>&1 &
i=0
while [ $i -lt 25 ]; do
    grep -q "Listening on port 3333" /tmp/oc.log 2>/dev/null && break
    sleep 1
    i=$((i+1))
done
grep -q "Listening on port 3333" /tmp/oc.log 2>/dev/null || {
    echo "openocd 起不来：" >&2; tail -3 /tmp/oc.log >&2; exit 1; }
echo "openocd 已就绪（telnet 4444 / gdb 3333）"
