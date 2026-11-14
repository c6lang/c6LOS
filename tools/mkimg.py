#!/usr/bin/env python3
"""自己生成 ESP32-S3 app 镜像（替代 esptool elf2image）。

为什么需要它：elf2image 把 DRAM 段排在镜像最前面，导致 esp_app_desc_t 落不到
文件偏移 0x20，bootloader 校验不过。这里按 IDF 的格式自己排段：
  - 第一段是 DROM(.flash.rodata)，数据正好从文件偏移 0x20 开始（app_desc 要求）
  - flash 段的镜像内偏移 = paddr - BASE，保证与 vaddr 同页对齐
  - RAM 段由 bootloader 拷贝到 vaddr

用法: mkimg.py <elf> <out.bin> [base]   (base 默认 0x20000 = app 分区偏移)
"""
import os, struct, subprocess, sys

ELF  = sys.argv[1]
OUT  = sys.argv[2]
BASE = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x20000
TC   = os.path.expanduser('~/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-')

# (section, paddr 或 None=RAM 段)
SECS = [
    ('.flash.rodata', 0x20020),    # DROM: 数据落文件偏移 0x20
    ('.flash.text',   0x100000),   # IROM: flash 0x100000（64KB 对齐）
    ('.eh_frame',     None),
    ('.vector',       None),
    ('.text',         None),
    ('.data',         None),
]

def sh(args):
    return subprocess.run(args, capture_output=True, text=True)

def section_vaddr(sec):
    r = sh([TC + 'objdump', '-h', ELF])
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 7 and p[1] == sec:
            return int(p[3], 16)
    return None

def extract(sec):
    tmp = '/tmp/_mkimg_%s.bin' % sec.strip('.')
    r = sh([TC + 'objcopy', '-O', 'binary', '--only-section=' + sec, ELF, tmp])
    if r.returncode != 0 or not os.path.exists(tmp):
        return b''
    d = open(tmp, 'rb').read()
    os.unlink(tmp)
    return d

def entry_point():
    for line in sh([TC + 'nm', ELF]).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == '_start':
            return int(p[0], 16)
    raise SystemExit('找不到 _start')

segs = []
for name, paddr in SECS:
    v = section_vaddr(name)
    if v is None:
        continue
    data = extract(name)
    if not data:
        continue
    if paddr is not None:
        v &= ~0xFFFF                     # flash 段 vaddr 必须按 64KB 对齐
    segs.append((name, v, paddr, data))

hdr = bytearray(24)
hdr[0] = 0xE9                            # magic
hdr[1] = len(segs)                       # segment count
hdr[2] = 2                               # DIO
hdr[3] = 0x1F                            # 40MHz | 16MB
struct.pack_into('<I', hdr, 4, entry_point())
hdr[8] = 0xEE                            # wp_pin 禁用
hdr[12:14] = struct.pack('<H', 9)        # chip_id = ESP32-S3
hdr[23] = 0                              # 不附 SHA256

out = bytearray(hdr)
for si, (name, vaddr, paddr, data) in enumerate(segs):
    if paddr is not None:
        off = paddr - BASE
        if off < 0x18 or len(out) > off - 8:
            raise SystemExit('%s: 偏移 0x%x 不合法（当前 0x%x）' % (name, off, len(out)))
        out += b'\x00' * (off - 8 - len(out))
    out += struct.pack('<II', vaddr, len(data))
    out += data
    if si != len(segs) - 1:          # 最后一段不补齐，校验和紧跟其后
        while len(out) % 4:
            out.append(0)

# IDF 要求：镜像末尾附 1 字节 XOR 校验和（初值 0xEF）
chk = 0xEF
for b in out:
    chk ^= b
out.append(chk)

open(OUT, 'wb').write(bytes(out))
print('wrote %s: %d bytes, %d segments, entry 0x%08x' % (OUT, len(out), len(segs), entry_point()))
for name, vaddr, paddr, data in segs:
    print('  %-14s vaddr=0x%08x paddr=%s size=0x%x' %
          (name, vaddr, ('0x%08x' % paddr) if paddr else 'RAM   ', len(data)))
