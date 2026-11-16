#!/usr/bin/env python3
"""把目录打成 Linux initramfs 的 newc cpio，并加一个 8 字节长度前缀。

用法：tools/mk-initramfs.py <out.img> <staging-dir>

为什么带长度前缀：cpio 本身没有总长度字段，内核要在读之前知道该从磁盘搬多少。
前缀是 u64 小端，磁盘上紧跟其后才是 cpio 流（内核读 LBA 2049 起）。

格式细节（内核算术依赖它）：
  magic "070701" 之后 13 个 8 字节十六进制字段，
  mode 在偏移 14，filesize 在 54，namesize 在 94，名字从 110 开始，
  名字与数据都按 4 字节对齐。
"""
import os
import struct
import sys


def entry(name: str, ino: int, mode: int, size: int) -> bytes:
    nm = name.encode() + b"\0"
    fields = [ino, mode, 0, 0, 1, 0, size, 0, 0, 0, 0, len(nm), 0]
    hdr = b"070701" + b"".join(b"%08x" % f for f in fields) + nm
    hdr += b"\0" * ((4 - len(hdr) % 4) % 4)
    return hdr


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    out, root = sys.argv[1], sys.argv[2]

    items = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        rel = os.path.relpath(dirpath, root)
        if rel != ".":
            items.append((rel.replace(os.sep, "/"), "dir", dirpath))
        for f in filenames:
            p = os.path.join(dirpath, f)
            items.append(
                (os.path.relpath(p, root).replace(os.sep, "/"), "file", p)
            )
    items.sort(key=lambda t: t[0])

    blobs = []
    ino = 1
    total = 0
    for name, kind, path in items:
        ino += 1
        if kind == "dir":
            blobs.append(entry(name, ino, 0o040755, 0))
        else:
            with open(path, "rb") as fh:
                data = fh.read()
            blobs.append(entry(name, ino, 0o100755, len(data)) + data)
            blobs[-1] += b"\0" * ((4 - len(data) % 4) % 4)
            total += len(data)
    blobs.append(entry("TRAILER!!!", ino + 1, 0, 0))

    body = b"".join(blobs)
    with open(out, "wb") as fh:
        fh.write(struct.pack("<Q", len(body)))
        # 补齐到整扇区：内核从 LBA 2049 直接读 cpio 流，前缀只占第一个扇区。
        fh.write(b"\0" * 504)
        fh.write(body)
    print(
        "initramfs: %d 项, %d 字节文件, 归档 %d 字节 -> %s"
        % (len(items), total, len(body), out)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
