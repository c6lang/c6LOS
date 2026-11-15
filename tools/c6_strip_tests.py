#!/usr/bin/env python3
"""去掉 c6 源码里的顶层 `test "…" { … }` 块。

为什么要它（docs/wifi-plan.md §214）：
  c6 标准库的模块（std/core/types.c6 之类）末尾都带 `test "…" { … }` 自测块。
  这些块在 `-c`（只编译）时也会被生成到 .c99 里，于是引入 c6 运行时里的
  `__tcc_err_set_copy()` 之类只有 `-run` 才需要的符号，导致我们给 ESP32-S3
  交叉编译时链接失败。板子上根本不需要这些自测，所以编译前先剥掉。

用法：
    tools/c6_strip_tests.py <输入.c6> <输出.c6>
"""
import re
import sys


def strip(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        # 行首（允许缩进）的 test ... { ？
        m = re.match(r'[ \t]*test\b[^\n{]*\{', text[i:])
        if m and (i == 0 or text[i - 1] == '\n'):
            j = i + m.end() - 1          # 指向 '{'
            depth = 0
            while j < n:
                c = text[j]
                if c == '{':
                    depth += 1
                elif c == '}':
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                elif c == '"':           # 跳过字符串
                    j += 1
                    while j < n and text[j] != '"':
                        if text[j] == '\\':
                            j += 1
                        j += 1
                elif c == '/' and j + 1 < n and text[j + 1] == '/':
                    while j < n and text[j] != '\n':
                        j += 1
                    continue
                j += 1
            # 吃掉块后面的换行，保持行号大致对齐
            while j < n and text[j] in '\r\n':
                j += 1
            out.append('\n' * text.count('\n', i, j))
            i = j
            continue
        out.append(text[i])
        i += 1
    return ''.join(out)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write('用法: %s <输入.c6> <输出.c6>\n' % sys.argv[0])
        return 2
    src = open(sys.argv[1], encoding='utf-8').read()
    open(sys.argv[2], 'w', encoding='utf-8').write(strip(src))
    return 0


if __name__ == '__main__':
    sys.exit(main())
