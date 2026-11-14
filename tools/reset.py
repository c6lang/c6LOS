#!/usr/bin/env python3
"""不进入下载模式的干净复位（和 esptool 内部的 ClassicReset 时序一致）。

esptool 的 USB-UART 复位会顺带把 strapping 设成下载模式
（串口里能读到 `boot:0x0 (DOWNLOAD(USB/UART0))` / `waiting for download`），
那样芯片根本不会去引导 flash；这个脚本只拉 EN，DTR 始终为 0（GPIO0 高）。

用法： sudo -n python3 tools/reset.py [port]
"""
import glob
import sys
import time

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else (sorted(glob.glob('/dev/ttyACM*')) or [None])[0]
    if not port:
        print('没有串口')
        return 1
    sp = serial.Serial(port, 115200, timeout=0.1)
    try:
        sp.dtr = False        # GPIO0 = 高（正常引导）
        sp.rts = True         # EN = 低（复位）
        time.sleep(0.2)
        sp.dtr = False
        sp.rts = False        # EN = 高（释放，开始引导 flash）
        print('已复位:', port)
    finally:
        sp.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
