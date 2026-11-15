#!/usr/bin/env python3
"""校验 ESP32-S3 镜像里 "flash 段在文件中的偏移 == 它的 LMA" 这条不变量。

为什么必须校验（docs/wifi-plan.md §202 真板实测）：
  - 我们直接由 ROM 引导（没有二级 bootloader）。ROM 给 flash 段（DROM/IROM）
    建 MMU 映射时，用的是段头里的 load addr（= 链接脚本给的 LMA）>>16 当物理页号；
  - 而 esptool 排布镜像文件时是按自己的规则摆放段数据（flash 段一律 0x10000 对齐，
    从第 3 段起就落在文件偏移 0x10000）。
  - 两者不相等时，0x42xxxxxx 取指会取到 flash 里**别的地方**的字节：现象是
    "从 flash 执行必炸"、IllegalInstruction、或者跳到地址 1/2 这类怪 PC。

判据很直接：ELF 里每个 PT_LOAD 段（尤其 p_paddr 落在 0x3Cxxxxxx/0x42xxxxxx 这些
flash 窗口里的）的字节，必须原封不动地出现在 .bin 的 p_paddr 偏移处。

用法: tools/chkimg.py <elf> <bin>
退出码 0 = 通过；1 = 不一致（附诊断）。
"""
import struct
import sys

DROM_LO = 0x3C000000          # DROM 窗口（0x3C000000..0x3E000000）
DROM_HI = 0x3E000000
IROM_LO = 0x42000000          # IROM 窗口（0x42000000..0x44000000）
IROM_HI = 0x44000000
PAGE = 0x10000


def load_phdrs(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF':
        raise SystemExit('不是 ELF: %s' % path)
    if d[4] != 1:            # ELFCLASS32
        raise SystemExit('只支持 32 位 ELF')
    e_phoff = struct.unpack('<I', d[28:32])[0]
    e_phentsize = struct.unpack('<H', d[42:44])[0]
    e_phnum = struct.unpack('<H', d[44:46])[0]
    out = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack('<8I', d[off:off + 32])
        if p_type != 1:      # PT_LOAD
            continue
        out.append((p_offset, p_vaddr, p_paddr, p_filesz))
    return d, out


def main():
    if len(sys.argv) != 3:
        raise SystemExit('用法: %s <elf> <bin>' % sys.argv[0])
    elf_path, bin_path = sys.argv[1], sys.argv[2]
    elf, phdrs = load_phdrs(elf_path)
    with open(bin_path, 'rb') as f:
        img = f.read()

    bad = 0
    checked = 0
    for p_offset, p_vaddr, p_paddr, p_filesz in phdrs:
        if p_filesz == 0:
            continue
        # 判据看 **VMA**（p_vaddr）：flash 段就是 VMA 落在 DROM/IROM 窗口里的段。
        # 它们的 p_paddr 是 LMA（也就是"应该在 flash 的哪个偏移"）。
        if not (DROM_LO <= p_vaddr < DROM_HI or IROM_LO <= p_vaddr < IROM_HI):
            continue
        checked += 1
        kind = 'IROM' if p_vaddr >= IROM_LO else 'DROM'
        if (p_vaddr - p_paddr) % PAGE != 0:
            print('!! %s: VMA 0x%08x 与 LMA 0x%08x 不是 mod 0x10000 同余（MMU 页 64KB）'
                  % (kind, p_vaddr, p_paddr))
            bad += 1
        if p_paddr + p_filesz > len(img):
            print('!! %s: LMA 0x%08x + 长度 0x%x 超过镜像大小 0x%x'
                  % (kind, p_paddr, p_filesz, len(img)))
            bad += 1
            continue
        want = elf[p_offset:p_offset + p_filesz]
        got = img[p_paddr:p_paddr + p_filesz]
        if want != got:
            first = next(i for i in range(p_filesz) if want[i] != got[i])
            print('!! %s: 镜像里 flash 偏移 0x%08x 处的字节和 ELF 不一致（首个差异 +0x%x）'
                  % (kind, p_paddr, first))
            print('   ELF: %s' % want[max(0, first - 4):first + 8].hex())
            print('   BIN: %s' % got[max(0, first - 4):first + 8].hex())
            print('   → 说明段数据实际被 esptool 放在别的文件偏移，而 ROM 会按 LMA')
            print('     建 MMU 映射。修法：调大 link.ld 里该段的 AT()/VMA，让 LMA 等于')
            print('     `esptool image-info` 里该段的 File offs + 8；或用 SHADOW_BIN')
            print('     /SHADOW_OFF 把这段原始字节补写到 flash 的 LMA 处。')
            bad += 1

    if checked == 0:
        print('chkimg: 没有 flash 段（普通镜像），跳过')
        return 0
    if bad:
        print('chkimg: 失败（%d 处问题）' % bad)
        return 1
    print('chkimg: OK（%d 个 flash 段，文件偏移 == LMA）' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
