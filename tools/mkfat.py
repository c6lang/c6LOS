#!/usr/bin/env python3
"""造一个最小 FAT16 镜像（M9-3 的验收输入）。

宿主没有 mtools（`mcopy`），所以这里直接按 FAT16 规范拼一个固定布局的卷：
  - 512B/扇区、1 扇区/簇、2 个 FAT、512 个根目录项
  - 根目录里一个 8.3 文件 `HELLO.TXT`，内容 `hello fat\\n`
  - 核内只读实现，所以镜像写死、可复现（每次生成字节一致）

用法：python3 tools/mkfat.py <out.img> [size_kb]
"""
import struct
import sys

BPS = 512
SPC = 1
RESERVED = 1
NFATS = 2
ROOT_ENTS = 512
MEDIA = 0xF8
FILE_NAME = "HELLO   "
FILE_EXT = "TXT"
FILE_DATA = b"hello fat\n"


def solve(size_kb: int) -> dict:
    total = size_kb * 2                       # 512B 扇区数
    root_sectors = (ROOT_ENTS * 32 + BPS - 1) // BPS
    fatsz = 1
    for _ in range(64):
        clusters = total - (RESERVED + NFATS * fatsz + root_sectors)
        need = ((clusters + 2) * 2 + BPS - 1) // BPS
        if need == fatsz:
            break
        fatsz = need
    clusters = total - (RESERVED + NFATS * fatsz + root_sectors)
    if clusters > 65524:
        raise SystemExit("镜像太大：FAT16 最多 65524 个簇")
    return {
        "total": total,
        "root_sectors": root_sectors,
        "fatsz": fatsz,
        "clusters": clusters,
        "data_start": RESERVED + NFATS * fatsz + root_sectors,
    }


def dir_entry(name8: str, ext3: str, attr: int, cluster: int, size: int) -> bytes:
    b = bytearray(32)
    b[0:8] = name8.encode("ascii")
    b[8:11] = ext3.encode("ascii")
    b[11] = attr
    struct.pack_into("<H", b, 26, cluster & 0xFFFF)
    struct.pack_into("<I", b, 28, size)
    return bytes(b)


def build(size_kb: int = 4096) -> bytes:
    lay = solve(size_kb)
    img = bytearray(lay["total"] * BPS)

    # ---- 引导扇区（BPB）----
    bs = bytearray(BPS)
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"C6OSFAT0"
    struct.pack_into("<H", bs, 11, BPS)
    bs[13] = SPC
    struct.pack_into("<H", bs, 14, RESERVED)
    bs[16] = NFATS
    struct.pack_into("<H", bs, 17, ROOT_ENTS)
    struct.pack_into("<H", bs, 19, lay["total"])
    bs[21] = MEDIA
    struct.pack_into("<H", bs, 22, lay["fatsz"])
    struct.pack_into("<H", bs, 24, 32)
    struct.pack_into("<H", bs, 26, 64)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0xC6051F5A)
    bs[43:54] = b"C6OS VFS   "
    bs[54:62] = b"FAT16   "
    bs[510] = 0x55
    bs[511] = 0xAA
    img[0:BPS] = bs

    # ---- FAT（两份一样）----
    fat = bytearray(lay["fatsz"] * BPS)
    struct.pack_into("<H", fat, 0, 0xFFF8)        # 介质描述
    struct.pack_into("<H", fat, 2, 0xFFFF)        # 保留
    struct.pack_into("<H", fat, 4, 0xFFFF)        # 簇 2：单簇文件的结束
    for k in range(NFATS):
        off = (RESERVED + k * lay["fatsz"]) * BPS
        img[off:off + len(fat)] = fat

    # ---- 根目录 ----
    root_off = (RESERVED + NFATS * lay["fatsz"]) * BPS
    img[root_off:root_off + 32] = dir_entry(
        FILE_NAME, FILE_EXT, 0x20, 2, len(FILE_DATA))

    # ---- 数据区（簇 2）----
    data_off = lay["data_start"] * BPS
    img[data_off:data_off + len(FILE_DATA)] = FILE_DATA
    return bytes(img)


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: mkfat.py <out.img> [size_kb]", file=sys.stderr)
        return 2
    out = sys.argv[1]
    size_kb = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
    data = build(size_kb)
    with open(out, "wb") as f:
        f.write(data)
    print(f"mkfat: {out} {len(data)} 字节（FAT16, HELLO.TXT, {size_kb}KB）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
