#!/usr/bin/env python3
"""tools/mkdockeriso.py —— docker 镜像 → 可引导 ISO（M11-3）

做三件事：
  1. **扁平化**镜像的 rootfs：支持三种输入
       --rootfs DIR         已经解开的根文件系统（docker export | tar -x 的产物）
       --from-tar TAR       一个 rootfs tar（同样支持 .wh. whiteout）
       --from-save TAR      docker save 的整包（manifest.json + layer tar，按层叠加）
     whiteout 语义：某层里出现 `.wh.名字` 表示这一层删掉了这个名字；`.wh..wh..opq`
     表示目录内容清空。构建期就消解掉，运行期不需要 overlay。
  2. 往 rootfs 写 `/etc/c6os.cmd`（ENTRYPOINT/CMD 元数据 + 要 exec 的路径）。
  3. 调 tools/mk-initramfs.py 打成 newc cpio，再调 tools/mkiso.py 生成 ISO
     （内核载荷 + rootfs 一起进光盘），顺带用 tools/mkfat.py 造可写数据盘。

用法：
    python3 tools/mkdockeriso.py --rootfs build/x86/iso/docker-root \
        --cmd /bin/app --boot build/x86/cd0.bin --kernel build/x86/iso/flat.bin \
        --data build/x86/docker-data.img -o build/x86/docker.iso
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WH = ".wh."


def _clean(path):
    """把 ./ 前缀、结尾 / 去掉，供 whiteout 匹配"""
    p = path.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    while p.startswith("/"):
        p = p[1:]
    return p.rstrip("/")


def _apply_whiteouts(dest):
    """扁平化阶段：把 .wh. 开头的文件和 .wh..wh..opq 消解掉"""
    removed = 0
    for root, dirs, files in os.walk(dest, topdown=False):
        for name in list(files) + list(dirs):
            full = os.path.join(root, name)
            if name == WH + ".wh..opq":
                # 目录内容清空（保留目录本身）
                for e in os.listdir(root):
                    p = os.path.join(root, e)
                    if p == full:
                        continue
                    if os.path.isdir(p) and not os.path.islink(p):
                        shutil.rmtree(p)
                    else:
                        os.remove(p)
                    removed += 1
                os.remove(full)
                continue
            if name.startswith(WH):
                target = os.path.join(root, name[len(WH):])
                if os.path.isdir(target) and not os.path.islink(target):
                    shutil.rmtree(target)
                elif os.path.exists(target) or os.path.islink(target):
                    os.remove(target)
                os.remove(full)
                removed += 1
    return removed


def _extract_member(tar, member, dest):
    name = _clean(member.name)
    if not name or name.startswith(".."):
        return
    base = os.path.basename(name)
    parent = os.path.dirname(name)
    if base.startswith(WH):
        # whiteout：先记下来，最后统一处理（这里直接处理也行）
        if base == WH + ".wh..opq":
            target_dir = os.path.join(dest, parent)
            if os.path.isdir(target_dir):
                for e in os.listdir(target_dir):
                    p = os.path.join(target_dir, e)
                    if os.path.isdir(p) and not os.path.islink(p):
                        shutil.rmtree(p)
                    else:
                        os.remove(p)
            return
        target = os.path.join(dest, parent, base[len(WH):])
        if os.path.isdir(target) and not os.path.islink(target):
            shutil.rmtree(target)
        elif os.path.exists(target) or os.path.islink(target):
            os.remove(target)
        return
    out = os.path.join(dest, name)
    os.makedirs(os.path.dirname(out) or dest, exist_ok=True)
    if member.isdir():
        os.makedirs(out, exist_ok=True)
        return
    if member.issym() or member.islnk():
        # 符号链接一律摊平成普通文件内容（没有 link() 也没有真正的符号链接语义）
        try:
            src = member.linkname
            if member.islnk():
                src = os.path.join(dest, _clean(member.linkname))
                with open(src, "rb") as f:
                    data = f.read()
            else:
                data = member.linkname.encode()
        except OSError:
            return
        with open(out, "wb") as f:
            f.write(data)
    else:
        f = tar.extractfile(member)
        if f is None:
            return
        with open(out, "wb") as o:
            shutil.copyfileobj(f, o)
    mode = member.mode & 0o777
    if mode:
        os.chmod(out, mode)


def from_rootfs(dest):
    if not os.path.isdir(dest):
        sys.exit("mkdockeriso: --rootfs 不是目录：%s" % dest)
    n = _apply_whiteouts(dest)
    return n


def from_tar(dest, tar_path):
    with tarfile.open(tar_path) as tar:
        for m in tar.getmembers():
            _extract_member(tar, m, dest)
    return _apply_whiteouts(dest)


def from_save(dest, tar_path):
    """docker save 的整包：manifest.json 里有 Config，Config 里有 Entrypoint/Cmd；
    层文件在 manifest 的 Layers 里，按顺序叠加（后面的层覆盖前面的）。"""
    entry = []
    cmd = []
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(tar_path) as tar:
            tar.extractall(tmp)
        man = os.path.join(tmp, "manifest.json")
        layers = []
        if os.path.exists(man):
            with open(man) as f:
                data = json.load(f)
            if data:
                cfg = os.path.join(tmp, data[0].get("Config", ""))
                if os.path.exists(cfg):
                    with open(cfg) as f:
                        c = json.load(f)
                    conf = c.get("config", {})
                    entry = conf.get("Entrypoint") or []
                    cmd = conf.get("Cmd") or []
                for l in data[0].get("Layers", []):
                    p = os.path.join(tmp, l)
                    if os.path.exists(p):
                        layers.append(p)
        if not layers:
            # 没有 manifest 就退化成"把整包当一个 rootfs tar"
            from_tar(dest, tar_path)
            return entry, cmd
        for p in layers:
            from_tar(dest, p)
    return entry, cmd


def run(cmd):
    print("  $ " + " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.exit("mkdockeriso: 子命令失败（%d）：%s" % (rc, " ".join(cmd)))


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--rootfs", help="已经解开的根文件系统目录")
    src.add_argument("--from-tar", help="rootfs tar")
    src.add_argument("--from-save", help="docker save 的整包 tar")
    ap.add_argument("--cmd", default="", help="要 exec 的命令行（默认取镜像的 Entrypoint+Cmd）")
    ap.add_argument("--boot", required=True, help="cd0.bin")
    ap.add_argument("--kernel", required=True, help="内核载荷 flat.bin")
    ap.add_argument("--data", default="", help="顺带生成的可写数据盘（FAT16）")
    ap.add_argument("--data-mb", type=int, default=8)
    ap.add_argument("--rootfs-dst", default="0x00090000",
                    help="rootfs 载入地址；M11-3 的镜像很小，放 1MB 以下最省事。"
                         "注意别选 0x31670~0x72684——那是内核 .bss（_start64 会清）")
    ap.add_argument("-o", "--out", required=True, help="输出 ISO")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="c6docker-")
    dest = os.path.join(tmp, "rootfs")
    os.makedirs(dest)
    entry = []
    cmd = []
    try:
        if args.rootfs:
            shutil.copytree(args.rootfs, dest, symlinks=True, dirs_exist_ok=True)
            wh = from_rootfs(dest)
            print("  扁平化：目录输入，消解 whiteout %d 处" % wh)
        elif args.from_tar:
            wh = from_tar(dest, args.from_tar)
            print("  扁平化：单层 tar，消解 whiteout %d 处" % wh)
        else:
            entry, cmd = from_save(dest, args.from_save)
            print("  扁平化：docker save（%d 层），Entrypoint=%s Cmd=%s"
                  % (len(os.listdir(dest)) and 0 or 0, entry, cmd))

        if not args.cmd:
            args.cmd = " ".join(list(entry) + list(cmd)) or "/bin/sh"
        os.makedirs(os.path.join(dest, "etc"), exist_ok=True)
        with open(os.path.join(dest, "etc", "c6os.cmd"), "w") as f:
            f.write(args.cmd + "\n")

        cpio = os.path.join(tmp, "rootfs.cpio")
        run([sys.executable, os.path.join(HERE, "mk-initramfs.py"), cpio, dest])
        rdst = int(args.rootfs_dst, 0)
        csz = os.path.getsize(cpio)
        if rdst < 0x100000 and rdst + csz > 0x0009F000:
            sys.exit("mkdockeriso: rootfs 太大（%d B）放不进 1MB 以下的空档，"
                     "用 --rootfs-dst 0x00800000 走 unreal mode" % csz)
        iso_cmd = [sys.executable, os.path.join(HERE, "mkiso.py"),
                   "--boot", args.boot, "--kernel", args.kernel,
                   "--rootfs", cpio, "--rootfs-dst", args.rootfs_dst,
                   "-o", args.out]
        run(iso_cmd)
        if args.data:
            # mkfat.py 的第二个参数是 KB（不是字节，也不是扇区）
            run([sys.executable, os.path.join(HERE, "mkfat.py"), args.data,
                 str(args.data_mb * 1024)])
        print("mkdockeriso: 镜像入口 = %s" % args.cmd)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
