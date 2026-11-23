#!/bin/sh
# 一键上板：等串口 → 刷镜像 → 抓串口 → 存日志 → 打摘要。
#
# 用法： ./tools/s3run.sh <bin> [抓取秒数]
#
# 为什么要有它：板子动不动就从 USB 上消失（§202/§205），每次实验都要
# "等设备回来 → 刷 → 抓 → 找关键字"这几步。这里一条命令做完，并且把串口
# 原始日志落到 build/logs/，出问题能事后翻。
#
# 摘要会 grep 这些标记（顺序就是启动顺序）：
#   [phy] g_phyFuns= / [phy] register_chipv7_phy rc=     §205/§206 PHY
#   [bisect]                                             §204 自动二分
#   [wifi] scan_start / AP0 / 已关联                     §215 射频与关联
#   [lwip] start rc= / ip=                               §210 DHCP
#   [https] TLS 握手完成 / 第一跳 / 第二跳               §213/§214 取页面
set -e
BIN="$1"
SECS="${2:-60}"
[ -n "$BIN" ] || { echo "用法: $0 <bin> [抓取秒数]" >&2; exit 1; }
[ -f "$BIN" ] || { echo "找不到镜像: $BIN" >&2; exit 1; }

LOGDIR=${LOGDIR:-build/logs}
mkdir -p "$LOGDIR"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$LOGDIR/s3run-$STAMP.log"

echo "== 等串口（最多 120s，板子可能还在自己复位）"
i=0
while [ $i -lt 120 ]; do
    D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
    [ -n "$D" ] && break
    sleep 1
    i=$((i + 1))
done
[ -n "$D" ] || { echo "没有串口：板子可能没插/没上电" >&2; exit 1; }

echo "== 刷 $BIN 并抓 ${SECS}s（日志 $LOG）"
./tools/flashmon.sh "$BIN" "$SECS" 2>&1 | tee "$LOG" || true

echo
echo "== 摘要"
for k in "\[phy\] g_phyFuns" "\[phy\] register_chipv7_phy rc" "\[bisect\]" \
         "\[wifi\] scan_start" "\[wifi\] AP0" "\[wifi\] 已关联" \
         "\[lwip\] start rc" "\[lwip\] ip=" "\[https\] TLS 握手完成" \
         "\[https\] 第一跳" "\[https\] 第二跳" "panic\|Guru\|IllegalInstruction"; do
    line=$(grep -aE "$k" "$LOG" | tail -2)
    if [ -n "$line" ]; then
        printf '  %-40s %s\n' "$k" "$(echo "$line" | tr '\n' ' | ')"
    else
        printf '  %-40s (没出现)\n' "$k"
    fi
done
echo
echo "完整日志: $LOG"
