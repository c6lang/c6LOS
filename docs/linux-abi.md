# 可配置 Linux ELF 二进制兼容层（Tier 3）

> 状态：L3 已跑通（initramfs + 文件语义；未修改的静态 Linux ELF 用 opendir/read 读真实文件）· 关联 [architecture.md](architecture.md) §1 修订 · [toolchain.md](toolchain.md) · [todo.md](todo.md)
>
> 目标：在**带 MMU 与特权级**的移植层上，用 `C6OS_LINUX_ABI` 打开原生 x86-64 Linux ELF
> 二进制兼容（静态/动态、进程、信号、线程）；在无 MMU 目标（ESP32-S3）上该特性
> **编译期不存在**，对镜像尺寸与实时性零影响。

## 0. 当前状态

| 里程碑 | 状态 | 可复现证据 |
|---|---|---|
| L0 门禁 / 能力模型 | **完成** | `make LINUX_ABI=1 s3` → 明确拒绝（`tools/chk-caps.sh`） |
| L1 ring3 通路 | **完成** | `make check-linuxabi` → `PASS: linux-abi L1（ring3 + int 0x80）` |
| L2 syscall 通道 + ELF64 静态加载 | **完成** | `make check-linuxabi-l2` → `PASS: linux-abi L2（未修改的 Linux 静态 ELF 在 ring3 跑通）` |
| L3 initramfs + 文件语义 | **完成** | `make check-linuxabi-l3` → `PASS: linux-abi L3（initramfs + 文件语义）` |

L1 实测输出（qemu-system-x86_64，`build/x86/user-disk.img`）：

```
c6os linux-abi L1: ring3 bring-up
mmu: e820=00000007 free_kb=0003ef80
mmu: cr3=00402000
mmu: ok
arch: gdt/tss/int80 ok
blob loaded
entering ring3 @0x400000 ...
hello from ring3

[lx] exit(0) —— 用户进程正常结束
```

L2 实测输出（`build/x86/elf-disk.img`）。用户态二进制由宿主
`gcc -static` 编译的**普通 Linux 程序**（`tests/linux/hello.c`），经磁盘
LBA 1024 载入，内核侧对它零改造：

```
c6os linux-abi L2: load an unmodified static Linux ELF
mmu: e820=00000007 free_kb=0003ef80
mmu: cr3=00402000
elf: size ok
elf: image read
elf: segments mapped
elf: stack ready, entering userland
[user] hi from an unmodified Linux x86-64 ELF

[lx] exit(0) —— 用户进程正常结束
```

L3 实测输出（`build/x86/fs-disk.img`）。同一个用户态程序（`tests/linux/ls.c`，
宿主 `gcc -static`）用 `opendir("/")` + `readdir` + `open`/`read` 读内核提供的
**initramfs**（标准 newc cpio，和 Linux 的 initramfs 同格式）：

```
c6os linux-abi L3: initramfs + 文件语义
elf: 映像已就位
fs: initramfs 已挂载
elf: 段与栈就绪，进入用户态
[ls] 根目录:
  .
  ..
  bin
  etc
  hello.txt
[ls] /etc/version:
c6os 1.0
[ls] /hello.txt:
hello from the c6LOS initramfs
[ls] 完成

[lx] exit(0) —— 用户进程正常结束
```

磁盘布局：LBA 1024 = 用户 ELF，LBA 4096 = 8 字节长度前缀 + cpio 流（LBA 4097
起）。**两者必须留够间距**：静态 glibc 程序 650KB 左右，从 LBA 1024 起会占到
1.16MB；initramfs 若放 LBA 2048 就会把 ELF 尾部覆盖掉，症状是用户态在 glibc
启动阶段就 #GP、一条系统调用都没发（这个坑实际踩过）。

它的系统调用序列与同一二进制在宿主上的 `strace -f` **逐条一致**：
`brk brk arch_prctl set_tid_address set_robust_list rseq(→ENOSYS) prlimit64
readlinkat getrandom brk brk brk mprotect write exit_group`。

已在 L1/L2 打通的硬件事实：E820 采集（boot16）→ PMM 位图 → 每进程 4 级页表 →
PML4[256] 直接映射窗口 → GDT/TSS（`ltr` 的 16 字节描述符必须在 GDT 限长内）→
`int 0x80` 中断门（DPL3）与 `syscall`（MSR_LSTAR）两条陷入通道 → iretq 双向切换 →
异常 0..31 全部装桩并打印 `vec/err/cr2/cs/rip/rsp`。

L2 修掉的 L1 简化：

- 系统调用主通道换成 `syscall`，寄存器约定与 Linux 一致（`rax`=号，
  `rdi/rsi/rdx/r10/r8/r9` 传参，返回值在 `rax`）；`int 0x80` 保留作调试通道；
- 每次陷入换到**内核栈**（`x86_kern_stack`，TSS.RSP0 同源），为 L5 的阻塞式
  系统调用铺路；
- 用户指针校验接上（`copy_from_user` / `port_as_user_range_ok`），
  `write/getrandom/readlinkat/prlimit64` 都会先验范围；越界返回 `-EFAULT`。

仍未做（后续里程碑）：用户态中断（`RFLAGS.IF=1` + `IRQ_USER`）、
`fork/execve/wait`、动态链接、信号。

### 0.1 L2 踩到的三个坑（都已写进代码注释）

1. **PMM 会把内核自己分配出去。** E820 里低 4MB 是 usable，而内核就住在
   0x10000（boot16 装载）/ 0x20000（链接）且 `.bss` 到 ~0x3d000。L1 只分配十几个
   页所以碰不到；L2 一次映射上百页，分配器越过 0x20000 之后把内核代码当页表页
   清零 —— 现象是"日志断在半行"（三重故障）。现在 `pmm_init` 无条件扣掉
   `[0, KERNEL_RESERVED_END)`。
2. **auxv 的 type/value 顺序。** 栈向低地址长，`push8(type)` 之后再
   `push8(value)`，内存里是 `[value][type]`；Linux 读的是 `[type][value]`。
   必须**先压 value 再压 type**。
3. **argv 的 NULL 结尾 + 对齐填充的位置。** 漏掉 argv 结尾会让 glibc 的
   `envp = argv + argc + 1` 直接落到 auxv 上；而"在 envp 与 auxv 之间垫 8 字节
   对齐"会被 glibc 当成 `AT_NULL`，auxv 整个读空，`_dl_random` 为 NULL，
   启动时立刻读空指针。16 字节对齐只能垫在**字符串与 auxv 之间**。

这三条都是"内核看起来完全正常、用户态静默跑飞"的类型，所以异常桩（上面第
三条硬件事实）在 L2 阶段就必须有：没有它，任何内核 trap 都是整机消失。

---

## 1. 定位：personality，不是"移植 Linux"

这不是让 c6LOS 变成 Linux，也不是跑一个 Linux 内核。它是 **personality 层**
（FreeBSD linuxulator / WSL1 / NetBSD `COMPAT_LINUX` 的做法）：

- 硬件仍是裸机 x86-64，c6LOS 仍是那个 32 优先级抢占内核；
- 用户态跑的是**未经修改的 Linux ELF 二进制**（x86-64 SysV ABI）；
- 二进制与内核之间只有一条缝：**系统调用**。兼容层负责把这 300 来个系统调用
  映射到 c6LOS 的对象与设备上。

关键区别：**Linux 用户态只通过 syscall 观察内核**。只要 syscall 语义、ELF 加载、
信号帧、`/proc` 表像、TLS 约定一致，二进制根本不知道底下不是 Linux。

| 配置 | 含义 | 镜像影响 |
|---|---|---|
| `C6OS_LINUX_ABI=0`（默认） | 纯 RTOS，单地址空间，同特权 | 0 字节 |
| `C6OS_LINUX_ABI=1` + `C6OS_LINUX_TIER=3` | 打开 personality：ring3 + 进程 + 信号 + 线程 | 仅该目标增长 |

`C6OS_LINUX_TIER` 是**让步构建**用的：1=静态单进程、2=动态链接、3=完整进程模型。
Tier 3 是目标，但低 tier 必须始终可构建、可回归。

---

## 2. 能力模型：MMU 是必要条件，不是充分条件

"有 MMU 就能开"只对了一半。可开 personality 需要三件硬件能力，缺一不可：

| 能力位 | 含义 | 没有它会发生什么 |
|---|---|---|
| `C6OS_CAP_MMU` | 可切换的地址空间（换页表根） | 进程之间无法隔离，`brk` 越界无法捕获 |
| `C6OS_CAP_USER` | 用户态/内核态特权边界 + 陷入通道 | 用户代码可直接改内核，syscall 无从拦截 |
| `C6OS_CAP_IRQ_USER` | 中断/异常能在特权边界之间正确换栈与返回 | 用户态一被时钟打断就 triple fault |

> 只有 MMU 没有 USER（多数 MCU 的 MPU）只能做"区域保护"，做不出 syscall 边界；
> 只有 USER 没有 MMU（xv6 早期）只能单进程。两者都有才是宿主。

### 2.1 各 port 现状

| port | MMU | USER | 结论 |
|---|---|---|---|
| `port/x86_64`（qemu 裸机） | ✅ long mode + 4 级页表 | ✅ ring0/ring3（待实现） | **第一宿主**，Tier 3 在此验收 |
| `port/rv32bare` | ❌ M-mode 裸跑（无 satp） | ❌ | 升级到 S-mode + Sv32 后才可开（二期） |
| `port/s3`（ESP32-S3） | ⚠️ 只有 cache 映射，无进程页表 | ❌ 无特权级 | **永久关闭**；`port_caps()` 返回 0 |
| `port/sim` / host | ❌ 隔离靠宿主进程 | ❌ | 只做 ABI **语义**回归（不验证特权切换） |

### 2.2 构建期门禁（两道）

1. **配置检查**：`C6OS_LINUX_ABI=1` 但 `C6OS_PORT_CAPS & (MMU|USER|IRQ_USER)` 不完整
   → `tools/chk-caps.sh` 在 make 的最前面以可读错误信息失败，**不产生半成品镜像**。
2. **静态断言**：`kernel/linux/abi.c6` 里 `static_assert` 兜底，防止有人绕过 make。

---

## 3. 地址空间布局（x86-64，低半核）

内核留在低地址（沿用 `port/x86_64/link.ld` 的 0x20000），**不做高半区迁移**：迁移会
同时改动启动链、链接脚本、所有物理地址常量，收益为零。代价是内核占用低地址区间，
用链接期断言保证内核不越过 4MB。

```
0x0000000000000000  0-1MB      BIOS/IVT/引导扇区，不用
0x0000000000100000  1-4MB      内核（identity、supervisor-only、RW）
  ├─ 0x20000 起：内核 text/rodata/data/bss（链接期断言 end < 0x400000）
  ├─ 内核栈（每 CPU 8KB，static）
  ├─ IDT / GDT / TSS / 页表池
  └─ 引导参数（E820 表 @0x6000，由 boot16 填）
0x0000000000400000  4MB     用户 ELF 默认装载基址（与 Linux 静态 ELF 默认 0x400000 相同）
0x0000000000400000+           brk 起点 = ELF 末尾
0x0000007F00000000            mmap 区（向下增长，Linux 默认约定）
0x00007FFFFFFFE000            用户栈顶（向下增长，Linux 默认约定）
```

页表策略：

- 每个进程一棵 4 级页表，`PML4[0]` 覆盖 0–512GB，PDPT/PD/PT 按需从 PMM 分配；
- 内核区间（0–4MB）在每个地址空间里都映射为 **supervisor + RW**，用户不可读写；
- 用户页 `U/S=1`，按 ELF 段的 `PF_R/W/X` 给权限；栈带 NX；
- 初期允许内核与用户共享低地址区（无 KPTI）；**不承诺抵御 Meltdown 类侧信道**。

---

## 4. 执行上下文与陷入通道

### 4.1 两种任务

| 类型 | 地址空间 | 特权级 | 可否访问内核对象 |
|---|---|---|---|
| 内核任务（现有语义，S3 上唯一形态） | 单地址空间 | ring0 | 是 |
| 用户任务（`proc != 0`） | 每进程 | ring3 | 只能经 syscall |

调度器不区分两者：同一条就绪链、同一套优先级。区别只在切换时的
`port_ctx_switch()` 是否额外换 `CR3` 与内核栈。

### 4.2 进入内核

| 通道 | 用途 | 栈切换 | 备注 |
|---|---|---|---|
| `int 0x80` | L1 起步、兼容老二进制 | CPU 经 TSS.RSP0 自动换栈 | 简单，先做 |
| `syscall`（LSTAR） | x86-64 主通道（glibc/musl 默认） | 手工换到内核栈 + `iretq` 返回 | L2 已实现（`x86_syscall_entry`） |

### 4.3 返回用户态

**统一用 `iretq`，不用 `sysret`。** 理由（写进代码注释）：

1. `sysret` 遇到非规范 `RCX` 会 `#GP`，且历史上是 CVE 温床（需要额外检查）；
2. `sysret` 不恢复 `SS:RSP`，无法表达"返回时栈被信号改写"（信号投递必需）；
3. 性能差异（约 20 周期）对"兼容层"目标可以忽略；内核任务路径不受影响。

### 4.4 从用户态来的中断/异常

陷阱入口必须判断 `CS & 3`：

- CPL=0 → 现有路径（同栈保存、`iretq`）；
- CPL=3 → CPU 已按 TSS.RSP0 换栈，保存完整帧，`os_tick_irq()` 后仍 `iretq`；
- `#PF` 在用户态 → 投 `SIGSEGV`（不是 panic）；内核态 → panic（现有行为）。

---

## 5. 内核对象

```c
struct OsProc {                 /* 进程：地址空间 + 资源 */
    u32  pid, ppid, pgid, sid;
    u64  cr3;                   /* 页表根物理地址 */
    struct OsMm mm;             /* VMA 链：code/data/heap/mmap/stack */
    u64  brk_start, brk_end;
    u64  ustack_top;
    struct OsFile *fd[C6OS_LINUX_MAX_FD];
    struct OsSigAction sig[C6OS_LINUX_NSIG];
    u64  pending, blocked;
    int  exit_code;
    struct OsProc *parent;
    struct OsWait *waiters;     /* wait4 */
    u32  flags;                 /* EXITING/ZOMBIE/... */
    char cwd[64];
};
```

- `struct OsTask` 增加 `struct OsProc *proc`；`proc == 0` 即内核任务（S3 上恒为 0）；
- 线程 = 共享 `OsProc` 的任务（`clone(CLONE_VM|CLONE_THREAD)`），TLS 在 `task->fsbase`；
- 进程回收沿用现有 `reap` 链：置 `ZOMBIE` 等父进程 `wait4`，父进程不存在则挂到 init。

---

## 6. 用户内存安全（最容易被做漏的一条）

`copy_from_user` / `copy_to_user` 必须存在，否则任何用户指针都能让内核越界读写
——那时兼容层不是"增加了功能"，而是**把内核的隔离性卖掉了**。

两阶段实现：

| 阶段 | 做法 | 强度 |
|---|---|---|
| L2 起步 | VMA 内范围检查 + `stac/clac`（SMAP 可选）+ 页表存在位检查 | 挡住绝大多数错误指针 |
| L5 完整 | `.extable` 异常表 + `#PF` 修复：拷贝指令出错时跳到修复表项，返回 `-EFAULT` | 与 Linux 同级，能扛并发 `munmap` |

---

## 7. 里程碑与验收

| 里程碑 | 内容 | 验收（qemu 真跑） |
|---|---|---|
| **L0** ✅ | 配置门禁、能力模型、`port_caps()`、本文档 | `C6OS_LINUX_ABI=0` 全目标不受影响；`=1` 在 S3 上 make 明确拒绝 |
| **L1** ✅ | GDT/TSS/IDT、ring3、PMM、4 级页表、`int 0x80`、首次返回用户态 | 用户态打印 `hello from ring3` 并 `exit(0)`，内核收到状态 0 |
| **L2** ✅ | `syscall` 通道、最小 syscall 集、`copy_from_user`、ELF64 静态加载器 | 加载**未修改的 Linux 静态 ELF**，它自己打印的那行字出现在串口上，并以 `exit_group(0)` 结束 |
| **L3** ✅ | initramfs（newc cpio）+ fd 表 + `openat`/`read`/`fstat`/`getdents64`/`lseek` | 未修改的静态 Linux 程序用 `opendir`/`readdir`/`open`/`read` 列出并读出 initramfs 里的文件 |
| **L4** | 动态链接：`PT_INTERP`、`ld.so`、`mmap` 文件映射、`AT_BASE`/`AT_ENTRY`、relocation | 动态链接（PIE）的 `busybox` 跑起 `busybox ls /` |
| **L5** | 进程模型：`fork`(CoW)、`execve`、`wait4`、`pipe`、fd 继承 | `sh -c 'a | b'` 管道通，`fork` 子进程退出码正确 |
| **L6** | 信号：`rt_sigaction`/`rt_sigprocmask`/`sigaltstack`/`rt_sigreturn` 帧精确复刻 | 用户态 `SIGSEGV` 被捕获、`longjmp` 恢复 |
| **L7** | 线程：`clone`、futex 完整、FS base 切换、`pthread` | 静态 `pthread` 程序 4 线程计数正确 |

每级验收必须在 `make check-linuxabi` 里可复现，且**上一级不退**。

> 关于验收二进制：本机没有 musl 工具链（`/usr/bin/musl-gcc` 不存在），L2 用的是
> 宿主 `gcc -static` 链接的 glibc 程序。glibc 对 auxv 与 TLS 的要求**比 musl 更
> 严**（前者的启动代码会读 `AT_RANDOM` 建栈保护、读 `AT_PHDR` 校验装载），先用它
> 更容易暴露问题；换 musl 只是换一个输入文件，内核侧不需要改。

---

## 8. syscall 面（x86-64 号）

| 归属 | syscall |
|---|---|
| L2 最小集 ✅ | `read`(0) `write`(1) `exit`(60) `exit_group`(231) `brk`(12) `mmap`(9) `munmap`(11) `mprotect`(10) `arch_prctl`(158) `set_tid_address`(218) `set_robust_list`(273) `getrandom`(318) `rseq`(334，返回 ENOSYS) `prlimit64`(302) `readlinkat`(267) `uname`(63) `getpid`(39) `gettid`(186) `futex`(202，单线程空实现) |
| L3 文件 ✅ | `openat`(257) `open`(2) `close`(3) `read`(0) `fstat`(5) `newfstatat`(262) `lseek`(8) `getdents64`(217) `access`(21) `getcwd`(79) |
| L3 待补 | `ioctl`(16) `readlink`(89) `getuid`(102) `dup`(32) `pipe2`(293)（随 L4/L5 补齐） |
| L4 动态 | `pread64`(17) `fcntl`(72) `poll`(7) `epoll_create1`(291)、`ld.so` 的重定位与 `mmap` 文件映射 |
| L5 进程 | `clone`(56) `fork`(57) `vfork`(58) `execve`(59) `wait4`(61) `pipe2`(293) `dup2`(33) `getppid`(110) `chdir`(80) `getcwd`(79) `kill`(62) |
| L6 信号 | `rt_sigaction`(13) `rt_sigprocmask`(14) `rt_sigreturn`(15) `sigaltstack`(131) `rt_sigtimedwait`(128) |
| L7 线程 | `clone` 完整语义、`futex` 完整、`sched_yield`(24) `gettid`(186) `tgkill`(234) |

原则：**未实现的 syscall 返回 `-ENOSYS` 并打一条限速日志**，绝不让用户进程
把内核卡死或 panic。可观测性优先于"看起来能跑"。

---

## 9. S3 零侵入的验收证据

`C6OS_LINUX_ABI=0` 时，必须能用数字证明没有回归：

1. `make s3` 的 text/data/bss 与基线**逐字节相同**（`tools/imgsize.sh` 出对比表）；
2. S3 tick 延迟 P99 与专项测试 `tests/t_s3_*` 不回归；
3. `kernel/linux/` 下所有文件**不参与** S3 的编译单元列表（不是被 `#if 0` 掉）。

这条是"可配置"的实质：不是开关写着 0，而是代码根本不进镜像。

L2 阶段的实际证据与限制：

- 第 3 条已结构性成立：`linux/*.c` 只出现在 `X86S_NAME=elf` 的对象列表里，
  唯一出口是 `X86S_OBJ` 这个变量；S3 的对象列表里既没有 `linux/`，也没有
  `-DC6OS_LINUX_ABI`。
- `make LINUX_ABI=1 s3` 在**编译开始之前**就被 `tools/chk-caps.sh` 拒绝
  （S3 的 `C6OS_PORT_CAPS=0`，缺 `MMU|USER|IRQ_USER`），见 §2.2。
- 第 1、2 条（与基线的字节对比、tick P99）目前**拿不到基线**：`main` 上的
  `make s3` 因 `os_event_get` 未参与链接而失败（`kernel/event.c6` 不在 S3 的
  对象列表里），`make check-host` 也因 `port_cur_ctx / port_snapshot /
  port_switch_trace` 未定义而失败。这两处与本特性无关（本特性的改动只在
  `port/x86_64/`、`linux/`、`include/c6os/` 与 x86 相关目标），属于既有问题；
  修好后本节三条证据才能补齐。

---

## 10. 风险表

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| glibc 启动探测（`rseq`/IFUNC/`prlimit64`）失败即 abort | 高 | L4 卡住 | 先 musl（探测少），再逐条对照 glibc `sysdeps/unix/sysv/linux` |
| 信号帧布局复刻错误 | 高 | 用户态收到信号即崩 | 用 Linux 头文件里的 `rt_sigframe` 布局逐字段对齐 + 断言 sizeof |
| 32 位 rv32 的 `time_t`/`off_t` 变体 | 中 | 二期返工 | 只做 x86-64（LP64），rv32 走 ABI 变体层 |
| `copy_from_user` 做漏 | 中 | **安全模型崩塌** | L2 就必须有；L5 补 extable |
| fork 写时复制与现有静态堆冲突 | 中 | L5 卡住 | 进程页从 PMM 走，内核堆与用户页彻底分离 |
| c6 编译器在大型子系统上的可用性 | 中 | 工期 | kernel 侧保持 c6；纯算法密集部分（ELF 解析）可先 C 原型再迁 c6 |
| 与现有 `X86_MAX_CTX=6` 静态上下文冲突 | 高 | L5 阻塞 | 用户进程栈/上下文改由 PMM 分配，扩展为动态表 |

---

## 11. 明确不做

- 不追求跑任意发行版二进制（缺 `/proc` 全表、`/sys`、cgroup、namespace 就不承诺）；
- 不支持 32 位 x86 二进制（`int 0x80` 的 i386 ABI 不做）；
- 不做 KPTI/PTI、完整 ASLR、seccomp、user namespace；
- 不在 S3 上做任何相关尝试（无 MMU、无特权级，物理上不成立）。
