#!/bin/bash
# M26：Linux ABI 统一回归入口。
# 按依赖顺序跑所有 check-linuxabi-* / check-{sal,ip,ping,udp,tcp} / check-alpine，
# 单项失败不中断，末尾汇总；任一项失败则本脚本退出 1。
#
# 用法：make check-linuxabi-all
# 说明：每个 check 目标内部已有 timeout，这里只做编排与汇总。
set -u
cd "$(dirname "$0")/.." || exit 1

TESTS="
check-linuxabi
check-linuxabi-l2
check-linuxabi-l4
check-linuxabi-l5
check-linuxabi-l6
check-linuxabi-l7
check-linuxabi-l8
check-linuxabi-m9
check-linuxabi-m13
check-linuxabi-m13b
check-linuxabi-m13c
check-linuxabi-m13d
check-linuxabi-m14
check-linuxabi-m15
check-linuxabi-m16
check-linuxabi-m17
check-linuxabi-m18
check-linuxabi-m19
check-linuxabi-m20
check-linuxabi-m21
check-linuxabi-m22
check-linuxabi-m23
check-linuxabi-m24
check-linuxabi-m25
check-linuxabi-m27
check-sal
check-ip
check-ping
check-udp
check-tcp
check-alpine
check-alpine-apps
check-linuxabi-m35
"

pass=0
fail=0
failed_list=""

for t in $TESTS; do
    printf '=== %s ===\n' "$t"
    out=$(make "$t" 2>&1)
    rc=$?
    line=$(printf '%s\n' "$out" | grep -E '^(PASS|FAIL):' | tail -1)
    if [ "$rc" -eq 0 ] && printf '%s' "$line" | grep -q '^PASS:'; then
        printf '  %s\n' "$line"
        pass=$((pass + 1))
    else
        printf '  FAIL: %s (rc=%d)\n' "$t" "$rc"
        printf '%s\n' "$out" | tail -12 | sed 's/^/    /'
        fail=$((fail + 1))
        failed_list="$failed_list $t"
    fi
    # 清理可能残留的 qemu（内部 timeout 被外层打断时会留锁）
    pkill -9 qemu-system-x86_64 2>/dev/null || true
    sleep 1
done

printf '\n=== Linux ABI 统一回归汇总: %d PASS, %d FAIL ===\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
    printf '失败项:%s\n' "$failed_list"
    exit 1
fi
printf 'PASS: check-linuxabi-all（%d 项全绿）\n' "$pass"
exit 0
