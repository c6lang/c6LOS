#!/usr/bin/env python3
"""自己生成 ESP32-S3 app 镜像（替代 esptool elf2image）。

为什么需要它：elf2image 把 DRAM 段排在镜像最前面，导致 esp_app_desc_t 落不到
文件偏移 0x20，bootloader 校验不过。这里按 IDF 的格式自己排段：
  - 第一段是 DROM(.flash.rodata)，数据正好从文件偏移 0x20 开始（app_desc 要求）
  - flash 段的镜像内偏移 = paddr - BASE，保证与 vaddr 同页对齐
  - RAM 段由 bootloader 拷贝到 vaddr

用法: mkimg.py <elf> <out.bin> [base]   (base 默认 0x20000 = app 分区偏移)
"""
import hashlib, os, struct, subprocess, sys

ELF  = sys.argv[1]
OUT  = sys.argv[2]
BASE = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x20000
TC   = os.path.expanduser('~/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-')

# (section, paddr 或 None=RAM 段)
SECS = [
    ('.flash.rodata', None),   # 运行时按 BASE+0x20 计算    # DROM: 数据落文件偏移 0x20（app_desc 要求）
    ('.flash.text',   None),   # 运行时按 BASE+0x10000 计算
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
    if paddr is None and name == '.flash.rodata':
        paddr = BASE + 0x20
    elif paddr is None and name == '.flash.text':
        paddr = BASE + 0x1020
    if os.environ.get('MKIMG_NO_FLASH') and paddr is not None:
        continue    # 实验：整个镜像不含 flash 段
    v = section_vaddr(name)
    if v is None:
        continue
    data = extract(name)
    if not data:
        continue
    # 注意：flash 段的 vaddr 保持 ELF 里的原值。它低 16 位必须等于该段数据在
    # 镜像文件里的偏移低 16 位（bootloader 用这个关系推导 flash 物理地址并建映射）。
    segs.append((name, v, paddr, data))

hdr = bytearray(24)
hdr[0] = 0xE9                            # magic
hdr[1] = len(segs)                       # segment count
hdr[2] = int(os.environ.get("MKIMG_SPI_MODE", "2"))  # 0=QIO 1=QOUT 2=DIO 3=DOUT
hdr[3] = 0x40                            # 高半字节=容量(4=16MB) 低半字节=速度(0=40MHz)
struct.pack_into('<I', hdr, 4, entry_point())
hdr[8] = 0xEE                            # wp_pin 禁用
hdr[12:14] = struct.pack('<H', 9)        # chip_id = ESP32-S3
hdr[15:17] = struct.pack('<H', 0x0063)   # min/max chip rev（照抄 IDF app）
hdr[23] = 0                              # 不附 SHA256

# 先把段排好（段表必须连续；flash 段前的空白并入上一段的数据）
records = []                          # [vaddr, bytearray]
for name, vaddr, paddr, data in segs:
    if paddr is not None:
        need = paddr - BASE           # 这段数据在镜像内的偏移（实测这套布局能引导）
        cur = 24 + sum(8 + len(d) for _v, d in records)
        if cur > need - 8:
            raise SystemExit('%s: 需要偏移 0x%x，已用到 0x%x' % (name, need, cur))
        gap = need - 8 - cur
        if gap:
            if not records:
                raise SystemExit('%s: 第一段前不能有空白' % name)
            records[-1][1].extend(b'\x00' * gap)
    d = bytearray(data)
    while len(d) % 4:                 # IDF 风格：补齐计入段长度（原厂各段长度都是 4 的倍数）
        d.append(0)
    records.append([vaddr, d])

out = bytearray(hdr)
for vaddr, d in records:
    out += struct.pack('<II', vaddr, len(d))
    out += d

# 最后一段之后补到 16 字节边界，再放 1 字节校验和（esptool/IDF 会先对齐再读校验和）
# 校验和本身占 1 字节，且整个镜像必须是 16 的倍数 —— 所以补到 (len+1)%16==0
while (len(out) + 1) % 16:
    out.append(0)
    records[-1][1].append(0)

# IDF 校验和：对所有段数据 XOR（初值 0xEF，含补齐）
chk = 0xEF
for _v, d in records:
    for b in d:
        chk ^= b
out.append(chk)

# 原厂镜像是 hash_appended=1 + 末尾 32 字节 SHA-256（覆盖到校验和字节为止）。
# 我们也照做，去掉这一处结构差异。
hdr[0x17] = 1
out[0x17] = 1
out += hashlib.sha256(bytes(out)).digest()

open(OUT, 'wb').write(bytes(out))
print('wrote %s: %d bytes, %d segments, entry 0x%08x' % (OUT, len(out), len(records), entry_point()))
for vaddr, d in records:
    print('  vaddr=0x%08x size=0x%x' % (vaddr, len(d)))
