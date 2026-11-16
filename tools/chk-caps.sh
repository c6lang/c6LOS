#!/bin/sh
# 构建期能力门禁：personality（Linux ELF 二进制兼容层）只在具备
# MMU + 特权级 + 用户态可被打断的移植层上允许打开。
#
# 用法：tools/chk-caps.sh <port-name> <caps-hex-or-dec> <linux-abi>
# 退出：0 = 允许构建；1 = 配置自相矛盾，拒绝产出半成品镜像。
#
# 见 docs/linux-abi.md §2.2。
set -u

port=${1:-unknown}
caps=${2:-0}
abi=${3:-0}

# C6OS_CAP_MMU | C6OS_CAP_USER | C6OS_CAP_IRQ_USER == 7
need=7

if [ "$abi" = "1" ]; then
    missing=$((need & ~caps))
    if [ "$missing" -ne 0 ]; then
        {
            printf 'c6os: 配置错误：C6OS_LINUX_ABI=1，但 port "%s" 能力不足。\n' "$port"
            printf '      需要 MMU|USER|IRQ_USER = 0x%x，实际 port_caps = 0x%x，缺 0x%x。\n' \
                   "$need" "$caps" "$missing"
            printf '      无 MMU / 无特权级的移植层（如 ESP32-S3）不能打开 Linux ABI；\n'
            printf '      要么切回 C6OS_LINUX_ABI=0，要么换到 port/x86_64。\n'
            printf '      依据：docs/linux-abi.md §2。\n'
        } >&2
        exit 1
    fi
fi

exit 0
