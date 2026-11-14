#!/usr/bin/env python3
"""持续抓串口（自动重连）：刷机/复位导致 USB 重新枚举也不会漏日志。
用法: sudo -n python3 tools/cap.py -t 20
"""
import argparse, fcntl, glob, os, struct, termios, time

TIOCMBIC = 0x5417
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004


def release_dtr_rts(fd):
    """把 DTR/RTS 都放开。

    ESP32-S3 的 USB-Serial/JTAG 用 DTR/RTS 模拟 EN/GPIO0：
    只要 DTR=1（低），芯片在任何复位后都会进**下载模式**（boot:0x0 DOWNLOAD），
    固件永远跑不起来。抓日志时必须让 DTR=0（GPIO0 高），否则等于一直按着 BOOT。
    """
    try:
        fcntl.ioctl(fd, TIOCMBIC, struct.pack('i', TIOCM_DTR))
        fcntl.ioctl(fd, TIOCMBIC, struct.pack('i', TIOCM_RTS))
    except OSError:
        pass

def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[4] = a[5] = termios.B115200
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[0] = 0
    a[1] = 0
    a[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    release_dtr_rts(fd)
    return fd

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-t', type=float, default=15.0)
    a = ap.parse_args()
    end = time.time() + a.t
    # 同时打开所有 ttyACM 节点：复位后常会残留一个僵尸节点，
    # 真数据可能在另一个节点上（ACM0/ACM1 漂移）。
    fds = {}
    out = b''
    while time.time() < end:
        for p in sorted(glob.glob('/dev/ttyACM*')):
            if p not in fds:
                try:
                    fds[p] = open_port(p)
                except OSError:
                    pass
        got = False
        for p, fd in list(fds.items()):
            try:
                d = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                try:
                    os.close(fd)
                except OSError:
                    pass
                del fds[p]
                continue
            if d:
                got = True
                out += d
        if not got:
            time.sleep(0.05)
    for fd in fds.values():
        try:
            os.close(fd)
        except OSError:
            pass
    print(out.decode('utf-8', 'replace'))

if __name__ == '__main__':
    main()
