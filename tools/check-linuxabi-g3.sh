#!/bin/bash
# G3 宿主闭环：用宿主 Chromium 的 --remote-debugging-pipe 跑真实 CDP，
# c6chrome-display 解 PNG 并写 PPM；客户机内同一二进制只把 PPM 换成 fb0。
set -eu
cd "$(dirname "$0")/.."
CHROME="${CHROME_BIN:-chromium}"
if ! command -v "$CHROME" >/dev/null 2>&1; then
    echo "FAIL: linuxabi-g3（找不到宿主 Chromium：$CHROME）"
    exit 1
fi
OUT="$(mktemp /tmp/c6g3-frame.XXXXXX.ppm)"
LOG="$(mktemp /tmp/c6g3-cdp.XXXXXX.log)"
trap 'rm -f "$OUT" "$LOG"' EXIT
BIN=build/x86/c6chrome-display.elf
TEST=build/x86/t_cdp
make -s "$BIN" "$TEST"
"$TEST"
URL='data:text/html,<body style="margin:0;background:%23ff0000"><div style="width:100px;height:100px;background:%2300ff00"></div></body>'
set +e
timeout 45 "$BIN" --chrome="$(command -v "$CHROME")" --url="$URL" \
    --output="$OUT" >"$LOG" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ] || [ ! -s "$OUT" ]; then
    echo "FAIL: linuxabi-g3（CDP 显示客户端 rc=$rc）"
    tail -20 "$LOG"
    exit 1
fi
grep -q '^frame: ' "$LOG" || { echo "FAIL: linuxabi-g3（没有解出 screencastFrame）"; tail -20 "$LOG"; exit 1; }
python3 tools/ppm_check.py --file "$OUT" --min-w 640 --min-h 480 \
    --pixel 10,10,0,255,0,8 --pixel 640,400,255,0,0,8 --nonblank
echo "PASS: linuxabi-g3（CDP pipe → PNG → 帧像素；客户机路径同一代码写 /dev/fb0）"
