#!/usr/bin/env python3
"""不进入下载模式的干净复位：DTR=0(GPIO0 高) + RTS 拉一下 EN。

esptool 的 USB-UART 复位会故意把 strapping 设成"下载模式"
（串口里能看到 boot:0x0 (DOWNLOAD(USB/UART0)) / waiting for download），
那样芯片根本不会去引导 flash。这个脚本只动 EN，不动 GPIO0。
用法： sudo -n python3 tools/reset.py [port]
"""
import glob, sys, time, termios, os

port = sys.argv[1] if len(sys.argv) > 1 else (sorted(glob.glob('/dev/ttyACM*')) or [None])[0]
if not port:
    print('没有串口')
    sys.exit(1)
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.HUPCL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    # DTR=0 -> GPIO0 高电平（正常引导）；RTS=1 -> EN 拉低 -> 0 释放
    import fcntl, struct
    TIOCMBIS, TIOCMBIC = 0x5416, 0x5417
    TIOCM_DTR, TIOCM_RTS = 0x002, 0x004
    fcntl.ioctl(fd, TIOCMBIC, struct.pack('i', TIOCM_DTR))
    fcntl.ioctl(fd, TIOCMBIS, struct.pack('i', TIOCM_RTS))
    time.sleep(0.15)
    fcntl.ioctl(fd, TIOCMBIC, struct.pack('i', TIOCM_RTS))
    print('已复位:', port)
finally:
    os.close(fd)
