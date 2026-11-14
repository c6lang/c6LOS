#!/usr/bin/env python3
"""给卡死的 ESP32-S3 USB 设备做一次主机侧端口复位（USBDEVFS_RESET）。

这块板的应用一旦挂死/崩死，USB-Serial/JTAG 会卡成 EPROTO，只能拔插。
其实用 USBDEVFS_RESET 让主机重新复位一次端口就能恢复，不用动线。
用法： sudo -n python3 tools/usbreset.py
"""
import fcntl, os, sys

USBDEVFS_RESET = 0x5514

def find_esp():
    """扫 sysfs 找 idVendor=303a(Espressif)，再用它的 busnum/devnum 组 /dev 路径。"""
    base = '/sys/bus/usb/devices'
    for name in sorted(os.listdir(base)):
        d = os.path.join(base, name)
        try:
            with open(os.path.join(d, 'idVendor')) as f:
                vid = f.read().strip()
            if vid != '303a':
                continue
            with open(os.path.join(d, 'busnum')) as f:
                bus = int(f.read().strip())
            with open(os.path.join(d, 'devnum')) as f:
                dev = int(f.read().strip())
        except (OSError, ValueError):
            continue
        return f'/dev/bus/usb/{bus:03d}/{dev:03d}'
    return None

def main():
    path = find_esp()
    if not path:
        print('没找到 Espressif(303a) USB 设备')
        return 1
    fd = os.open(path, os.O_WRONLY)
    try:
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
        print('USB 端口已复位:', path)
    finally:
        os.close(fd)
    return 0

if __name__ == '__main__':
    sys.exit(main())
