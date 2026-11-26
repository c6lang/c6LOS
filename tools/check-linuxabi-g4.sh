#!/bin/bash
# G4 宿主闭环：起 c6wl（自包含 Wayland server），跑 wl_demo 的
# wl_compositor + wl_shm + xdg_shell；合成结果写 PPM 验像素。
# 客户机里同一对二进制把 PPM 换成 /dev/fb0。
set -eu
cd "$(dirname "$0")/.."
OUT="$(mktemp /tmp/c6g4-frame.XXXXXX.ppm)"
LOG="$(mktemp /tmp/c6g4.XXXXXX.log)"
SOCK="$(mktemp -u /tmp/c6g4-sock.XXXXXX)"
trap 'rm -f "$OUT" "$LOG" "$SOCK"' EXIT
make -s build/x86/c6wl.elf build/x86/wl_demo.elf
"build/x86/c6wl.elf" --socket="$SOCK" --output="$OUT" --width=800 --height=600 \
    >"$LOG" 2>&1 &
SRV=$!
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; sleep 0.02; done
set +e
timeout 20 build/x86/wl_demo.elf --socket="$SOCK" --width=800 --height=600 \
    >>"$LOG" 2>&1
rc=$?
set -e
wait "$SRV" 2>/dev/null || true
if [ "$rc" -ne 0 ]; then
    echo "FAIL: linuxabi-g4（wl_demo rc=$rc）"
    tail -30 "$LOG"
    exit 1
fi
grep -q 'c6wl: output' "$LOG" || { echo "FAIL: linuxabi-g4（合成器没有输出）"; tail -30 "$LOG"; exit 1; }
# 最后一帧方块 x=500..620、y=250..370；取 (560,300) 绿，(700,100) 红。
python3 tools/ppm_check.py --file "$OUT" --min-w 640 --min-h 480 \
    --pixel 560,300,0,255,0,8 --pixel 700,100,255,0,0,8 --nonblank
echo "PASS: linuxabi-g4（wl_shm buffer + xdg_shell configure + 合成像素）"
