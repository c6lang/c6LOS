# 可配置 Linux ELF 二进制兼容层（Tier 3）

> 状态：**L0–L8 全梯子已跑通** —— 未修改的动态 `busybox` 能跑 `sh` 管道，信号能
> 捕获/恢复，`pthread` 4 线程 + mutex 计数正确，调度已是**抢占式**（ring3 定时器
> 中断 + 时间片轮转）。范围：只保证"跑得动这类未修改二进制"，不含网络，文件系统
> 是只读 initramfs（见 §11）· 关联 [architecture.md](architecture.md) §1 修订 · [toolchain.md](toolchain.md) · [todo.md](todo.md)
>
> 目标：在**带 MMU 与特权级**的移植层上，用 `C6OS_LINUX_ABI` 打开原生 x86-64 Linux ELF
> 二进制兼容（静态/动态、进程、信号、线程）；在无 MMU 目标（ESP32-S3）上该特性
> **编译期不存在**，对镜像尺寸与实时性零影响。

## 0. 当前状态

| 里程碑 | 状态 | 可复现证据 |
|---|---|---|
| L0 门禁 / 能力模型 | **完成** | `make LINUX_ABI=1 s3` → 解析期 `$(error)` 直接拒绝；`make LINUX_ABI=1 x86-dyn` 放行（§2.2） |
| L1 ring3 通路 | **完成** | `make check-linuxabi` → `PASS: linux-abi L1（ring3 + int 0x80）` |
| L2 syscall 通道 + ELF64 静态加载 | **完成** | `make check-linuxabi-l2` → `PASS: linux-abi L2（未修改的 Linux 静态 ELF 在 ring3 跑通）` |
| L3 initramfs + 文件语义 | **完成** | `make check-linuxabi-l3` → `PASS: linux-abi L3（initramfs + 文件语义）` |
| L4 动态链接 + ld.so | **完成** | `make check-linuxabi-l4` → `PASS: linux-abi L4（未修改的动态 busybox 执行 ls / 成功）` |
| L5 进程模型 | **完成** | `make check-linuxabi-l5` → 未修改的 `busybox sh` 跑出 `GOT:hello-from-pipe`（§0.2） |
| L6 信号 | **完成** | `make check-linuxabi-l6` → `sig: SIGSEGV caught, back via siglongjmp`（§0.3） |
| L7 线程 | **完成** | `make check-linuxabi-l7` → `thr: 4 threads joined, counter=400, mutex=80`（§0.4） |
| L8 抢占式调度 | **完成** | `make check-linuxabi-l8` → 子进程纯用户态死循环时父进程仍能跑，内核 `[preempt] ticks=9 switch=1`（§0.5） |

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

L4 实测输出（`build/x86/dyn-disk.img`）。宿主上现成的 `/usr/bin/busybox` 是
**动态链接 PIE**（`PT_INTERP = /lib64/ld-linux-x86-64.so.2`，`DT_NEEDED =
libresolv.so.2 + libc.so.6`），原封不动放进盘里，参数 `ls /`：

```
c6os linux-abi L4: 动态链接（未修改的 busybox PIE）
elf: 映像已就位
fs: initramfs 已挂载
elf: 主程序已装载
elf: 解释器 /lib64/ld-linux-x86-64.so.2
elf: 就绪，进入用户态（RIP = 解释器入口）
bin
lib
lib64

[lx] exit(0) —— 用户进程正常结束
```

这条通路里**内核不做动态链接**：它只把主程序和 `ld.so` 各自按 ELF 段映射进
同一个地址空间，在栈上摆好 `AT_PHDR`/`AT_BASE`/`AT_ENTRY`，RIP 直接指向
`ld.so` 入口。重定位、符号解析、`DT_NEEDED` 递归加载、版本检查全部由 `ld.so`
自己完成——内核只要保证 `openat`/`pread`/`mmap`/`fstat` 的语义与 Linux 一致。
`ld.so` 映射库文件时走 `mmap` 的 file-backed 分支：分配物理页 → 用
`lx_fs_pread_k` 把 initramfs 内容拷进内核页 → 再建用户页表。

两个必须踩过才知道的细节：

- **`fstat` 的 `AT_EMPTY_PATH`**：本机 glibc 的 `fstat` 实际发的是
  `newfstatat(fd, "", &st, AT_EMPTY_PATH=0x1000)`。只按路径查表的话空路径会被
  当成 `/`，于是**所有文件都返回根目录那套 `st_dev`/`st_ino`**；而 `ld.so`
  正是用 dev/ino 判断"这个文件已经加载过"，结果第二个库（libc）被整个跳过，
  接着就是满屏 `version 'GLIBC_2.x' not found`。症状离原因极远，最后靠
  `[open]`/`[fstat]` 日志才定位：libc 只 open、**一次 mmap 都没发**。
- **`invlpg`**：`mmap` 填完页表必须刷 TLB，否则 `ld.so` 读回的 `.dynamic` 仍是
  零页（表现为 `link_map->l_info[DT_STRTAB]` 为 NULL，随即在
  `_dl_map_object` 里 #GP）。

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
   0x10000（boot16 装载）/ 0x11000（stage1 之后）且 `.bss` 到 ~0x92000。L1 只分配十几个
   页所以碰不到；L2 一次映射上百页，分配器越过内核镜像末尾之后把内核代码当页表页
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

### 0.2 L5 进程模型：协作式调度 + 两个"必须踩过才知道"的细节

**已完成（qemu 实测，全部是未修改的 Linux 二进制）**：`fork`/`vfork`/`clone`(fork
语义)、`execve`（动态，含 `ld.so` 重新装载）、`wait4`、`pipe`/`pipe2`/`poll`、
`dup`/`dup2`、`exit` 状态码与僵尸回收、fd 继承。验收跑的是**未修改的 `busybox`**，
由内核直接装载并给它 `sh -c 'echo hello-from-pipe | while read l; do echo GOT:$l; done'`：

```
elf: 主程序已装载
elf: 解释器 /lib64/ld-linux-x86-64.so.2
[lx] exit(0) —— 用户进程正常结束
GOT:hello-from-pipe
[lx] exit(0) —— 用户进程正常结束
[lx] exit(0) —— 用户进程正常结束
[lx] 所有进程都已退出，内核停在这里
```

三个进程（sh 本体 + 管道两端子 shell）各自 `fork`+`execve`、管道连通、按序退出 ——
这正是 `sh -c 'a | b'` 的结构。

**设计取舍：协作式调度。** ring3 的 `RFLAGS.IF=0`（`IRQ_USER` 还没做），用户态不会
被打断，所以进程切换只发生在**系统调用边界**：`lx_syscall` 返回前调一次
`lx_sched_tick()`。阻塞式 syscall（空管道 `read`、无子进程 `wait4`、`poll`）走
`lx_proc_block()` → `resched_()`，对端 `write`/`close`/`exit` 再 `wake_()`/
`wake_all_()`。每进程一块**静态** 8KB 内核栈（`g_kstack[8][8192]`），
`lx_ctx_switch()` 只保存/恢复 callee 寄存器，切换路径里不做任何分配。

#### 坑 1：`MSR_FS_BASE` 是 CPU 全局寄存器，TLS 必须跟着进程切

现象很迷惑：`pipe`+`fork`+子进程 `execve(busybox)` **成功**（`ls /` 输出正常、
`exit(0)` 正常），但**父进程**从阻塞恢复后、还在自己的 libc 里就 `#PF`：

```
vec=14 err=0x0005 cr2=0x7fefffe056c8  pid=1
rip=0x7fefffef4e8f（父进程 libc 代码段内，可执行）
```

`cr2` 落在父进程 libc 基址（0x7FEFFFE1B000）**以下** 0x15A38 —— 只可能来自"用另一个
基址算出来的地址"。真正的原因是：子进程的 `ld.so` 执行 `arch_prctl(ARCH_SET_FS, …)`
把**全局** FS base 换成了 busybox 的 TLS 块；父进程恢复后 libc 仍用 `%fs + 偏移`
取 `errno`，于是读到子进程地址空间里的地址，在父进程的页表里当然没有。

修法：`LxProc.fs_base` 保存每进程 TLS 基址，`switch_to_()` 里 `wrmsr(IA32_FS_BASE)`
跟着切；`fork` 继承、`execve` 归零（新镜像自己会设）。**凡是"每进程一份"的 CPU 状态
（CR3、RSP0、FS/GS base、FPU）都必须在切换点成组处理 —— 漏一个就是这种"人在内核里、
数据来自别人"的 bug，而且只在 execve 之后才现形。**

#### 坑 2：`busybox sh` 的管道依赖 `poll`

`echo x | while read l; …` 的 `read` 内建用 `poll` 探 stdin。原来 `poll` 走
`-ENOSYS`，`read` 当 EOF 处理，管道对端写的东西全丢 —— 现象是"三个进程都
`exit(0)`，但一行输出都没有"，看上去完全不像缺 syscall。补上 `poll`(7)/`ppoll`(271)：
只看管道可读/可写/对端已关闭，普通 fd 一律报就绪；没有别的可运行进程时返回 0
（协作式调度里等不到人，不能死等）。

### 0.3 L6 信号：把 rt_sigframe 复刻对，两条返回路径就都通了

内核侧只维护每进程的 `handler/flags/restorer/mask`、屏蔽字、挂起位和
`sigaltstack`；投递时在用户栈上压一个 **Linux 布局** 的 `rt_sigframe`
（`pretcode` + `ucontext`（`uc_mcontext.gregs` 按 Linux 的 23 项顺序）+ `siginfo`），
然后把 iret 帧改成"跳进 handler、RSP 指到帧基址"。restorer 是 glibc 的
`__restore_rt`（`sigaction` 时随 `SA_RESTORER` 传进来），它执行
`rt_sigreturn(15)` 回到内核，我们再按同一份布局把 23 个 gregs 原样恢复。
于是两种典型写法同时成立：

- handler 正常 `return` → 走 `rt_sigreturn` → 上下文完全恢复；
- handler 里 `siglongjmp` 跑掉 → 不再回内核，屏蔽字由 glibc 的
  `sigsetjmp(env, 1)`/`siglongjmp` 自己恢复（和 Linux 行为一致）。

异常 → 信号：`#PF`/`#GP`/`#UD`/`#DE`/`#AC` 且 `cs&3==3` 时，先把异常帧改造成
信号帧；handler 是 `SIG_DFL` 就按信号终止（`wait4` 的状态字是信号号本身，
`WIFSIGNALED` 成立），没装 handler 又没有默认动作的才停机。内核态异常照旧打
`!! CPU 异常` 并停机。

实测（`make check-linuxabi-l6`，未修改的动态链接程序）：

```
sig: SIGUSR1 handler ok          <- kill 自投递 + handler 正常 return
sig: SIGUSR1 blocked ok          <- 屏蔽期间只挂起
sig: delivered on unblock        <- sigprocmask(SIG_UNBLOCK) 立即投递
sig: SIG_IGN ok
[lx] 进程被信号终止 sig=11        <- 子进程 SIG_DFL
sig: child killed by SIGSEGV     <- 父进程 wait4 拿到 WIFSIGNALED
sig: SIGSEGV caught, back via siglongjmp
sig: done
```

#### 踩到的坑：阻塞进程的陷入帧不在 `LxProc.f` 里

第一版在 `switch_to_()` 里对所有要上 CPU 的进程都用 `&p->f` 投递信号，
结果 L5 的管道用例立刻炸：`cs=0`（内核态）`cr2=0xfffffffffffffa80`。
`0xfa80 = 128 + 1280` —— 正是 `sp - red_zone - sizeof(rt_sigframe)`，而 `sp` 是 0。

原因：`LxProc.f` 只是"**首次启动**新子进程"用的寄存器快照
（`setup_trampoline_`/`x86_resume_user` 那条路）。一个已经跑起来的进程在
系统调用里阻塞时，真正的陷入帧固定在内核栈顶的 128 字节
（`x86_syscall_entry` 压的 16 个寄存器，阻塞、让出都不会挪动它），`p->f` 是
陈旧数据。修法：`LxProc.started` 区分两种情况 —— 没启动过的用 `&p->f`，
启动过的用 `(LxFrame *)(kstack_top - 128)`。**任何时候要改"某个不在 CPU 上的
进程的用户上下文"，都必须先确认那份帧到底在哪儿。**

### 0.4 L7 线程：共享状态怎么放，决定要多写多少代码

pthread 要的内核面其实不大：`clone(CLONE_VM|THREAD|SETTLS|PARENT_SETTID|
CHILD_CLEARTID)`、每线程的 `%fs`（TLS）、`futex`、以及线程退出时清
`clear_child_tid` 再唤醒 `pthread_join`。麻烦的是"哪些状态是线程共享的"：

| 状态 | 归属 |
|---|---|
| 地址空间 / `brk` / `mmap_top` | 线程组共享 |
| fd 表 | 组共享（`CLONE_FILES`） |
| `sigaction` 表 | 组共享（`CLONE_SIGHAND`） |
| 屏蔽字 / `sigaltstack` / `FS base` / 内核栈 / 陷入帧 / `clear_child_tid` | 每线程 |

这里没有引入新的共享对象，而是让同组任务各存一份，在**切换点**与组长做双向
同步：让出前写回组长，上 CPU 前从组长取新值（`sync_to_leader_` /
`sync_from_leader_`）。代价是每次切换多复制 16 个 fd + 32 个 sigaction；
收益是 fork/exec/wait/信号这些已经跑通的路径**一行都没改** —— 对已经绿了六个
里程碑的代码，这个交换很划算。

顺带修掉一个潜伏的 bug（这个才是 pthread 一开始必炸的原因）：切换时原来只把
`g_brk`/`g_mmap_top` 从 PCB 往全局搬，**从不往回存**。单进程时看不出来（没有
别人动这两个值），但一旦发生切换，切回来就按陈旧值把分配指针往回拽，后续
`mmap` 与已有映射重叠。现象极具迷惑性：`cr2 == rip`（取指缺页），地址比上一个
线程栈高出一个栈大小，看起来像"跳到了没映射的地址"。修法是在 `switch_to_`
让出前先 `c->brk = lx_brk_get(); c->mmap_top = lx_mmap_top_get();`。

实测（`make check-linuxabi-l7`，未修改的 pthread 程序）：

```
[clone tid=2 tgid=1] … [clone tid=5 tgid=1]
thr: 4 threads joined, counter=400, mutex=80
thr: done
[lx] exit_group(0)
```

四个线程各自跑在自己的 8MB 栈上、各有 TLS，`mutex` 走 futex，`pthread_join`
靠 `clear_child_tid` + futex 唤醒收口，最后 `exit_group` 整组退出。
协作式调度下线程不会被抢占，所以"计数正确"靠的是每个线程一次跑到底 + 共享
同一份地址空间；抢占要等 `IRQ_USER`（见 §11 的取舍）。

### 0.5 L8 抢占：ring3 的 IF=1 + PIT 定时器

- 返回用户态时 `RFLAGS.IF=1`（`port_user_enter` 的初始帧和 `x86_ret_to_user` 都
  带上 IF）；`syscall` 的 `SFMASK=0x600` 清 IF，所以**内核态永远不可抢占** ——
  等价于"进内核即关中断的临界区"，进程表、页表、fd 表都不需要额外加锁。
- `port_linux_timer_init()`：PIC 重映射到 0x20/0x28、只放开 IRQ0、PIT 约 200Hz、
  IDT 上装 0x20 号**中断门**（0x8E，进内核自动清 IF）。
- `x86_timer_entry`：补 15 个通用寄存器 → `x86_timer_dispatch`（EOI + 让调度器
  看一眼）→ 原样弹回 `iretq`。这里**不能**复用 `x86_ret_to_user`：那条路拿
  rcx/r11 装 RIP/RFLAGS，而中断必须原封不动保留用户的 rcx/r11。
- `lx_sched_preempt()`：每 `LX_SLICE_TICKS`(=2) 个 tick 调一次 `resched_()`。
  被定时器抢占的进程，活帧是 20 个 qword（15 寄存器 + 硬件帧），不是
  `kstack_top-128` 那份 LxFrame，所以 `switch_to_` 靠 `LxProc.irq_frame` 判断，
  这种情况把信号投递延后到它下次系统调用返回。

实测（`make check-linuxabi-l8`，未修改的动态链接程序）：

```
preempt: parent runs while child spins
[preempt] ticks=9 switch=1
```

子进程是**纯用户态死循环**（一次系统调用都不做）：协作式内核里父进程永远回不来，
现在定时器把它切下去，父进程才拿到 CPU。`ticks` 是 ring3 定时器中断次数，
`switch` 是"被定时器切走"的次数。

#### 顺手修掉的调度 bug（这个才是第一版的死因）

`pick_next_()` 的循环写成 `i <= LX_MAX_PROC`，最后一圈
`(from + LX_MAX_PROC) % LX_MAX_PROC == from` 会把**当前槽位自己**返回；而
`resched_()` 在调用它之前刚把当前置成 READY，于是单进程时必然
`switch_to_(自己)` —— 用陈旧 `ksp` 自切换，把内核栈换坏（现象是随机 RIP 的
`#GP` 接三重故障，CPU 日志里 `SP=0000:0`）。协作式调度下 `resched_` 很少在
"只有自己 READY"时被调用，所以这条一直潜伏；抢占每 2 个 tick 走一次这条路，
一开就必炸。改成 `i < LX_MAX_PROC`。

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

> **修订（平台无关化，M21）**：上表说明的是"这个 port 能不能当宿主"，**不足以**说明
> "能不能跑某个 ABI"——本文 §3/§8 里的地址空间布局、`LX_SYS_*` 号、`stat`/`sigframe`
> 布局都是 `x86_64-linux` 这**一个后端**的实例。现已把 `linux/` 拆成 ABI 无关核心
> `lx/` 与每架构一份的 `abi/<abi>/`，门禁也从一维（caps）变二维（caps + ABI 架构）。
> 详见 [linux-abi-portable.md](linux-abi-portable.md) §1/§4 与
> [maixcam-bringup.md](maixcam-bringup.md)（第一个新后端是 `rv64-linux`）。

### 2.2 构建期门禁（两道）

> 实现在 `Makefile` 里分两道：**解析期** `$(error)`（`LINUX_ABI=1` 且目标含
> `s3`/`all` 时直接停，一个字节都不产出）和每个 port 目标上的
> `caps-check-<port>` recipe（调 `tools/chk-caps.sh`）。
>
> 这里踩过两个坑，都已修：①recipe 里原来用 shell 变量 `$CAPS_x86` 取能力位，
> 那是 make 变量、不进 recipe 环境，永远展开成 0 —— 结果连 x86 都会被拒；
> 能力位必须在 **make 展开期** 填进去。②`caps-check-<port>` 作为隐含规则
> 前提，在目标不是命令行显式目标时可能被 make 的依赖判定跳过，所以又加了
> 解析期那道。

1. **配置检查**：`C6OS_LINUX_ABI=1` 但 `C6OS_PORT_CAPS & (MMU|USER|IRQ_USER)` 不完整
   → `tools/chk-caps.sh` 在 make 的最前面以可读错误信息失败，**不产生半成品镜像**。
2. **静态断言**：`kernel/linux/abi.c6` 里 `static_assert` 兜底，防止有人绕过 make。

---

## 3. 地址空间布局（x86-64，低半核）

内核留在低地址（`port/x86_64/stage2.ld`，基址 0x11000 紧贴 stage1），**不做高半区迁移**：
迁移会同时改动启动链、链接脚本、所有物理地址常量，收益为零。代价是内核占用低地址
区间，用链接期断言保证内核不越过 0xA0000（VGA/BIOS 空洞起点）。

```
0x0000000000000000  0-1MB      BIOS/IVT/引导扇区，不用
0x0000000000100000  1-4MB      内核（identity、supervisor-only、RW）
  ├─ 0x11000 起：内核 text/rodata/data/bss（链接期断言 end <= 0xA0000）
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
| **L4** ✅ | 动态链接：`PT_INTERP`、`ld.so`、`mmap` 文件映射、`AT_BASE`/`AT_ENTRY`、relocation 全交给 `ld.so` | 动态链接（PIE）的 `busybox` 跑起 `ls /` |
| **L5** ✅ | 进程模型：`fork`、`execve`、`wait4`、`pipe`、`poll`、fd 继承 | 未修改的 `busybox sh` 跑通 `echo … \| while read …` 管道，三进程按序退出（§0.2） |
| **L6** ✅ | 信号：`rt_sigaction`/`rt_sigprocmask`/`sigaltstack`/`kill`/`rt_sigreturn` 帧精确复刻 | 用户态 `SIGSEGV` 被捕获、`siglongjmp` 恢复；默认动作下 `wait4` 看到 `WIFSIGNALED`（§0.3） |
| **L7** ✅ | 线程：`clone`、`futex`(WAIT/WAKE)、每线程 FS base、`pthread` | 未修改的 pthread 程序 4 线程 + mutex 计数正确（§0.4） |
| **L8** ✅ | 抢占式调度：ring3 `IF=1`、PIT 定时器中断、时间片轮转 | 子进程纯用户态死循环时父进程仍能运行，内核计数 `switch≥1`（§0.5） |

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
| L4 动态 ✅ | `mmap` 文件映射(9) `pread64`(17) `writev`(20) `fstat`→`newfstatat(AT_EMPTY_PATH)` `ioctl`(16) `getuid`/`getgid`/`geteuid`/`getegid` `setuid`/`setgid` `time`(201) |
| L4 待补 | `fcntl`(72) `poll`(7) `epoll_create1`(291) `readlink`(89) `dup`(32)（随 L5 补齐） |
| L5 进程 ✅ | `fork`(57) `vfork`(58) `clone`(56，仅 `fork` 语义) `execve`(59) `wait4`(61) `pipe`(22) `pipe2`(293) `poll`(7) `ppoll`(271) `dup`(32) `dup2`(33) `close`(3，管道引用计数) `getppid`(110) `setpgid`(109) `getpgrp`(111) `setsid`(112) `umask`(95) `chdir`(80) `fcntl`(72，空实现) |
| L5 待补 | `kill`(62) `select`(23) `waitid`(247) 以及抢占式调度（需 `IRQ_USER`） |
| L6 信号 ✅ | `rt_sigaction`(13) `rt_sigprocmask`(14) `rt_sigreturn`(15) `sigaltstack`(131) `kill`(62) `tgkill`(234)；`#PF`/`#GP`/`#UD`/`#DE`/`#AC` → `SIGSEGV`/`SIGILL`/`SIGFPE`/`SIGBUS` |
| L6 待补 | `rt_sigtimedwait`(128) `sigsuspend` 语义、`SA_ONSTACK` 真正切到备用栈、`SIGKILL` 的不可捕获语义（现在只拒绝改它的 handler） |
| L7 线程 ✅ | `clone`(56，`CLONE_VM`/`THREAD`/`SETTLS`/`PARENT_SETTID`/`CHILD_CLEARTID`/`SETTID`) `futex`(202，WAIT/WAKE) `set_tid_address`(218) `exit_group`(231) `sched_yield`(24) `sched_getaffinity`(204) `madvise`(28) `prctl`(157) |
| L7 待补 | 抢占式调度（需 `IRQ_USER`）、`futex` 的 PI/robust 变体、`clone3`(435) |

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

- **网络（socket）**：一条都没有。`socket/connect/bind/listen/accept/send/recv/
  setsockopt` 全部走 `-ENOSYS`，也没有网卡驱动或 TCP/IP 栈。initramfs 里的
  `libresolv.so.2` 只是因为 `busybox` 有 `DT_NEEDED`（必须能加载，不是能用）。
- **可写文件系统**：文件系统是**只读**的 newc cpio initramfs（内核解析成节点表），
  fd 只做读；`open` 不带 `O_CREAT/O_WRONLY`，没有 `unlink/mkdir/rename/truncate`、
  没有 `mount`、也没有块设备上的文件系统（`port_ata_read` 只按裸扇区读 ELF 与
  initramfs 镜像）。写方向只有 console 和 pipe。
- 任意发行版二进制的全兼容：只保证本文档里这几个**未修改**的测试二进制
  （静态 glibc 程序、动态 PIE busybox、信号用例、pthread 用例）跑得动，
  不追求通用性（见 §7 末尾）。

- 不追求跑任意发行版二进制（缺 `/proc` 全表、`/sys`、cgroup、namespace 就不承诺）；
- 不支持 32 位 x86 二进制（`int 0x80` 的 i386 ABI 不做）；
- 不做 KPTI/PTI、完整 ASLR、seccomp、user namespace；
- 不在 S3 上做任何相关尝试（无 MMU、无特权级，物理上不成立）。
