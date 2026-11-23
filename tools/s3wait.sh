#!/bin/sh
# 等 ESP32-S3 的 USB 设备出现（303a:xxxx 或 /dev/ttyACM*），出现后立刻执行一条命令。
#
# 典型用法（板子枚举有问题、需要人工"按住 BOOT 再复位"时）：
#
#     ./tools/s3wait.sh make C6_ROOT=/home/winger/c6 bringup
#
# 脚本会一直盯着（默认 300s，可用 WAIT_SECS 改），一旦看到设备就执行后面的命令，
# 这样"按 BOOT+RST 进下载模式"和"开始刷机"之间不需要抢时间。
#
# 另外它会顺手打印 dmesg 里最近和 USB 有关的行，方便判断枚举到底失败在哪一步。
set -e
WAIT_SECS=${WAIT_SECS:-300}
CMD="$*"
[ -n "$CMD" ] || { echo "用法: $0 <要执行的命令...>" >&2; exit 1; }

show_usb() {
    D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
    V=$(lsusb 2>/dev/null | grep -i "303a:" | head -1)
    if [ -n "$D" ] || [ -n "$V" ]; then
        echo "== 看到设备：${V:-（无 lsusb 行）} ${D:-（无 ttyACM）}"
        return 0
    fi
    return 1
}

if show_usb; then
    echo "== 设备已在，直接执行：$CMD"
    exec $CMD
fi

echo "== 等 ESP32-S3 出现（最多 ${WAIT_SECS}s）"
echo "   提示：按住 BOOT(GPIO0) → 点一下 RST/EN → 松开 BOOT，进入下载模式"
i=0
while [ "$i" -lt "$WAIT_SECS" ]; do
    if show_usb; then
        echo "== 设备出现，等 1.5s 让它枚举完，然后执行：$CMD"
        sleep 1.5
        exec $CMD
    fi
    if [ $((i % 10)) -eq 0 ] && [ "$i" -gt 0 ]; then
        printf '   ... 已等 %ss\n' "$i"
    fi
    sleep 1
    i=$((i + 1))
done

echo "== 超时：${WAIT_SECS}s 内没看到设备" >&2
echo "== 最近的内核 USB 日志：" >&2
sudo -n dmesg 2>/dev/null | grep -iE "usb|ttyacm" | tail -15 >&2 || true
exit 1
