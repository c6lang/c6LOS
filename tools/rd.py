#!/usr/bin/env python3
"""一次连接读一整段内存，打印成表。

用法： sudo -n python3 tools/rd.py <地址> <字数> [标签...]

为什么要这个脚本：`esptool read-mem` 每次调用都会**先复位芯片再读**，所以
逐个字段读会拿到不同次启动的值，多个字段之间根本对不上。这里一次连接把整段
读回来，得到的是**同一时刻的一致性快照**。

可选的环境变量 RD_PORT 指定串口（默认自动在 ttyACM0/ttyACM1 里找）。
"""
import os
import sys

# tools/ 目录里有个 serial.py，会把 pyserial 的 serial 包顶掉
# （esptool 需要 serial.tools）。导入 esptool 之前先把这两个路径摘掉。
sys.path = [p for p in sys.path
            if p not in ("", ".", os.path.dirname(os.path.abspath(__file__)))]


def find_port():
    p = os.environ.get("RD_PORT")
    if p:
        return p
    for cand in ("/dev/ttyACM0", "/dev/ttyACM1"):
        if os.path.exists(cand):
            return cand
    raise SystemExit("找不到串口（试过 /dev/ttyACM0、/dev/ttyACM1）")


def main():
    if len(sys.argv) < 3:
        raise SystemExit("用法: rd.py <地址> <字数> [标签...]")
    addr = int(sys.argv[1], 0)
    count = int(sys.argv[2], 0)
    labels = sys.argv[3:]

    import esptool

    cls = None
    try:
        from esptool.targets.esp32s3 import ESP32S3ROM as cls
    except Exception:
        cls = esptool.ESP32ROM
    esp = cls(find_port(), 115200)
    try:
        esp.connect()
        # read-mem 需要 stub 固件（CLI 里也是这么做的）
        dev = esp.run_stub() if hasattr(esp, "run_stub") else esp
        if hasattr(dev, "read_mem"):
            data = dev.read_mem(addr, count * 4)
        else:
            data = b""
            for i in range(count):
                dev.mem_begin(4, 1, 4, addr + i * 4)
                data += bytes(dev.mem_block())
                dev.mem_finish()
    finally:
        try:
            esp._port.close()
        except Exception:
            pass

    words = [int.from_bytes(data[i * 4:i * 4 + 4], "little") for i in range(count)]
    for i, w in enumerate(words):
        tag = labels[i] if i < len(labels) else ""
        print("0x%08x = 0x%08x  %10d  %s" % (addr + i * 4, w, w, tag))


if __name__ == "__main__":
    main()
