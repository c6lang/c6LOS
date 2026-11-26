#!/bin/sh
# G2 屏幕端到端：串口跑 g2dev，再经 QEMU monitor screendump 校验 fb0 像素。
set -eu
DISK=${G2_DISK:-build/x86/g2-disk.img}
[ -f "$DISK" ] || { echo "FAIL: linuxabi-g2（缺 $DISK）"; exit 1; }
SER=$(mktemp /tmp/g2ser.XXXXXX)
PPM=$(mktemp /tmp/g2scr.XXXXXX.ppm)
MON=$(mktemp -u /tmp/g2mon.XXXXXX)
rm -f "$PPM" "$MON"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null || true; rm -f "$MON"; }
trap cleanup EXIT INT TERM

qemu-system-x86_64 -m 512M -drive format=raw,file="$DISK",if=ide -boot c \
    -vga std -display none -serial file:"$SER" \
    -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!

i=0
while [ "$i" -lt 100 ]; do
    if grep -q '^PASS: g2dev' "$SER" 2>/dev/null; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    i=$((i+1)); sleep 0.15
done
if ! grep -q '^PASS: g2dev' "$SER" 2>/dev/null; then
    echo "FAIL: linuxabi-g2（g2dev 未通过）"; tail -40 "$SER"; exit 1
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
    raise SystemExit("FAIL: linuxabi-g2（连不上 QEMU monitor）")
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

python3 - "$PPM" <<'PY'
import sys
p = sys.argv[1]
d = open(p, 'rb').read()
parts = d.split(b'\n', 3)
if len(parts) != 4 or parts[0] != b'P6':
    raise SystemExit("FAIL: linuxabi-g2（screendump 不是 P6 PPM）")
w, h = map(int, parts[1].split())
px = parts[3]
def pix(x, y):
    o = (y * w + x) * 3
    return tuple(px[o:o+3])
# g2dev 在 RAM 扫描缓冲 (0,0) 写 0x00112233，C6FBIOFLUSH 全屏缩放到面板。
r, g, b = pix(0, 0)
if (r, g, b) != (0x11, 0x22, 0x33):
    raise SystemExit("FAIL: linuxabi-g2（面板像素=(%d,%d,%d)，期望 0x112233）" % (r, g, b))
if w < 640 or h < 480:
    raise SystemExit("FAIL: linuxabi-g2（屏幕尺寸 %dx%d）" % (w, h))
print("PASS: linuxabi-g2（fb0 像素 0x112233 已上屏；%dx%d）" % (w, h))
PY
rm -f "$SER" "$PPM"
