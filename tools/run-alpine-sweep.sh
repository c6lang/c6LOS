#!/bin/sh
# run-alpine-sweep.sh —— 跑 Alpine ISO，带"引导看门狗"和总时长预算。
#
# 为什么不用 `qemu ... | tr -d '\r' | tee log`：本次验证里这条管道在串口输出
# 到 4KB 左右会整体停住（qemu 卡在写 chardev，客户机卡在 put_chr 轮询），同一
# 张 ISO 改成 `> log 2>&1` 直接重定向就正常。日志里的 \r 由解析脚本 strip 掉。
#
# 用法：run-alpine-sweep.sh <iso> <data.img> <log> <alive 标记> <done 标记> <秒>
#   引导 90s 内没看到 alive 标记就杀掉重试（最多 3 次，只重试"没进脚本"的）；
#   进了脚本之后最多等 <秒>，看到 done 标记就成功返回。
set -u

iso=$1
data=$2
log=$3
alive=$4
done_marker=$5
budget=$6
attempt=0

: > "$log"

while [ "$attempt" -lt 3 ]; do
    attempt=$((attempt + 1))
    qemu-system-x86_64 -m 256M -cdrom "$iso" -boot d \
        -drive format=raw,file="$data",if=ide \
        -nographic -serial stdio -monitor none -no-reboot \
        -nic user,model=e1000 > "$log" 2>&1 &
    pid=$!

    waited=0
    booted=0
    while [ "$waited" -lt 90 ]; do
        sleep 2
        waited=$((waited + 2))
        if grep -q "$alive" "$log" 2>/dev/null; then
            booted=1
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
    done

    if [ "$booted" = 0 ]; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "重试：第 $attempt 次引导没进脚本（装载/串口停住）"
        : > "$log"
        continue
    fi

    waited=0
    while [ "$waited" -lt "$budget" ]; do
        sleep 2
        waited=$((waited + 2))
        grep -q "$done_marker" "$log" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
    done
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    if grep -q "$done_marker" "$log" 2>/dev/null; then
        exit 0
    fi
    echo "FAIL: 进了脚本但没跑完（预算 ${budget}s 用完或客户机挂住）"
    exit 1
done

echo "FAIL: 3 次引导都没进脚本"
exit 1