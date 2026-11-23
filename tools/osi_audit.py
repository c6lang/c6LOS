#!/usr/bin/env python3
"""审计我们 OSI 表的覆盖情况：哪些槽位接了实现、哪些走兜底 glue_ret0。

为什么要有（docs/wifi-plan.md §225）：OSI 表有 120 个槽位，闭源库随便调哪个都
可能致命（§218 的 wpa_crypto_funcs 就是 NULL 指针的例子）。这个脚本把
`port/s3/wifi_glue.c` 里 `s3_wifi_osi_init()` 的 OSI_SET(...) 与
`include/c6os/wifi_osi.h` 的字段顺序对齐，列出：
  - 已接实现的槽位（连同实现函数名）；
  - 仍是兜底空实现的槽位（这些要么静态统计证明库不调，要么返回 0 就是正确答案，
    要么是本轮已知的待办）。

用法： tools/osi_audit.py [--list-unimpl]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HDR = os.path.join(ROOT, 'include', 'c6os', 'wifi_osi.h')
GLUE = os.path.join(ROOT, 'port', 's3', 'wifi_glue.c')


def fields():
    txt = open(HDR).read()
    body = txt.split('typedef struct wifi_osi_funcs_t {', 1)[1].split('}', 1)[0]
    return re.findall(r'void\s*\*\s*(\w+)\s*;', body)


def init_body():
    lines = open(GLUE).read().split('\n')
    start = next(i for i, l in enumerate(lines) if l.startswith('void s3_wifi_osi_init'))
    depth = 0
    out = []
    i = start
    while i < len(lines):
        depth += lines[i].count('{') - lines[i].count('}')
        out.append(lines[i])
        if i > start and depth <= 0:
            break
        i += 1
    return '\n'.join(out)


def main():
    show_unimpl = '--list-unimpl' in sys.argv
    f = fields()
    body = init_body()
    wired = dict(re.findall(r'OSI_SET\((\w+),\s*([\w\.]+)\)', body))
    missing = [(i, name) for i, name in enumerate(f) if name not in wired]
    print('OSI 槽位 %d 个：已接实现 %d，兜底空实现 %d'
          % (len(f), len(wired), len(missing)))
    if show_unimpl:
        for i, name in missing:
            print('  %3d %s' % (i, name))
    return 0


if __name__ == '__main__':
    sys.exit(main())
