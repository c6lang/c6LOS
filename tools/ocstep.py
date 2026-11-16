#!/usr/bin/env python3
"""用 OpenOCD 的 telnet 口（4444）直接驱动 JTAG：下硬件断点 + 单步。

为什么要这个（docs/wifi-plan.md §257/§261/§264）：这块板子上 xtensa-gdb 一执行
`continue` / `stepi` 就报 "Cannot execute this command while the target is running"，
`set non-stop off` 也没用 —— 等于**没法单步**。但 OpenOCD 自己的 telnet 接口
（`bp` / `resume` / `stepi` / `reg`）是好用的。闭源 WiFi 库里崩在哪里，只能靠它。

先起 openocd（与 tools/dbg.sh 相同的参数，但留在后台）：
    ./tools/dbg.sh /dev/null /dev/null   # 不行，dbg.sh 会杀进程
    tools/ocboot.sh                      # 见下（本文件不负责起 openocd）
或者手工：
    $(ls -d ~/.espressif/tools/openocd-esp32/*/openocd-esp32/bin/openocd) \
        -f board/esp32s3-builtin.cfg >/tmp/oc.log 2>&1 &

用法:
    tools/ocstep.py reset                        # 复位后放行
    tools/ocstep.py regs                         # halt 后读关键寄存器
    tools/ocstep.py bp 0x4203d594 step 400       # 断在该地址，然后最多单步 400 条
    tools/ocstep.py bp 0x4203d594 regs           # 只断住读现场

输出只打关键行，方便直接读。
"""
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 4444
IAC = 0xFF

REGS_SHORT = ("pc", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a8", "a10",
              "a12", "ps")
REGS_ALL = ("pc", "ps", "windowbase", "windowstart", "a0", "a1", "a2", "a3",
            "a4", "a5", "a6", "a7", "a8", "a9", "a10", "a11", "a12", "a13",
            "a14", "a15")


class Oc:
    def __init__(self, host=HOST, port=PORT, timeout=60.0):
        self.s = socket.create_connection((host, port), timeout=5.0)
        self.s.settimeout(timeout)
        self.drain(0.6)

    def _read_some(self, timeout):
        self.s.settimeout(timeout)
        try:
            return self.s.recv(65536)
        except socket.timeout:
            return b""

    @staticmethod
    def _strip(data):
        """去掉 telnet 的 IAC 序列，留下可读文本。"""
        out = bytearray()
        i = 0
        while i < len(data):
            b = data[i]
            if b == IAC and i + 1 < len(data):
                cmd = data[i + 1]
                if cmd in (0xFB, 0xFC, 0xFD, 0xFE) and i + 2 < len(data):
                    i += 3
                else:
                    i += 2
                continue
            out.append(b)
            i += 1
        return out.decode("utf-8", "replace").replace("\r", "")

    def drain(self, timeout=0.3):
        out = b""
        while True:
            chunk = self._read_some(timeout)
            if not chunk:
                break
            out += chunk
            timeout = 0.08
        return self._strip(out)

    def cmd(self, line, wait=0.3):
        self.s.sendall((line + "\n").encode())
        time.sleep(wait)
        text = self.drain(0.15)
        lines = [ln for ln in text.split("\n")
                 if ln.strip() and ln.strip() != ">"]
        if lines and lines[0].strip() == line:
            lines = lines[1:]
        return "\n".join(lines).strip()

    def wait_for(self, needle, timeout=40.0):
        deadline = time.time() + timeout
        acc = ""
        low = needle.lower()
        while time.time() < deadline:
            chunk = self._strip(self._read_some(0.3))
            if chunk:
                acc += chunk
                if low in acc.lower():
                    return acc
        return acc

    def reg(self, name):
        text = self.cmd("reg " + name, wait=0.10)
        for tok in text.replace("\n", " ").split():
            if tok.startswith("0x"):
                try:
                    return int(tok, 16)
                except ValueError:
                    pass
        return None

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def fmt(v):
    return "-" if v is None else "0x%08x" % (v & 0xFFFFFFFF)


def dump_regs(oc, names=REGS_SHORT):
    for name in names:
        print("    %-4s %s" % (name, fmt(oc.reg(name))))


def in_code(pc):
    return 0x40000000 <= pc < 0x42100000


def do_step(oc, count):
    hist = []
    for i in range(count):
        oc.s.sendall(b"stepi\n")
        chunk = oc.wait_for("halted", timeout=20.0)
        if "halted" not in chunk.lower():
            print("!! 第 %d 步没停下来: %s" % (i, chunk[-200:]))
            break
        pc_new = oc.reg("pc")
        if pc_new is None:
            print("!! 第 %d 步读不到 pc" % i)
            break
        hist.append(pc_new)
        if i < 8 or i % 40 == 0:
            print("  step %4d pc=%s" % (i, fmt(pc_new)))
        if not in_code(pc_new):
            print("*** 第 %d 步跳飞了: pc=%s" % (i, fmt(pc_new)))
            dump_regs(oc)
            break
    print("最后若干条 PC：")
    for v in hist[-16:]:
        print("   %s" % fmt(v))
    return hist


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    oc = Oc()
    try:
        # 这块芯片是双核：cpu1 的 examine 会失败（"Unexpected OCD_ID = 00000000"，
        # 没人给它上电/放时钟），必须显式选 cpu0 才能读寄存器。
        oc.cmd("targets esp32s3.cpu0", wait=0.25)
        mode = argv[1]

        if mode == "reset":
            oc.cmd("halt", wait=0.5)
            print(oc.cmd("reset halt", wait=2.0)[-300:])
            oc.cmd("resume", wait=0.4)
            print("reset+resume 完成")
            return 0

        if mode == "regs":
            oc.cmd("halt", wait=0.6)
            dump_regs(oc, REGS_ALL)
            return 0

        if mode == "bp":
            addr = int(argv[2], 16)
            action = argv[3] if len(argv) > 3 else "regs"
            count = int(argv[4]) if len(argv) > 4 else 200

            oc.cmd("reset halt", wait=2.2)   # 从头跑一遍，保证崩点是可复现的那次
            print("bp:", oc.cmd("bp 0x%08x 1" % addr, wait=0.4))
            oc.cmd("resume", wait=0.3)
            out = oc.wait_for("halted", timeout=90.0)
            if "halted" not in out.lower():
                print("!! 没等到断点，最后输出：")
                print(out[-400:])
                return 1
            pc = oc.reg("pc")
            print("命中: pc=%s" % fmt(pc))

            if action == "regs":
                dump_regs(oc, REGS_ALL)
                return 0

            if action == "step":
                do_step(oc, count)
                return 0

        # gdb 已经用 `hbreak` 下好硬件断点（telnet 的 bp 只能下软件断点，而闭源库
        # 在 flash 里，软件断点写不进去）。这里只负责放行 + 等停机 + 单步。
        if mode == "runstep":
            count = int(argv[2]) if len(argv) > 2 else 200
            oc.cmd("resume", wait=0.3)
            out = oc.wait_for("halted", timeout=120.0)
            if "halted" not in out.lower():
                print("!! 没等到停机，最后输出：")
                print(out[-400:])
                return 1
            pc = oc.reg("pc")
            print("停下来: pc=%s" % fmt(pc))
            dump_regs(oc)
            do_step(oc, count)
            return 0

        # armstep：唯一可行的"真板断点"姿势。
        # 这块板子的 USB-Serial-JTAG 和串口是同一个 USB：esptool 一复位，OpenOCD 的
        # 连接就断（"Target disconnected"），所以"先下断点再复位"不可行；而 OpenOCD
        # 自己的 reset 又起不来我们的 app（只跑到 port_idle，g_lwip_stage 全 0）。
        # 于是：先 tools/flashmon.sh 真复位，再立刻 ocboot.sh，然后 halt→写
        # ibreaka0/ibreakenable→resume，赶在库里第一次深调用之前把断点架上。
        if mode == "armstep":
            addr = int(argv[2], 16)
            count = int(argv[3]) if len(argv) > 3 else 200
            oc.cmd("halt", wait=0.6)
            oc.cmd("reg ibreaka0 0x%08x" % addr, wait=0.25)
            oc.cmd("reg ibreakenable 1", wait=0.25)
            print("ibreaka0=%s enable=%s" % (fmt(oc.reg("ibreaka0")),
                                             fmt(oc.reg("ibreakenable"))))
            oc.cmd("resume", wait=0.25)
            out = oc.wait_for("halted", timeout=60.0)
            if "halted" not in out.lower():
                print("!! 断点没命中（app 可能已经跑到别处或崩了）")
                return 1
            pc = oc.reg("pc")
            print("命中断点: pc=%s" % fmt(pc))
            dump_regs(oc, REGS_ALL)
            if count > 0:
                do_step(oc, count)
            return 0

        print(__doc__)
        return 2
    finally:
        oc.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
