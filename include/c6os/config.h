#ifndef C6OS_CONFIG_H
#define C6OS_CONFIG_H

/* 可覆盖的编译期配置；目标侧用 -D 覆盖。 */
#ifndef C6OS_MAX_CORES
#define C6OS_MAX_CORES 2
#endif
#ifndef C6OS_MAX_TASKS
#define C6OS_MAX_TASKS 16
#endif
#ifndef C6OS_PRIOS
#define C6OS_PRIOS 32
#endif
#ifndef C6OS_TICK_HZ
#define C6OS_TICK_HZ 100
#endif
#ifndef C6OS_TIMESLICE
#define C6OS_TIMESLICE 10
#endif
#ifndef C6OS_STACK_DEFAULT
#define C6OS_STACK_DEFAULT 8192
#endif
#ifndef C6OS_SHELL_ALLOW_REBOOT
#define C6OS_SHELL_ALLOW_REBOOT 0
#endif
#ifndef C6OS_HEAP_SIZE
#define C6OS_HEAP_SIZE 65536
#endif

/* ---- Linux ELF 二进制兼容层（personality，见 docs/linux-abi.md）---------
 * 0 = 关闭（默认）。kernel/linux/ 不参与编译，镜像零字节影响。
 * 1 = 打开。要求 port 满足 C6OS_CAP_LINUX_ABI（MMU|USER|IRQ_USER），
 *     否则 tools/chk-caps.sh 在构建期拒绝。S3 上恒为 0。 */
#ifndef C6OS_LINUX_ABI
#define C6OS_LINUX_ABI 0
#endif

/* 让步构建梯度：1=静态单进程  2=动态链接  3=完整进程模型（目标） */
#ifndef C6OS_LINUX_TIER
#define C6OS_LINUX_TIER 3
#endif

#ifndef C6OS_LINUX_MAX_PROC
#define C6OS_LINUX_MAX_PROC 8
#endif
#ifndef C6OS_LINUX_MAX_FD
#define C6OS_LINUX_MAX_FD 16
#endif
#ifndef C6OS_LINUX_NSIG
#define C6OS_LINUX_NSIG 64
#endif

/* 用户地址空间布局（x86-64 低半核，见 docs/linux-abi.md §3）。
 * 内核必须整体落在 0..C6OS_LINUX_KERNEL_END 之内，由链接期断言保证。 */
#ifndef C6OS_LINUX_KERNEL_END
#define C6OS_LINUX_KERNEL_END 0x400000ull
#endif
#ifndef C6OS_LINUX_ELF_BASE
#define C6OS_LINUX_ELF_BASE 0x400000ull
#endif
#ifndef C6OS_LINUX_STACK_TOP
#define C6OS_LINUX_STACK_TOP 0x7fffffffe000ull
#endif
#ifndef C6OS_LINUX_MMAP_BASE
#define C6OS_LINUX_MMAP_BASE 0x7f0000000000ull
#endif

#endif
