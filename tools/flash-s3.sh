#!/bin/sh
# 等待 ESP32-S3 进入下载模式并烧写。用法：tools/flash-s3.sh [bin]
# 进入下载模式：按住 BOOT(IO0)，点一下 RESET(EN)，松开 BOOT。
set -e
BIN=${1:-build/s3/c6os.bin}
PORT=${PORT:-/dev/ttyACM0}

[ -f "$BIN" ] || { echo "missing $BIN"; exit 1; }
echo "waiting for $PORT (hold BOOT, tap RESET)..."
i=0
while [ ! -e "$PORT" ]; do
    i=$((i + 1))
    if [ $((i % 30)) -eq 0 ]; then echo "still waiting..."; fi
    sleep 1
done
sleep 1
for try in 1 2 3 4 5; do
    echo "flash attempt $try"
    if sudo -n esptool --chip esp32s3 --port "$PORT" --before no-reset \
        --after hard-reset write-flash 0x0 "$BIN"; then
        echo "flash ok"
        exit 0
    fi
    sleep 2
done
echo "flash failed: is the board in download mode (BOOT held at reset)?"
exit 1
