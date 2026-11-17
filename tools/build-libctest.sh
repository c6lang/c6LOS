#!/bin/bash
# 把 musl libc-test functional 子集的测试编译成动态链接 ELF 并放进 staging 目录。
# 用法: build-libctest.sh <staging-dir>
# 动态链接：18 个测试共享一份 libc（静态链接每个带一份 libc 共 14M，远超
# initramfs 3MB 上限）。解释器 ld.so 与 libc.so 由 Makefile 放进 staging。
# 每个测试自身 main 返回 t_status，宿主已验证这些测试在干净 environment 下 PASS。
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
C="$ROOT/tests/linux/libctest/src/common"
F="$ROOT/tests/linux/libctest/src/functional"
OUT=${1:?usage: build-libctest.sh <outdir>}
LIBSRC="$C/print.c $C/memfill.c $C/rand.c $C/utf8.c $C/path.c $C/fdfill.c $C/vmfill.c $C/setrlim.c"

# 宿主已 PASS 的纯计算/字符串/搜索子集，无 locale/网络/线程/外部文件依赖
TESTS="string string_memcpy string_memset string_strchr string_strcspn \
string_strstr qsort setjmp snprintf search_tsearch search_hsearch \
search_lsearch search_insque strtod strtof udiv wcsstr"

mkdir -p "$OUT/lt"
for t in $TESTS; do
    gcc -Os -I "$C" "$F/$t.c" $LIBSRC -o "$OUT/lt/$t"
done
echo "built $(echo $TESTS | wc -w) libc-test binaries into $OUT/lt"
