#!/bin/sh
# 在**宿主**上跑 TLS 1.2 客户端的各个自测（不需要板子）。
#
# 为什么能在宿主上跑（docs/wifi-plan.md §211/§212）：c6 编译器 tcc/c6 可以直接
# 把"多文件 + 模块源码"编成宿主可执行文件，而且宿主上有真 socket；
# 所以 TLS 1.2 这一层可以对着真 qq.com 开发和验证，再原样搬到 ESP32-S3。
#
# 用法： ./tools/tls12_selftest.sh      （或 make tls12check）
set -e
C6=${C6_ROOT:-/home/winger/c6}
C6CC=$C6/tcc/c6
OUT=${OUT:-build/hostc6}
mkdir -p "$OUT"

run() {
    name=$1
    shift
    printf '\n========== %s ==========\n' "$name"
    "$C6CC" -B "$C6/tcc" -I"$C6" -Inet -o "$OUT/$name" "$@"
    "./$OUT/$name"
}

run aes_gcm_test \
    "$C6/std/core/types.c6" "$C6/std/encoding/aes.c6" \
    net/aes_gcm.c6 tests/t_aes_gcm.c6

run tls12_prf_test \
    "$C6/std/core/types.c6" "$C6/std/core/bits.c6" "$C6/std/encoding/sha256.c6" \
    net/tls12_prf.c6 tests/t_tls12_prf.c6

run rsa_verify_test \
    "$C6/std/core/types.c6" "$C6/std/core/bits.c6" "$C6/std/encoding/sha256.c6" \
    net/rsa_verify.c6 tests/t_rsa_verify.c6

printf '\n全部 TLS 1.2 基础件自测通过。\n'
