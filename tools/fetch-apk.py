#!/usr/bin/env python3
"""tools/fetch-apk.py —— 往 Alpine rootfs 里装额外的 apk 包（M64：curl）

为什么不用 apk 自己装：宿主上不一定有 apk，非 root 跑 `apk add --root` 又会
在 chown 上折腾；而 curl 需要的整条依赖闭包（libcurl / OpenSSL / c-ares /
brotli / zstd / nghttp2 / idn2 / psl / unistring …）都只是"解包几个 tar.gz"。
这里就做这一件事，让构建期离线可用、无需 root：

  1. 从 `<base>/<repo>/x86_64/APKINDEX.tar.gz` 读索引（repo 默认 main+community）；
  2. 按 `D:`（依赖）字段递归展开闭包，`so:xxx` 通过索引里的 `p:`（provides）
     反查提供者；
  3. minirootfs 里已经装过的包（`/lib/apk/db/installed`）**不覆盖**——musl /
     OpenSSL / zlib 保持镜像自带版本，只补缺的；
  4. 下载缺失的 .apk（缓存在 `--cache`，文件名带版本，CDN 上的文件不可变），
     直接解包进 `--root`。

apk 文件格式（v2）：一个 gzip tar，成员里直接是 `usr/bin/curl` 之类的文件，
外加 `.PKGINFO` / `.SIGN.*` 两个元数据成员。新版 tar 会把 apk 的
`APK-TOOLS.checksum.SHA1` 当未知 PAX 头告警，Python 的 tarfile 不会受影响。

用法：
    tools/fetch-apk.py --root build/x86/alpine/root \\
        --cache build/x86/alpine/pkgs \\
        --base https://dl-cdn.alpinelinux.org/alpine/v3.20 \\
        [--upgrade-installed] [--marker build/x86/alpine/root/.extra-pkgs] \\
        curl

退出码：0 成功；非 0 表示索引/下载/解包有硬错误（缺依赖库之类只告警）。
"""
import argparse
import io
import os
import sys
import tarfile
import urllib.request

UA = "c6os-fetch-apk/1 (+https://github.com/c6os)"


def warn(msg):
    print("  apk: " + msg, file=sys.stderr)


def fetch(url, timeout=60, retries=3):
    last = None
    for _ in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except Exception as e:  # noqa: BLE001 —— 换一次重试，最后统一报错
            last = e
    raise RuntimeError("下载失败 %s: %s" % (url, last))


def parse_index(text):
    """APKINDEX 是一串空行分隔的 key:value 块；key 只有一个字符。"""
    pkgs = []
    cur = {}
    for line in text.splitlines() + [""]:
        if not line:
            if cur.get("P"):
                pkgs.append(cur)
            cur = {}
            continue
        if len(line) > 1 and line[1] == ":":
            cur[line[0]] = line[2:]
    return pkgs


def parse_provides(pkg):
    """把 p: 字段拆成 ['libfoo.so.1', ...]（去掉 '=版本'）。"""
    out = []
    for tok in pkg.get("p", "").split():
        out.append(split_ver(tok))
    return out


def split_ver(tok):
    for op in ("=", ">", "<", "~"):
        i = tok.find(op)
        if i > 0:
            return tok[:i]
    return tok


class Index:
    def __init__(self):
        self.pkgs = {}          # 包名 -> 字段字典（同名的后出现的覆盖前面的）
        self.provider = {}      # provides 名 -> 包名
        self.source = {}         # 包名 -> repo 名

    def add(self, repo, text):
        for p in parse_index(text):
            name = p["P"]
            self.pkgs[name] = p
            self.source[name] = repo
            self.provider[name] = name      # 包名本身也是 provides
            for pr in parse_provides(p):
                self.provider.setdefault(pr, name)

    def resolve(self, roots):
        """返回依赖闭包（包名 -> 字段），按包名排序以保证稳定输出。"""
        out = {}
        stack = [(r, "需求") for r in roots]
        while stack:
            tok, why = stack.pop()
            name = tok
            if tok.startswith("so:") or tok.startswith("cmd:"):
                name = self.provider.get(tok)
                if name is None:
                    warn("找不到 %s 的提供者（来自 %s）" % (tok, why))
                    continue
            elif tok.startswith("!"):
                continue                    # 冲突声明，不是依赖
            elif tok.startswith("/"):
                continue                    # 文件依赖（如 /bin/sh），根 fs 自带
            if name in out:
                continue
            p = self.pkgs.get(name)
            if p is None:
                warn("索引里没有包 %s（来自 %s）" % (name, why))
                continue
            out[name] = p
            for d in p.get("D", "").split():
                stack.append((split_ver(d), name))
        return out


def read_installed(root):
    """rootfs 自带的 /lib/apk/db/installed：包名 -> 版本。"""
    path = os.path.join(root, "lib/apk/db/installed")
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return out
    for blk in text.split("\n\n"):
        name = ver = None
        for line in blk.splitlines():
            if line.startswith("P:"):
                name = line[2:].strip()
            elif line.startswith("V:"):
                ver = line[2:].strip()
        if name:
            out[name] = ver or ""
    return out


def extract_apk(path, root):
    """只解 data 成员：跳过 .PKGINFO / .SIGN.*，其余按 tar 类型落到 root。"""
    n = 0
    with tarfile.open(path, "r:gz") as tf:
        for m in tf:
            name = m.name
            if name.startswith("./"):
                name = name[2:]
            if not name or name.split("/")[0].startswith("."):
                continue
            parts = name.split("/")
            if ".." in parts or name.startswith("/"):
                warn("跳过可疑路径 %s" % name)
                continue
            dst = os.path.join(root, name)
            parent = os.path.dirname(dst)
            if parent:
                os.makedirs(parent, exist_ok=True)
            if m.isdir():
                os.makedirs(dst, exist_ok=True)
                continue
            if os.path.islink(dst) or os.path.exists(dst):
                if os.path.isdir(dst) and not os.path.islink(dst):
                    # 包里的文件撞上已有目录：不删目录，写不进去就告警
                    warn("路径冲突（目录）%s" % name)
                    continue
                os.unlink(dst)
            if m.issym():
                os.symlink(m.linkname, dst)
            elif m.islnk():
                linkname = m.linkname
                if linkname.startswith("./"):
                    linkname = linkname[2:]
                src = os.path.join(root, linkname)
                if os.path.exists(src):
                    os.link(src, dst)
                else:
                    warn("硬链接目标不存在：%s -> %s" % (name, m.linkname))
                    continue
            elif m.isfile():
                data = tf.extractfile(m)
                if data is None:
                    continue
                with open(dst, "wb") as out:
                    out.write(data.read())
                os.chmod(dst, (m.mode & 0o777) or 0o755)
            else:
                warn("跳过特殊文件 %s（mode %o）" % (name, m.mode))
                continue
            n += 1
    return n


def marker_text(base, roots):
    return "base %s\npkgs %s\n" % (base, " ".join(roots))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="Alpine rootfs 目录")
    ap.add_argument("--cache", required=True, help=".apk 缓存目录")
    ap.add_argument("--base", required=True,
                    help="仓库前缀，如 https://dl-cdn.alpinelinux.org/alpine/v3.20")
    ap.add_argument("--repos", default="main,community")
    ap.add_argument("--upgrade-installed", action="store_true",
                    help="连 minirootfs 里已有的包也覆盖（默认保留自带版本）")
    ap.add_argument("--marker", default=None,
                    help="已装清单标记文件；内容一致时直接返回（幂等）")
    ap.add_argument("--skip", default=None,
                    help="逗号分隔的包名，永远不装（调试用）")
    ap.add_argument("roots", nargs="*", help="要安装的包（如 curl）")
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        warn("rootfs 目录不存在：%s" % args.root)
        return 1

    text = marker_text(args.base, args.roots)
    if args.marker:
        try:
            with open(args.marker, "r", encoding="utf-8") as fh:
                if fh.read() == text:
                    return 0
        except OSError:
            pass

    idx = Index()
    for repo in [r for r in args.repos.split(",") if r]:
        url = "%s/%s/x86_64/APKINDEX.tar.gz" % (args.base, repo)
        try:
            raw = fetch(url)
        except Exception as e:  # noqa: BLE001
            warn("索引拉取失败（%s）：%s" % (repo, e))
            continue
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as tf:
            m = tf.extractfile("APKINDEX")
            if m is None:
                warn("%s 的 APKINDEX 里没有 APKINDEX 成员" % repo)
                continue
            idx.add(repo, m.read().decode("utf-8", "replace"))

    if not idx.pkgs:
        warn("一个包的索引都没拿到")
        return 1

    closure = idx.resolve(args.roots)
    installed = read_installed(args.root)
    skip = {s for s in (args.skip or "").split(",") if s}

    os.makedirs(args.cache, exist_ok=True)
    todo = []
    for name, p in sorted(closure.items()):
        if name in skip:
            print("  apk: 跳过 %s（--skip）" % name)
            continue
        if name in installed and not args.upgrade_installed:
            print("  apk: 已有 %s %s（保留 rootfs 自带版本）" % (name, installed[name]))
            continue
        todo.append((name, p))

    total = 0
    for name, p in todo:
        ver = p.get("V", "?")
        repo = idx.source[name]
        fname = "%s-%s.apk" % (name, ver)
        dst = os.path.join(args.cache, fname)
        url = "%s/%s/x86_64/%s" % (args.base, repo, fname)
        if not os.path.exists(dst) or os.path.getsize(dst) == 0:
            print("  apk: 下载 %s" % fname)
            try:
                data = fetch(url)
            except Exception as e:  # noqa: BLE001
                warn("包下载失败 %s：%s" % (fname, e))
                return 1
            tmp = dst + ".part"
            with open(tmp, "wb") as fh:
                fh.write(data)
            os.replace(tmp, dst)
        n = extract_apk(dst, args.root)
        size = int(p.get("S", 0))
        total += size
        print("  apk: 装入 %-18s %-14s %8d B  %3d 项  (%s)"
              % (name, ver, size, n, repo))

    if args.marker:
        with open(args.marker, "w", encoding="utf-8") as fh:
            fh.write(text)
    print("  apk: 新增 %d 个包, %d 字节（闭包共 %d 个）"
          % (len(todo), total, len(closure)))
    return 0


if __name__ == "__main__":
    sys.exit(main())