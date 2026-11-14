#!/bin/bash
# 刷机小工具：自动等串口 + 重试（这块板的 USB-CDC 偶尔会卡死/掉线）。
#   ./tools/flash.sh build/xxx/c6os.bin          # 刷到 0x0
#   ./tools/flash.sh build/xxx/c6os.bin 0x10000  # 刷到 app 分区
# 用了 --after watchdog-reset：这块板上只有它能可靠复位进应用
# （--after hard-reset 经常把芯片留在 bootloader 里）。
BIN=${1:?usage: flash.sh <bin> [addr]}
ADDR=${2:-0x0}
for t in $(seq 1 8); do
  for i in $(seq 1 20); do
    D=$(ls /dev/ttyACM* 2>/dev/null | head -1)
    [ -n "$D" ] && break
    sleep 1
  done
  if [ -n "$D" ] && sudo -n esptool --chip esp32s3 -p "$D" --before default-reset \
       --after watchdog-reset write-flash "$ADDR" "$BIN" 2>&1 | grep -q "Hash of data verified"; then
    echo "flash ok: $BIN -> $ADDR ($D)"
    exit 0
  fi
  sleep 2
  D=""
done
echo "flash FAILED: $BIN"
exit 1
