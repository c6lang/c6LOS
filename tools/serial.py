#!/usr/bin/env python3
"""USB-Serial/JTAG 抓串口 + 可选发送（S3 真板联调）。

用法：
  sudo -n python3 tools/serial.py -t 3                     # 抓 3 秒
  sudo -n python3 tools/serial.py -t 5 --send 'tick\r'     # 1 秒后发一行
  sudo -n python3 tools/serial.py -t 5 --send 'ps\r' --send 'free\r' --send-gap 1.0
  sudo -n python3 tools/serial.py -t 3 --hex               # 原始字节

说明：本板 RESET 会让 USB 掉线不恢复，抓日志前用拔插 USB / esptool hard-reset。
"""
import argparse
import fcntl
import os
import struct
import sys
import termios
import time

DTR_RTS = 0x006


def open_port(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0            # iflag
    attrs[1] = 0            # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0            # lflag
    attrs[4] = baud
    attrs[5] = baud
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    fcntl.ioctl(fd, 0x5416, struct.pack("i", DTR_RTS))
    return fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=os.environ.get("PORT", "/dev/ttyACM0"))
    ap.add_argument("-t", "--time", type=float, default=3.0, help="抓取秒数")
    ap.add_argument("--send", action="append", default=[], help="要发送的行，可重复")
    ap.add_argument("--send-after", type=float, default=1.0, help="第一行发送延迟（秒）")
    ap.add_argument("--send-gap", type=float, default=0.5, help="相邻两行间隔（秒）")
    ap.add_argument("--hex", action="store_true", help="输出原始字节")
    args = ap.parse_args()

    fd = open_port(args.port, termios.B115200)
    start = time.time()
    pending = list(args.send)
    next_send = start + args.send_after
    out = b""
    try:
        while time.time() - start < args.time:
            now = time.time()
            if pending and now >= next_send:
                line = pending.pop(0)
                if not line.endswith("\r"):
                    line += "\r"
                os.write(fd, line.encode())
                sys.stdout.write("\n[send] %r\n" % line)
                sys.stdout.flush()
                next_send = now + args.send_gap
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            except OSError as exc:            # USB 掉线
                sys.stdout.write("\n[serial gone] %s\n" % exc)
                break
            if not chunk:
                continue
            out += chunk
            if args.hex:
                sys.stdout.write(" ".join("%02x" % b for b in chunk) + "\n")
            else:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    finally:
        os.close(fd)
    sys.stdout.write("\n[bytes=%d]\n" % len(out))


if __name__ == "__main__":
    main()
