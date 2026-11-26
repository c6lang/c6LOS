#!/bin/bash
# G4 客户机验收：c6g4demo（clone 服务线程 + wl_demo）写 fb0，
# QEMU VBE screendump 验第三帧绿色方块/红底。
set -eu
cd "$(dirname "$0")/.."
SER=$(mktemp /tmp/g4guest-ser.XXXXXX)
PPM=$(mktemp /tmp/g4guest-scr.XXXXXX.ppm)
MON=$(mktemp -u /tmp/g4guest-mon.XXXXXX)
rm -f "$PPM" "$MON"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null || true; rm -f "$MON" "$SER"; }
trap cleanup EXIT INT TERM
make -s x86-g4guest
qemu-system-x86_64 -m 512M -cdrom build/x86/g4.iso -boot d \
    -vga std -display none -serial file:"$SER" \
    -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
i=0
while [ "$i" -lt 120 ]; do
    if grep -q '^g4-done' "$SER" 2>/dev/null; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    i=$((i+1)); sleep 0.1
done
if ! grep -q '^g4-done' "$SER" 2>/dev/null; then
    echo "FAIL: linuxabi-g4 guest（wl_demo 未跑完）"; tail -30 "$SER"; exit 1
fi
python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time, os
mon, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try:
        s.connect(mon); break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("FAIL: linuxabi-g4 guest（连不上 monitor）")
time.sleep(0.2)
s.sendall(("screendump %s\n" % out).encode())
time.sleep(0.5)
s.sendall(b"quit\n")
s.close()
for _ in range(30):
    if os.path.exists(out) and os.path.getsize(out) > 16:
        break
    time.sleep(0.1)
PY
python3 tools/ppm_check.py --file "$PPM" --min-w 640 --min-h 480 \
    --pixel 350,190,0,255,0,12 --pixel 700,100,255,0,0,12 --nonblank
if [ -n "${G4_KEEP_PPM:-}" ]; then cp "$PPM" "$G4_KEEP_PPM"; fi
echo "PASS: linuxabi-g4 guest（c6wl + wl_demo 合成上屏；800x600 screendump 像素）"
