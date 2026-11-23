#!/usr/bin/env python3
"""统计 IDF 闭源 WiFi 库**实际调用**了 wifi_osi_funcs_t 的哪些槽位。

为什么要这个（docs/wifi-plan.md §203）：我们的 port/s3/wifi_glue.c 把 OSI 表里
大部分槽位先填成"返回 0 的空实现"（glue_ret0），只有内存/队列/互斥/任务/日志是真的。
到底哪些空实现是**致命的**、哪些是**无所谓**的，靠猜没意义 —— 直接把闭源 .a 反汇编，
看它们从 `g_osi_funcs_p` 里取了哪些偏移量再 `callx8` 出去，就能得到确凿清单。

用法:
    tools/osislots.py [--libdir DIR] [--limit N]
默认 libdir = /media/winger/inst_26/esp/esp-idf/components/esp_wifi/lib/esp32s3
槽位名字直接读 include/c6os/wifi_osi.h（按字段声明顺序），所以名字永远和 ABI 同步。

原理：对每条 `callx8 aN`，往回最多 40 条指令找"最近一条写 aN 的指令"；如果它是
`l32i aN, aBase, off` 且 off 是 4 的倍数、落在表大小内，就记一次 (off/4)。
参数准备会插在取函数指针和 callx8 之间，所以往回找比往前找可靠。
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADER = os.path.join(ROOT, 'include', 'c6os', 'wifi_osi.h')
DEFAULT_LIBDIR = '/media/winger/inst_26/esp/esp-idf/components/esp_wifi/lib/esp32s3'
OBJDUMP = os.path.expanduser(
    '~/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump')


def slot_names():
    """从 wifi_osi.h 里按声明顺序取字段名。"""
    txt = open(HEADER).read()
    body = txt.split('typedef struct wifi_osi_funcs_t {', 1)[1].split('}', 1)[0]
    return re.findall(r'void\s*\*\s*(\w+)\s*;', body)


def disassemble(libdir, limit):
    out = []
    with tempfile.TemporaryDirectory() as td:
        for name in sorted(os.listdir(libdir)):
            if not name.endswith('.a'):
                continue
            d = os.path.join(td, name[:-2])
            os.makedirs(d, exist_ok=True)
            subprocess.run(['ar', 'x', os.path.join(libdir, name)], cwd=d, check=False)
            objs = sorted(f for f in os.listdir(d) if f.endswith('.o'))
            for o in objs[:limit]:
                r = subprocess.run([OBJDUMP, '-d', '-z', os.path.join(d, o)],
                                   capture_output=True, text=True)
                out.append((name[:-2] + '/' + o, r.stdout))
    return out


PAT_FUNC = re.compile(r'^([0-9a-f]{8}) <([^>]+)>:')
PAT_CALL = re.compile(r'callx(?:8|0|12|4)\s+(a\d+)')
PAT_L32I = re.compile(r'l32i(?:\.n)?\s+(a\d+),\s*(a\d+),\s*(0x[0-9a-f]+)')


def analyse(texts, nslots):
    hits = {}
    where = {}
    for objname, text in texts:
        func = '?'
        lines = text.split('\n')
        for i, line in enumerate(lines):
            m = PAT_FUNC.match(line)
            if m:
                func = m.group(2)
                continue
            c = PAT_CALL.search(line)
            if not c:
                continue
            reg = c.group(1)
            found = None
            for j in range(i - 1, max(i - 41, 0), -1):
                lj = lines[j]
                if PAT_FUNC.match(lj):
                    break
                mm = PAT_L32I.search(lj)
                if mm and mm.group(1) == reg:
                    found = int(mm.group(3), 16)
                    break
                parts = lj.split('\t')
                if len(parts) >= 3:
                    ops = [x.strip() for x in parts[2].split(',')]
                    if ops and ops[0] == reg:
                        break
            if found is None or found % 4 or found // 4 >= nslots:
                continue
            slot = found // 4
            hits[slot] = hits.get(slot, 0) + 1
            where.setdefault(slot, set()).add(objname.split('/')[0] + ':' + func)
    return hits, where


def main():
    libdir = DEFAULT_LIBDIR
    limit = 10 ** 9
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == '--libdir':
            libdir = args.pop(0)
        elif a == '--limit':
            limit = int(args.pop(0))
        else:
            raise SystemExit('用法: %s [--libdir DIR] [--limit N]' % sys.argv[0])

    names = slot_names()
    texts = disassemble(libdir, limit)
    hits, where = analyse(texts, len(names))
    print('%d 个槽位（来自 include/c6os/wifi_osi.h），被闭源库调用过的：' % len(names))
    for slot in sorted(hits):
        print('  slot %3d (0x%03x) %-34s x%-4d  例: %s'
              % (slot, slot * 4, names[slot], hits[slot],
                 ', '.join(sorted(where[slot])[:3])))
    unused = [s for s in range(len(names)) if s not in hits]
    print('\n没被直接调用过的 %d 个槽位（含 _version/_magic 和只在别处用的）：' % len(unused))
    print('  ' + ', '.join(names[s] for s in unused))
    return 0


if __name__ == '__main__':
    sys.exit(main())
