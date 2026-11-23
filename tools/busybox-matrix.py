#!/usr/bin/env python3
"""把 check-alpine-busybox 的串口日志整理成 applet 矩阵并判定成败。

用法：tools/busybox-matrix.py <log> <baseline.tsv>

日志里认这几种行（其余行忽略；串口日志前面有内核/装载的输出）：

  XA|<applet>|<rc>       /bin/busybox <applet> --help            （argv[1] 派发）
  XB|<applet>|<rc>       <rootfs 里的绝对路径> --help            （argv[0] 派发）
  OK|<name>              F 段一条功能验证通过
  NO|<name>|rc=<n>       F 段一条功能验证失败
  FB|<name>|<rc>         /data FAT16 盘的只读/写状态（只报告，不判负）

baseline.tsv 是宿主 Linux 上用 qemu-user 跑同一份 Alpine minirootfs 的
busybox 得到的 `applet<TAB>期望 rc`。

判定口径：

* XA（argv[1] 派发）**逐个与 baseline 比**：同一份二进制在真 Linux 上什么
  rc，在本内核上也该是什么 rc。差异要么是内核缺口，要么进 XA_WAIVERS。
* XB（argv[0] 派发）**只查"跑没跑起来"**：missing 或 rc=126/127 才算失败。
  argv[0] 这条路会走到 busybox 多路复用器的 `--help` 分支，和 argv[1] 那条
  路的帮助处理本来就不同（例如 `[` 走 XB 会打印 usage/exit 0，而走 XA 是
  "missing ]"/exit 2），拿 baseline 硬比会制造噪声；XB 与 XA 的 rc 差异只列
  出来看，不判负。
* F 段按 OK/NO 判：不在 F_WAIVERS 里的 NO 就是失败（rc=124 是 timeout 兜底，
  单独标"超时"，说明该 applet 在本内核上挂住）。
* FB 行是 /data FAT16 盘的状态，属于内核文件系统栈，单列报告、不参与判定。

新增 applet 或修好缺口时，改 tests/alpine/busybox-sweep.sh 与这边的豁免
清单即可。
"""
import collections
import re
import sys

XA_RE = re.compile(r'^XA\|([^|]+)\|(-?\d+)$')
XB_RE = re.compile(r'^XB\|([^|]+)\|(-?\d+)$')
OK_RE = re.compile(r'^OK\|([^|]+)$')
NO_RE = re.compile(r'^NO\|([^|]+)\|rc=(.*)$')
FB_RE = re.compile(r'^FB\|([^|]+)\|(-?\d+)$')

# 已知差异（A 段 XA：argv[1] 派发）。键是 applet 名，值是差异原因。
XA_WAIVERS = {
    # baseline 侧不可靠：qemu-user 会把 ioctl(TIOCGWINSZ) 伪装成成功，
    # 同一份 busybox 在真 Linux 上对非 tty 的 stdout 返回 0；本内核按
    # ENOTTY 如实失败（rc=2）。ttysize 不涉及派发，归为基线噪声。
    'ttysize': 'baseline 用 qemu-user 假成功；本内核 ENOTTY 如实报错（环境相关）',
    # 重跑两次都是 2：readahead(2) 没实现，busybox 走 perror+exit(2)。
    'readahead': 'readahead(2) 未实现（确定性缺口，非偶发）',
}

# 已知缺口（F 段：功能性验证）。键是检查名，值写清原因。
# 只收**已经定位过**的缺口：每一条都在 docs/busybox-alpine-report.md §4.2/§5
# 里有现象与原因。新增失败不在表里 → 直接判负，防止缺口被悄悄豁免。
F_WAIVERS = {
    # stat 没把 mode 落到输出（Access 行缺失/不符）
    'chmod-mode': 'stat 未报告 mode（chmod 600 后 grep 0600 不中）',
    # ramfs 符号链接解析
    'ln-read': 'ramfs 上 ln -s 后 cat 链接 rc=1（符号链接解析缺口）',
    # 文本工具单项
    'uniq': 'uniq rc=1（sort/sort -u 等同类工具均正常）',
    'uniq-c': 'uniq -c rc=1（同上）',
    'strings': 'strings /bin/busybox 无输出',
    'xargs': 'xargs -n1 echo 输出不符',
    # 进程/信号
    'top': 'top -b -n 1 挂住（timeout 15 拉不回，见报告 §6）',
    'pstree': 'pstree 输出与进程树不符',
    'pidof': 'pidof 查不到进程（/proc/<pid> 数据不全）',
    'pgrep': 'pgrep 查不到进程（同上）',
    'kill-l': 'kill -l 信号表不全（只列出前几个）',
    # timeout 语义：子进程退出没被及时回收，时间一到就杀
    'timeout': 'timeout 2 echo ok 返回 143（子进程退出未被 wait4 及时看到）',
    'timeout-124': 'timeout 1 sleep 5 返回 143 而非 124（同上）',
    # tar 家族挂住
    'tar-c': 'tar -cf 挂住（15s 超时）',
    'tar-t': 'tar -tf 挂住（同上）',
    'tar-x': 'tar -xf 挂住（同上）',
    'tar-extracted': 'tar -xf 没跑完，解包文件不存在（连带）',
    'mkdir-untar': '解包目录残留（tar-x 挂住的连带）',
    # gzip/bzip2
    'gzip-c': 'gzip -c 失败（修完 fd 引用后由挂住变 rc=1）',
    'gzip-file': 'gzip 输出文件为空',
    'gzip-dc': 'gzip -dc rc=1',
    'bzip2-c': 'bzip2 -c rc=1',
    'bzip2-file': 'bzip2 输出文件为空',
    'bzip2-dc': 'bzip2 -dc rc=1',
    # /proc/net 与 /proc/<pid>/fd
    'lsof': 'lsof 无输出（/proc/<pid>/fd 未实现）',
    'ifconfig': 'ifconfig 输出与期望不符（/proc/net/dev 不全）',
    'netstat': 'netstat rc=1（/proc/net/tcp 等未实现）',
    'route': 'route rc=1（/proc/net/route 未实现）',
    'nslookup': 'nslookup rc=1（无 DNS 配置/网络）',
    'mpstat': 'mpstat rc=1（/proc/stat 缺 CPU 明细）',
}

EXPECTED_APPLETS = 304


def parse_log(path):
    xa = collections.OrderedDict()
    xb = collections.OrderedDict()
    ok = []
    no = collections.OrderedDict()
    fb = collections.OrderedDict()
    seen = {'alive': False, 'done': False, 'a_done': False, 'f_done': False}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if line == 'bx-sweep-alive':
                seen['alive'] = True
                continue
            if line == 'bx-sweep-done':
                seen['done'] = True
                continue
            if line == '# A-DONE':
                seen['a_done'] = True
                continue
            if line == '# F-DONE':
                seen['f_done'] = True
                continue
            m = XA_RE.match(line)
            if m:
                xa.setdefault(m.group(1), int(m.group(2)))
                continue
            m = XB_RE.match(line)
            if m:
                xb.setdefault(m.group(1), int(m.group(2)))
                continue
            m = OK_RE.match(line)
            if m:
                ok.append(m.group(1))
                continue
            m = NO_RE.match(line)
            if m:
                no.setdefault(m.group(1), m.group(2).strip())
                continue
            m = FB_RE.match(line)
            if m:
                fb.setdefault(m.group(1), int(m.group(2)))
    return xa, xb, ok, no, fb, seen


def load_baseline(path):
    base = collections.OrderedDict()
    with open(path, 'r', encoding='utf-8') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            applet, rc = line.split('\t')
            base[applet] = int(rc)
    return base


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    log, baseline = sys.argv[1], sys.argv[2]
    xa, xb, ok, no, fb, seen = parse_log(log)
    base = load_baseline(baseline)

    print('=== busybox applet 矩阵（Alpine 3.20 minirootfs，baseline 来自真 Linux）===')
    if not seen['alive']:
        print('FAIL: 引导块没跑到扫描脚本（缺 bx-sweep-alive）')
        return 1
    if not seen['a_done']:
        print('FAIL: A 段没跑完（缺 # A-DONE）')
    print('A 段 applet 总数: 期望 %d，XA %d，XB %d%s'
          % (EXPECTED_APPLETS, len(xa), len(xb),
             '' if seen['a_done'] else '（未跑完）'))

    bad = 0
    waived = 0
    diffs = []
    for applet, want in base.items():
        have = xa.get(applet)
        if have is None:
            diffs.append((applet, want, 'missing'))
            continue
        if have == want:
            continue
        if applet in XA_WAIVERS:
            waived += 1
            continue
        diffs.append((applet, want, have))
    print('XA（argv[1] 派发）: 一致 %d/%d%s'
          % (len(base) - len(diffs) - waived, len(base),
             '' if not diffs else '，差异 %d' % len(diffs)))
    for applet, want, have in diffs:
        print('    %-16s 期望 rc=%s 实际 %s' % (applet, want, have))
    bad += len(diffs)

    # XB：只看有没有真的执行起来
    xb_bad = []
    for applet in base:
        have = xb.get(applet)
        if have is None:
            xb_bad.append((applet, 'missing'))
        elif have in (126, 127):
            xb_bad.append((applet, have))
    xb_diff = [(a, r, xa[a]) for a, r in xb.items()
               if a in xa and r != xa[a] and r not in (126, 127)]
    print('XB（argv[0] 派发）: 执行成功 %d/%d%s'
          % (len(base) - len(xb_bad), len(base),
             '' if not xb_bad else '，失败 %d' % len(xb_bad)))
    for applet, have in xb_bad:
        print('    %-16s rc=%s' % (applet, have))
    bad += len(xb_bad)
    if xb_diff:
        print('    （XB 与 XA 的 rc 差异 %d 个，属帮助/分支差异，不判负）: %s'
              % (len(xb_diff),
                 ', '.join('%s XB=%s XA=%s' % (a, r, x) for a, r, x in xb_diff[:8])))

    total_f = len(ok) + len(no)
    fail = [(n, r) for n, r in no.items() if n not in F_WAIVERS]
    waived_f = [n for n in no if n in F_WAIVERS]
    timeouts = [n for n, r in no.items() if r == '124']
    print('F 段功能验证: OK %d / %d%s'
          % (len(ok), total_f, '' if not fail else '，失败 %d' % len(fail)))
    for name, rc in fail:
        print('    %-16s rc=%s%s' % (name, rc,
                                     '  ← 挂住（timeout 兜底）' if rc == '124' else ''))
    bad += len(fail)

    if waived or waived_f:
        print('豁免（已知缺口，不计入失败）: %d 个 applet 差异, %d 条功能验证'
              % (waived, len(waived_f)))
        for applet in sorted(a for a in XA_WAIVERS if a in xa or a in base):
            print('    applet %-14s %s' % (applet, XA_WAIVERS[applet]))
        for name in waived_f:
            print('    check  %-14s %s' % (name, F_WAIVERS.get(name, '')))

    if fb:
        print('/data FAT16 盘（只报告，不判负）:')
        for name, rc in fb.items():
            print('    %-22s rc=%s' % (name, rc))

    if not seen['f_done']:
        print('FAIL: F 段没跑完（缺 # F-DONE）')
        bad += 1
    if not seen['done']:
        print('FAIL: 扫描没跑到底（缺 bx-sweep-done）')
        bad += 1

    if bad == 0 and seen['a_done'] and seen['f_done'] and seen['done']:
        print('PASS: alpine busybox（A 段 %d applet × 2 派发形式 + F 段 %d 条功能验证）'
              % (len(base), total_f))
        return 0
    print('FAIL: alpine busybox（%d 处失败，详见上面清单）' % bad)
    return 1


if __name__ == '__main__':
    sys.exit(main())