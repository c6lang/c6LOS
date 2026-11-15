#!/bin/sh
# 刷机 + 立刻抓串口（相当于 idf.py flash monitor）。
# 用法: ./tools/flashmon.sh <bin> [抓取秒数]
#
# 可选环境变量 SHADOW_BIN / SHADOW_OFF：把一份"影子镜像"额外写到指定 flash 偏移。
# 用途（§202）：ROM 给 flash 段建 MMU 映射用的是**段头里的 load addr**（镜像内偏移），
# 而 esptool 把 flash 段的数据按自己的对齐规则摆（第 3 段起一律 0x10000 对齐），
# 两者不一定相等。于是 0x42xxxxxx 取指会取到别的数据。影子镜像就是把 .flash.text
# 的原始字节按 load addr 再写一份，让 MMU 映射能落到正确的字节上。
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
# 影子镜像只能**单独**再写一次：esptool 会拒绝"同一次 write-flash 里区间重叠"。
# 顺序很重要——先写主镜像（它覆盖 0x0..镜像尾），再写影子把它盖回正确内容。
flash_one() {
    # 每次写之前重新找串口：写一次之后 USB-JTAG 重新枚举，ttyACM0/ttyACM1 会跳。
    for j in $(seq 1 20); do
        D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
        [ -n "$D" ] && break
        sleep 1
    done
    [ -n "$D" ] || return 1
    sleep 1
    sudo -n esptool --chip esp32s3 -p "$D" --before default-reset --after watchdog-reset \
         write-flash "$1" "$2" >>/tmp/flashmon.log 2>&1
}
ok=0
: >/tmp/flashmon.log
for t in 1 2 3; do
    if flash_one 0x0 "$BIN"; then
        if [ -n "$SHADOW_BIN" ]; then
            flash_one "$SHADOW_OFF" "$SHADOW_BIN" || { ok=0; continue; }
        fi
        ok=1; break
    fi
    sudo -n python3 "$(dirname "$0")/usbreset.py" >/dev/null 2>&1
    sleep 3
    D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
done
[ "$ok" = 1 ] || { echo "刷机失败：" >&2; tail -3 /tmp/flashmon.log >&2; exit 1; }
echo "已刷入，开始抓取 ${SECS}s ..."
sudo -n python3 "$(dirname "$0")/cap.py" -t "$SECS"
