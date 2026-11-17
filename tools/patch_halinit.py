#!/usr/bin/env python3
"""把闭源 WiFi 库 hal_init() 里的硬件握手轮询改成直接向下走。

真板（§504/§505）观察到 ppTask 在 hal_init+0x1a 处死循环：

    l32r  a2, =0x60033d14      // WiFi MAC 寄存器块
    or    a3, a3, 2
    s32i  a3, a2, 0            // 写 bit1（kick）
1:  l32i  a3, a2, 0
    bbci  a3, 0, 1b            // 等 bit0 置位 —— 我们的环境里永不发生

0x60033d14 是 write-only kick 寄存器（JTAG 写 3 读回仍是 0），时钟和复位
都已打开（WIFI_CLK_EN=0xfffce7bf、WIFI_RST_EN=0）。绕过这次握手后，
WiFi 立刻走到 `start=0` / `set_ps=0` / `scan start`，说明这是唯一卡点。

在最终 bin 上把 3 字节 `bbci a3,0,<loop>` 换成等价长度的 `j .+3`
（汇编器生成，字节 c6 ff ff），其余指令和 ELF 均不动。
只在模式完全匹配时打补丁，避免掩盖别的布局变化。
"""
import re
import subprocess
import sys

BIN, ELF = sys.argv[1], sys.argv[2]
ORIG = bytes.fromhex("0763f7")   # bbci a3,0,<loop>  (objdump: f76307)
PATCH = bytes.fromhex("c6ffff")  # j .+3             (objdump: ffffc6)
VADDR_BIAS = 0x42000000          # bin 偏移 = vaddr - 0x42000000
INSTR_OFF = 0x1A                 # spin 在 hal_init 内的偏移

try:
    out = subprocess.check_output(["nm", ELF], text=True, stderr=subprocess.DEVNULL)
except Exception:
    sys.exit(0)
m = re.search(r"^\s*([0-9a-fA-F]+)\s+T\s+hal_init\s*$", out, re.M)
if not m:
    sys.exit(0)
off = int(m.group(1), 16) - VADDR_BIAS + INSTR_OFF
try:
    data = bytearray(open(BIN, "rb").read())
except OSError:
    sys.exit(0)
if off < 0 or off + 3 > len(data):
    sys.exit(0)
cur = bytes(data[off:off + 3])
if cur == PATCH:
    sys.exit(0)
if cur != ORIG:
    print("patch_halinit: unexpected bytes at %#x: %s" % (off, cur.hex()), file=sys.stderr)
    sys.exit(1)
data[off:off + 3] = PATCH
open(BIN, "wb").write(data)
print("patch_halinit: bypassed hal_init MAC handshake spin at bin offset %#x" % off)
