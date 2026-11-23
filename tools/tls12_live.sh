#!/bin/sh
# 在**宿主**上真连 qq.com:443，跑完 TLS 1.2 握手 + HTTP GET（不需要板子）。
#
# 这是"取 https://qq.com 页面"这条路的协议层验证：板子上要做的只是把 socket
# 换成 lwIP（§210 已经链进镜像），TLS/HTTP 这一层一行都不用改。
#
# 用法： ./tools/tls12_live.sh [host] [port]      （或 make tls12live）
set -e
C6=${C6_ROOT:-/home/winger/c6}
C6CC=$C6/tcc/c6
OUT=${OUT:-build/hostc6}
HOST=${1:-qq.com}
PORT=${2:-443}
mkdir -p "$OUT"
"$C6CC" -B "$C6/tcc" -I"$C6" -Inet -DTLS_HOST="\"$HOST\"" -DTLS_PORT="$PORT" \
    -o "$OUT/tls12_live" \
    "$C6/std/core/types.c6" "$C6/std/core/bits.c6" "$C6/std/core/rand.c6" \
    "$C6/std/encoding/aes.c6" "$C6/std/encoding/sha256.c6" \
    "$C6/std/net/x25519.c6" "$C6/std/net/tcp.c6" \
    "$C6/std/encoding/asn1.c6" "$C6/std/text/str.c6" "$C6/std/data/x509.c6" \
    net/aes_gcm.c6 net/tls12_prf.c6 net/rsa_verify.c6 net/cert_parse.c6 \
    net/tls12_hs.c6 \
    tests/t_tls12_live.c6
"./$OUT/tls12_live"
