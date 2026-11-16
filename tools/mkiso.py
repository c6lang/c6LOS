#!/usr/bin/env python3
"""tools/mkiso.py —— 生成可引导 ISO9660（El Torito no-emulation），M11-0

为什么要自己写：我们只要"极小 ISO9660 + El Torito"，而且要求 **LBA 布局确定** ——
引导块按固定 LBA 直接读载荷，这样内核侧不需要 ISO9660 解析器，也不需要 ATAPI 驱动。
风格与 tools/mkfat.py 一致（同为构建期小工具，不引入第三方依赖）。

布局（1 块 = 2048 字节）：

    LBA 0..15   系统区（全 0）
    16          PVD
    17          El Torito Boot Record VD
    18          Volume Descriptor Set Terminator
    19          Boot Catalog
    20          引导块（cd0.bin，2048 字节）
    21..        内核载荷 flat.bin（按块补齐）
    ..          rootfs.cpio（可选）
    ..          Type L 路径表 / Type M 路径表
    ..          根目录（. / .. / C6OS.KER[;1] / ROOTFS.CPI[;1]）

用法：
    python3 tools/mkiso.py --boot build/x86/cd0.bin --kernel build/x86/iso/flat.bin \\
        [--rootfs build/x86/iso/rootfs.cpio] [-o build/x86/iso/c6os.iso]
"""

import argparse
import struct
import sys

BLOCK = 2048
BOOT_MAGIC = b"C6ISOLDR"
KERNEL_DST = 0x00010000
ROOTFS_DST = 0x00800000


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def date7():
    # 年(1900 起) 月 日 时 分 秒 时区（15 分钟为单位）
    return bytes([125, 1, 1, 0, 0, 0, 0])


def date17():
    return b"2025010100000000" + bytes([0])


def dirrec(extent, size, is_dir, name):
    n = len(name)
    ln = 33 + n
    if ln % 2:
        ln += 1
    b = bytearray(ln)
    b[0] = ln
    b[1] = 0
    b[2:10] = both32(extent)
    b[10:18] = both32(size)
    b[18:25] = date7()
    b[25] = 0x02 if is_dir else 0x00
    b[26] = 0
    b[27] = 0
    b[28:32] = both16(1)
    b[32] = n
    b[33:33 + n] = name
    return bytes(b)


def pvd(total_blocks, root_lba, path_l, path_m, path_size):
    b = bytearray(BLOCK)
    b[0] = 1
    b[1:6] = b"CD001"
    b[6] = 1
    for s, txt in ((8, "C6OS"), (40, "C6OS-M11")):
        v = txt.encode("ascii")
        b[s:s + len(v)] = v
        b[s + len(v):s + 32] = b" " * (32 - len(v))
    b[80:88] = both32(total_blocks)
    b[120:124] = both16(1)
    b[124:128] = both16(1)
    b[128:132] = both16(BLOCK)
    b[132:140] = both32(path_size)
    b[140:144] = struct.pack("<I", path_l)
    b[144:148] = struct.pack("<I", 0)
    b[148:152] = struct.pack(">I", path_m)
    b[152:156] = struct.pack(">I", 0)
    b[156:156 + 34] = dirrec(root_lba, BLOCK, True, b"\x00")
    for s, n in ((190, 128), (318, 128), (446, 128), (574, 128),
                 (702, 37), (739, 37), (776, 37)):
        b[s:s + n] = b" " * n
    b[813:830] = date17()
    b[830:847] = date17()
    b[847:864] = b"0" * 16 + bytes([0])
    b[864:881] = b"0" * 16 + bytes([0])
    b[881] = 1
    return bytes(b)


def boot_record(catalog_lba):
    b = bytearray(BLOCK)
    b[0] = 0
    b[1:6] = b"CD001"
    b[6] = 1
    # 规范要求用 0 补齐（不是空格）——SeaBIOS 认这个字符串才肯走光盘引导
    b[7:7 + 32] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    b[71:75] = struct.pack("<I", catalog_lba)
    return bytes(b)


def terminator():
    b = bytearray(BLOCK)
    b[0] = 255
    b[1:6] = b"CD001"
    b[6] = 1
    return bytes(b)


def boot_catalog(boot_lba, sector_count=4, load_seg=0x07C0):
    val = bytearray(32)
    val[0] = 0x01
    val[1] = 0x00
    val[4:4 + 12] = b"c6LOS  M11  "
    val[30] = 0x55
    val[31] = 0xAA
    s = 0
    for i in range(0, 32, 2):
        s += val[i] | (val[i + 1] << 8)
    ck = (-s) & 0xFFFF
    val[28:30] = struct.pack("<H", ck)
    ent = bytearray(32)
    ent[0] = 0x88
    ent[1] = 0x00
    ent[2:4] = struct.pack("<H", load_seg)
    ent[4] = 0x00
    ent[5] = 0x00
    ent[6:8] = struct.pack("<H", sector_count)
    ent[8:12] = struct.pack("<I", boot_lba)
    b = bytearray(BLOCK)
    b[0:32] = bytes(val)
    b[32:64] = bytes(ent)
    return bytes(b)


def path_table(root_lba, big):
    fmt4 = ">I" if big else "<I"
    fmt2 = ">H" if big else "<H"
    b = bytearray(10)
    b[0] = 1
    b[1] = 0
    b[2:6] = struct.pack(fmt4, root_lba)
    b[6:8] = struct.pack(fmt2, 1)
    b[8] = 0x00
    b[9] = 0x00
    return bytes(b)


def find_magic(boot):
    i = boot.find(BOOT_MAGIC)
    if i < 0:
        print("mkiso: 提示：引导块里没有 C6ISOLDR 魔数，跳过参数回填", file=sys.stderr)
        return -1
    if i + 32 > len(boot):
        sys.exit("mkiso: 引导块魔数后面不够 24 字节")
    return i + len(BOOT_MAGIC)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", required=True, help="cd0.bin（<=2048 字节）")
    ap.add_argument("--kernel", required=True, help="内核载荷 flat.bin")
    ap.add_argument("--rootfs", default=None, help="rootfs.cpio（可选）")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    boot = bytearray(open(args.boot, "rb").read())
    kernel = open(args.kernel, "rb").read()
    rootfs = open(args.rootfs, "rb").read() if args.rootfs else b""
    if len(boot) > BLOCK:
        sys.exit("mkiso: 引导块超过 2048 字节（%d）" % len(boot))
    if not kernel:
        sys.exit("mkiso: 内核载荷是空的")
    boot += b"\x00" * (BLOCK - len(boot))

    ker_blocks = (len(kernel) + BLOCK - 1) // BLOCK
    root_blocks = (len(rootfs) + BLOCK - 1) // BLOCK

    lba_pvd = 16
    lba_brvd = 17
    lba_term = 18
    lba_cat = 19
    lba_boot = 20
    lba_kernel = lba_boot + 1
    lba_rootfs = lba_kernel + ker_blocks
    lba_path_l = lba_rootfs + root_blocks
    lba_path_m = lba_path_l + 1
    lba_rootdir = lba_path_m + 1
    total = lba_rootdir + 1

    off = find_magic(boot)
    if off > 0:
        struct.pack_into("<IIIIII", boot, off,
                         lba_kernel, ker_blocks, KERNEL_DST,
                         lba_rootfs, root_blocks, ROOTFS_DST if rootfs else 0)

    # 根目录：. / .. / 文件
    ents = dirrec(lba_rootdir, BLOCK, True, b"\x00")
    ents += dirrec(lba_rootdir, BLOCK, True, b"\x01")
    ents += dirrec(lba_kernel, len(kernel), False, b"C6OS.KER;1")
    if rootfs:
        ents += dirrec(lba_rootfs, len(rootfs), False, b"ROOTFS.CPI;1")
    if len(ents) > BLOCK:
        sys.exit("mkiso: 根目录超过 1 块")

    pt = path_table(lba_rootdir, False)
    pm = path_table(lba_rootdir, True)

    img = bytearray(total * BLOCK)
    img[lba_pvd * BLOCK:(lba_pvd + 1) * BLOCK] = pvd(total, lba_rootdir,
                                                    lba_path_l, lba_path_m, len(pt))
    img[lba_brvd * BLOCK:(lba_brvd + 1) * BLOCK] = boot_record(lba_cat)
    img[lba_term * BLOCK:(lba_term + 1) * BLOCK] = terminator()
    img[lba_cat * BLOCK:(lba_cat + 1) * BLOCK] = boot_catalog(lba_boot)
    img[lba_boot * BLOCK:(lba_boot + 1) * BLOCK] = boot
    img[lba_kernel * BLOCK:lba_kernel * BLOCK + len(kernel)] = kernel
    if rootfs:
        img[lba_rootfs * BLOCK:lba_rootfs * BLOCK + len(rootfs)] = rootfs
    img[lba_path_l * BLOCK:lba_path_l * BLOCK + len(pt)] = pt
    img[lba_path_m * BLOCK:lba_path_m * BLOCK + len(pm)] = pm
    img[lba_rootdir * BLOCK:lba_rootdir * BLOCK + len(ents)] = ents

    with open(args.out, "wb") as f:
        f.write(img)

    # 自检：把生成结果重新解析一遍关键字段
    chk = open(args.out, "rb").read()
    assert chk[lba_pvd * BLOCK] == 1 and chk[lba_pvd * BLOCK + 1:lba_pvd * BLOCK + 6] == b"CD001"
    assert chk[lba_brvd * BLOCK + 7:lba_brvd * BLOCK + 9] == b"EL"
    assert struct.unpack_from("<I", chk, lba_brvd * BLOCK + 71)[0] == lba_cat
    assert chk[lba_cat * BLOCK] == 1 and chk[lba_cat * BLOCK + 30] == 0x55
    assert struct.unpack_from("<I", chk, lba_cat * BLOCK + 32 + 8)[0] == lba_boot
    assert chk[lba_kernel * BLOCK:lba_kernel * BLOCK + len(kernel)] == kernel

    print("iso: %s" % args.out)
    print("  块大小 2048，总块数 %d（%.1f KB）" % (total, total * BLOCK / 1024.0))
    print("  PVD=%d  BRVD=%d  TERM=%d  CAT=%d  BOOT=%d" %
          (lba_pvd, lba_brvd, lba_term, lba_cat, lba_boot))
    print("  kernel: lba=%d blocks=%d (%d B) -> 0x%08X" %
          (lba_kernel, ker_blocks, len(kernel), KERNEL_DST))
    if rootfs:
        print("  rootfs: lba=%d blocks=%d (%d B) -> 0x%08X" %
              (lba_rootfs, root_blocks, len(rootfs), ROOTFS_DST))
    else:
        print("  rootfs: 无")
    print("  pathL=%d pathM=%d rootdir=%d" % (lba_path_l, lba_path_m, lba_rootdir))


if __name__ == "__main__":
    main()
