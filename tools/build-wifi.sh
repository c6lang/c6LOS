#!/bin/bash
# c6LOS WiFi 构建/上板脚本（固化 §1566 交接总结里的最优配置）
# 用法：
#   bash tools/build-wifi.sh build     只用默认配置构建
#   bash tools/build-wifi.sh best      用当前最优（前沿）配置构建 + 上板 + 抓取
set -e
cd "$(dirname "$0")/.."
export LD_LIBRARY_PATH=/home/winger/c6/tcc:$LD_LIBRARY_PATH

BASE_CDEFS="-DS3_INJECT_IDF_CAL -DS3_SCAN_CHAN39 -DS3_RC_SEED -DS3_RC_DRIVE \
-DS3_RC_HANDOFF -DS3_ONE_ABI -DS3_SKIP_FLASH_INIT -DS3_STA_CHANNEL6 \
-DS3_TICK_DEFER -DS3_TICK_IN_ISR -DS3_TICK_TIMEOUT_SCAN -DS3_HOME_EARLY \
-DS3_PM_ATTACH -DS3_PMOFF_FORCE -DS3_SCAN_CB_FIX -DS3_SCAN_CB_SWEEP \
-DS3_PM_WAKE2 -DS3_TIMER_DISPATCH -DS3_SCAN_BITMAP -DS3_SWEEP_PRINT \
-DS3_YIELD_IN_GUARD -DS3_SPILL_SHELL -DS3_PHYQUEUE_FIX -DS3_REAL_ASSOC_GATE"

# 前沿配置：把"能走到扫描状态机"的全部开关都打开（见 docs/wifi-plan.md §1566）
# 前沿：已把 IC 初始化真正走通（§1598-1602）+ 各路闸门开关
FRONTIER_CDEFS="$BASE_CDEFS -DS3_QFALLBACK -DS3_RECV_CWRAP -DS3_IC16_FILL \
-DS3_SETCH_DIRECT -DS3_SCAN_FLAG_FIX -DS3_NO_TCPIP_TASK -DS3_NO_DIRECT_PPATTACH \
-DS3_PHY_CAL_MODE=2 \
-DS3_PREFILL_MENUCONFIG -DS3_RXBUF_DIRECT -DS3_PREP_DESC_CNT"
C6FLAGS="-DS3_NO_WDT -DS3_NO_MON -DS3_ONE_ABI -DS3_SKIP_FLASH_INIT -DS3_TICK_DEFER"

make C6_ROOT=/home/winger/c6 S3_ABI=windowed S3_APP_BASE=0x20000 \
  S3_NO_PREEMPT=1 S3_TARGET_BSSID=1 \
  S3_CDEFS="${FRONTIER_CDEFS}" S3_C6FLAGS="${C6FLAGS}" https \
  2>&1 | grep -iE "overflow|collision|error|错误|undefined" || true

if [ ! -f build/wifi/c6os.bin ]; then
    echo "构建失败：build/wifi/c6os.bin 不存在"
    exit 1
fi
echo "构建成功：$(ls -l build/wifi/c6os.bin | awk '{print $5}') 字节"

if [ "$1" = "best" ]; then
    pkill -x openocd 2>/dev/null || true
    sleep 1
    esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
        --before default-reset --after hard-reset \
        write-flash 0x20000 build/wifi/c6os.bin >/dev/null 2>&1
    echo "已刷写，开始抓取（190s）..."
    timeout 200 python3 tools/cap.py -t 190 > /tmp/cap-best.txt 2>&1 || true
    echo "抓取完成：/tmp/cap-best.txt（$(wc -l < /tmp/cap-best.txt) 行）"
    echo "关键指标："
    grep -oE "irq=[0-9]+" /tmp/cap-best.txt | tail -1
    grep -E "\[rxblk\]|\[eb\]|scan_get_ap_num" /tmp/cap-best.txt | tail -3 || true
fi
