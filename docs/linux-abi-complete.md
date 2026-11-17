# Linux ABI 完整支持（Tier 3+）总体设计

> 状态：**设计稿，M13 起施工**。L0–L8 已由 [linux-abi.md](linux-abi.md) 验收；
> 本文处理"从能跑几个 busybox 命令"到"Alpine 用户态程序功能完整正确"之间的差距。
> 关联：[linux-abi.md](linux-abi.md)、[vfs-net-design.md](vfs-net-design.md)、
> [m11-docker-iso-design.md](m11-docker-iso-design.md)、[todo.md](todo.md)。

> 施工原则（用户给定，本文所有取舍都服从它）：
> **先按 LiteOS 兼容面补 OS 的缺口，再补 Linux ABI 的缺口**；
> 架构师视角、全局出发、可维护、高性能、资源不受限（不设人为的编译期小上限）。

---

## 0. 口径：什么叫"完整支持"

"完整 Linux"不是有限工作量（Linux 内核自身 300+ syscall、几十个子系统、每版还在长），
所以先把验收面钉死。本文用四级台阶定义"完整"，每级都有可复现的 `check-*`：

| 级别 | 含义 | 验收面 | 本文状态 |
|---|---|---|---|
| **T0 现状** | busybox 的 `sh`/`ls`/`cat`/`uname` 等少数命令能跑 | 现有 `check-linuxabi-l1..l8`、`check-alpine` | ✅ 已达成 |
| **T1 POSIX 功能正确** | libc 语义、文件语义、进程/信号/线程、阻塞/超时、错误码都按 POSIX 走 | musl `libc-test` 的 functional 子集（明确豁免项要写进清单） | 本文 M13–M16 |
| **T2 Alpine base 全 applet** | Alpine 官方 minirootfs 里 busybox 的每个 applet 都能正确工作（含错误路径） | busybox applet 矩阵 + Alpine 基础包冒烟 | 本文 M15–M19 |
| **T3 常见软件包** | 依赖 pty/网络/多线程的常见程序（`wget`、`dropbear`/`ssh`、`vim`、`python3` 等）能跑 | 固定版本的软件包冒烟清单 | 本文 M17–M18，逐包推进 |
| **T4 容器语义** | namespace/cgroup/overlayfs/seccomp | — | **明确不做**（见 §8） |

"正确"的判定不只看"打印出了东西"，还看四件事，所有验收都按这四条写：

1. **返回值与 `errno`** 与 Linux 一致（含 `ENOSYS` 之外的正确错误码）；
2. **阻塞与唤醒** 语义一致（不忙等、不丢唤醒、超时准时、信号可打断）；
3. **资源可回收**：进程/线程/映射/文件反复创建销毁后，空闲内存回到基线；
4. **并发正确**：多线程共享 fd/地址空间/信号时结果稳定。

---

## 1. 现状与缺口（代码实测，不是文档推测）

### 1.1 分层与代码位置

```
app (Alpine/busybox/glibc 程序)
   │  syscall / int 0x80
linux/          ABI 适配层：sys.c(71 case) fs.c proc.c sig.c tty.c elf64.c
   │  只允许调用 ↓
kernel/         LiteOS 兼容内核面：vfs/fsapi/ramfs/romfs/fatfs、sal + net/*、
   │            task/timer/tick/mutex/sem/queue/membox/heap
port/x86_64/    硬件层：mmu.c(4 级页表/CR3)、user_entry.S(syscall/iret)、
                irq.c(PIT/异常)、ata.c、e1000.c、linux.c
```

### 1.2 结构性缺口（按 OS 子系统）

| 子系统 | 现状 | 缺口 | 直接后果 |
|---|---|---|---|
| 物理内存 | `port_phys_free()` 存在但**没有任何调用者**；`port_as_destroy()` 是空函数；`port_as_unmap()` 只清 PTE 不还页 | 页框引用计数、地址空间销毁、映射替换/回收 | `execve`/`fork`/`munmap` 永久漏页；跑几十个命令后 OOM |
| 地址空间 | 只有页表根 + `brk`/`mmap_top` 两个整数；没有 VMA | VMA 链、保护位、COW、按需分页、栈增长、`mremap/msync`、`MAP_SHARED` | `fork` 全量拷贝；`mprotect` 空实现；`mmap` 恒 RW；栈固定 128 KB |
| 进程/线程 | `LX_MAX_PROC=8` 静态数组；每线程**静态** 8 KB 内核栈 | 动态任务表、动态内核栈（guard page）、pid 分配器 | 最多 8 个线程；内存浪费 |
| fd / IO | 每进程 `C6OS_LINUX_MAX_FD=16` 固定数组；按 `kind` 分支处理 | 可增长 fd 表、`OsFile` 对象 + ops、CLOEXEC、`O_NONBLOCK` | 16 个 fd；select/epoll 无从实现 |
| 阻塞模型 | 管道/终端各自轮询 + `lx_proc_block()`；futex `WAKE` 唤醒**所有**阻塞进程 | 统一等待队列 + deadline + 信号打断 | 丢唤醒/伪唤醒、不能 `EINTR`、不能超时 |
| VFS | `fsapi.c6` 按文件系统名字分支；路径解析分散在 `fs_find`/`vfs_owns_`/`path_` | ops 分发表、挂载表、dentry 缓存、page cache | 写类 syscall 缺失；挂载点 `stat/access/chdir` 不可见 |
| 设备 | 只有 console | devfs（null/zero/full/random/urandom/tty/ptmx） | `2>/dev/null`、`/dev/tty` 全部失败 |
| 信号 | 32 个信号；`SIGSTOP` 默认动作是**终止**；无作业控制；不发 `SIGPIPE` | 64 信号、stop/cont、pgrp/session/ctty、EINTR/`SA_RESTART`、`sigsuspend` | shell 作业控制、`timeout`、管道提前退出的程序行为错 |
| 时间 | `time()` 返回常量；`nanosleep` 忙等；只有两个 clock id；无 RTC | 校准 TSC、睡眠队列、`clock_*`/`gettimeofday`/定时器 | `date` 不准；`timeout` 不准；程序探测 clock 失败 |
| 事件 | 只有 `poll/ppoll`（轮询） | `select`、`epoll`、`eventfd`、`timerfd`、`signalfd` | `epoll_wait` 的程序直接 `ENOSYS` |
| 网络 | 内核有 SAL（UDP/TCP 停止-等待子集），**Linux ABI 没有 socket 面** | `OsSock` + socket 族 + AF_UNIX + epoll 集成 + TCP 完善 | 用户态完全不能联网 |
| /proc /sys | 无；`readlinkat("/proc/self/exe")` 硬编码返回 `/hello` | procfs 最小集、`/sys` 最小集 | `ps/top/free/df`、`/proc/self/maps` 全部不可用 |
| 安全 | 无 SMAP/KPTI/ASLR/seccomp；`getrandom` 是 LCG；`prctl` 一律成功 | 权限位、`O_EXCL/O_NOFOLLOW`、seccomp 返回 `EINVAL` 不假成功 | 隔离性弱；`seccomp` 探测会误判 |
| 可观测性 | 未实现 syscall 每次打日志（无限速）；无 strace | 限速日志、可选的 syscall trace 环 | 用户态跑飞时难定位 |

### 1.3 人为资源上限（目标：全部动态化）

| 上限 | 现值 | 位置 | 目标 |
|---|---|---|---|
| 进程/线程数 | 8 | `linux_proc.h: LX_MAX_PROC` | 动态任务表，仅受内存限制 |
| 每线程内核栈 | 静态 8 KB × 8 | `g_kstack` | 首次调度时分配（含 guard page） |
| fd/进程 | 16 | `config.h: C6OS_LINUX_MAX_FD` | 可增长，软上限可配 |
| 管道容量 | 4 KB，固定区 | `linux/proc.c: LX_PIPE_SZ` | 动态环形缓冲，默认 64 KB |
| argv | 16 个 / 64 KB | `LX_ARG_MAX`、`LX_ARG_BYTES` | 按 RLIMIT/内存，默认 Linux 的 2 MB |
| 信号 | 32 | `LX_NSIG` | 64（Linux x86-64 内核 sigset 8 字节） |
| ramfs | 32 节点 / 8 KB arena | `kernel/ramfs.c6` | 堆上动态，节点/数据按需 |
| 用户栈 | 固定 32 页（128 KB） | `linux/elf64.c: STACK_PAGES` | VMA + 按需增长到 RLIMIT_STACK（8 MB） |
| 路径/节点 | 96 B / 1024 节点 | `fs.c: FS_NAME/FS_MAX_FILES` | 动态节点表 + 更长路径 |
| CPU | 单核 | port | 保持单核（SMP 归 §8） |

### 1.4 P0 bug 清单（本轮先修，都是"看起来能用、实际错"的类型）

| # | 位置 | 现象 | 修法 |
|---|---|---|---|
| B1 | `linux/sys.c` `LX_SYS_setpgid` 与 `LX_SYS_chdir` 共用一个 case | `setpgid()` 实际执行 `chdir(pid指针)` | 拆开；`setpgid` 真正改 pgrp |
| B2 | `port/x86_64/mmu.c` | `port_as_destroy()` 空、`port_as_unmap()` 不 free、`MAP_FIXED` 覆盖不 free | 页引用计数 + 递归释放页表 + 销毁路径 |
| B3 | `linux/proc.c: lx_proc_wait4` | 忽略 `options`，`WNOHANG` 会阻塞 | 选项解析 + 非阻塞轮询 |
| B4 | `linux/proc.c: lx_futex` | 只认 WAIT/WAKE；WAKE 广播所有阻塞进程；无 bitset/PI/robust/超时 | 按地址挂等待队列；补 WAIT_BITSET/WAKE_BITSET/REQUEUE；robust list |
| B5 | `linux/proc.c: pipe_write_` | 无读者时只回 `EPIPE`，不发 `SIGPIPE` | 发 `SIGPIPE`（可忽略） |
| B6 | `linux/proc.c: lx_proc_execve` | `(void)uenvp` 丢掉环境变量；无 `#!`；CLOEXEC 未实现 | envp 上栈、shebang、exec 时按 CLOEXEC 关 fd |
| B7 | `linux/fs.c: statat/openat/access/chdir` | `dirfd` 全部被忽略（`(void)dirfd`） | 相对路径按 dirfd 解析；`AT_EMPTY_PATH` 保持 |
| B8 | `linux/fs.c: statat/access/chdir` | 不查 VFS，挂载点不可见 | 统一走 `vfs_lookup`（见 M15） |
| B9 | `linux/sys.c: readlinkat` | `/proc/self/exe` 恒返回 `/hello` | 记录每进程 exe 路径；procfs 提供 |
| B10 | `linux/sys.c: mprotect` | 空实现，`mmap` 恒 RW | 改 PTE；W^X 可配 |
| B11 | `linux/sys.c` | `gettimeofday`、`clock_getres`、`clock_nanosleep` 直接 `ENOSYS` | 时间子系统（M16） |
| B12 | `linux/sig.c: lx_sig_kill` | 负 pid 当自身；`tgkill` 丢 tgid；`SIGSTOP/SIGTSTP` 默认终止 | 进程组投递 + stop/cont 状态机 |
| B13 | `linux/sys.c` | `readv` 有号无实现；`sendfile` 缺 | 补 file ops 后一起实现 |
| B14 | `linux/sys.c: prctl/madvise/fcntl/umask/setuid...` | 一律返回 0（假成功） | 能实现的实现，不能的返回明确错误；`fcntl` 真语义 |
| B15 | `Makefile` | 干净构建时 `check-linuxabi-l5..l8` 找不到 `dyn-disk.img` 规则 | 已修：补文件目标 + order-only 依赖 |
| B16 | `linux/sys.c: wait4`（同类：kill/setpgid/tgkill 等 int 参数） | pid 是 Linux `int`，按 u64 直接取用：`wait(-1)` 变 +4294967295，恒 `ECHILD` | 已修 wait4（`(i64)(i32)a0`）；其余 int 参数在 M14 统一扫一遍 |

> B15 是施工前顺手修掉的基础设施 bug；没有它，L5–L8 的回归无法在干净树上跑。

---

## 2. 架构原则（后续所有改动的评审标准）

1. **LiteOS 面先行**：内存、任务、锁、队列、定时器、VFS、块设备、网络这些
   "LiteOS-M 本来就该有的能力"进 `kernel/`，用 `LOS_*` 兼容命名与语义；
   Linux ABI 层只做参数翻译和 Linux 布局复刻，**不允许**把策略写进 `linux/`。
   （LiteOS-M 没有进程/地址空间/信号，这部分用 `Os*` 命名放 `kernel/mm`、`kernel/proc`。）
2. **一切对象化 + 引用计数**：地址空间、VMA、文件、fd 表、socket、挂载点、页框
   都有所有者/引用计数/明确销毁路径。"资源不受限"的前提是资源能回收。
3. **统一阻塞模型**：所有可能阻塞的点（管道、终端、socket、futex、wait4、
   nanosleep、poll/epoll）都走 `OsWaitQueue` + deadline。
   `-EINTR`、`-ETIMEDOUT`、`SA_RESTART` 只在这一层实现一次。
4. **统一 file ops**：`read/write/poll/ioctl/mmap/close/stat` 进分发表；
   `select/epoll` 只依赖 `ops->poll`，syscall 层不再出现 `if (kind == PIPE)`。
5. **动态资源**：不设编译期小上限；表按需增长；只在内存耗尽时失败。
   保留可配置的水位线用于防御性限流，而不是功能上限。
6. **分层不反转**：`linux/` → `kernel/` → `port/`，禁止反向依赖。
   `C6OS_LINUX_ABI=0` 时新代码**不参与编译**（对象列表门禁），不是 `#if 0`。
7. **可验证**：每个里程碑一个 `check-*`；用户态测试程序进 `tests/linux/`；
   测试用的镜像/工具版本固定在 Makefile 里。

性能上不做玄学优化，先守住三个底线：syscall 快路径不打印、不分配；
fd/futex/页缓存用哈希或位图，不做 O(n) 全表扫；阻塞路径零轮询（终端泵除外）。

---

## 3. 内核对象模型

### 3.1 地址空间、VMA、页框

```c
typedef struct OsVma {              /* 一段连续映射 */
    u64 start, end;                 /* [start,end) */
    u32 prot;                       /* R/W/X */
    u32 flags;                      /* PRIVATE/SHARED/GROWSDOWN/STACK */
    struct OsFile *file;            /* NULL = 匿名 */
    u64 off;                        /* 文件内偏移 */
    struct OsVma *next;
} OsVma;

typedef struct OsAs {
    u64 pml4;                       /* 物理地址 */
    u32 ref;                        /* 线程组共享（CLONE_VM） */
    OsVma *vma;                     /* 按 start 排序 */
    u64 brk, brk_start;
    u64 mmap_top;
    u64 exe_path;                   /* /proc/self/exe 用（内核拷贝字符串） */
} OsAs;
```

- **页框引用计数**：PMM 增加 `u16 ref[page_count]`，`port_phys_get/put()`；
  PTE 的空闲位（如 bit 9）标记 COW。`put` 到 0 才回 free list。
- **fork**：复制 VMA 链 → 可写页标 COW、`ref++`；只读页直接共享。
  写缺页时分配新页、拷贝、改回可写。
- **按需分页**：用户态 `#PF` 先查 VMA：
  - 匿名页 → 分配零页映射；
  - 文件页 → 页缓存命中直接映射，未命中从 VFS `read` 一页；
  - 栈 VMA → 向下扩展（有上限）；
  - 不在 VMA 或权限不符 → `SIGSEGV`（带 `si_addr`）。
- **销毁**：`OsAsPut()` 遍历页表只释放 `PTE_U` 叶子和页表页；
  `execve` 先切到新 AS，再把旧 AS `put` —— 回收路径统一成一条。
- **页缓存**：`(mount, inode, page_index) → page` 哈希 + LRU；
  `MAP_SHARED` 写回、FAT 后端复用现有 `LOS_FsWrite`。

### 3.2 文件与 fd 表

```c
typedef struct OsFile {
    const struct OsFileOps *ops;     /* vfs / pipe / tty / dev / sock / proc */
    u32 ref;
    u32 flags;                       /* O_NONBLOCK / O_APPEND / ... */
    u64 pos;                         /* 独立偏移（CLONE_FILES 时由 fd 表共享） */
    void *priv;                      /* inode、pipe、socket、dev 私有数据 */
} OsFile;

typedef struct OsFdTable {
    OsFile **fd;
    u64 *cloexec;                    /* 位图 */
    u32 max;                         /* 可增长 */
    u32 ref;                         /* CLONE_FILES 共享 */
} OsFdTable;
```

- fd 分配取最低空闲位；`dup/dup2/dup3/fcntl(F_DUPFD)` 共用一套。
- `execve` 关闭所有 CLOEXEC fd（B6）。
- `poll` 统一走 `ops->poll(file, events, &revents)`；`select/epoll` 都建在它上面。

### 3.3 任务、进程、等待队列

```c
typedef struct OsWaitQueue { OsWaitEntry *head; } OsWaitQueue;
typedef struct OsWaitEntry {
    struct OsTask *task; u64 deadline; u32 wake_mask;
    struct OsWaitEntry *next;
} OsWaitEntry;
```

- `OsTask`：tid/pid/tgid、状态、动态内核栈、FS base、FPU、挂起/屏蔽信号、
  `clear_child_tid`、退出码。
- `OsProc`：`OsAs*`、`OsFdTable*`、sigaction 表、pgrp/session/ctty、cwd、umask。
  线程 = 共享 `OsProc` 的 `OsTask`。
- 等待队列 API：`os_wait_prepare()` → `os_wait_sleep()` → `os_wait_wake_one/all()`；
  `sleep` 返回 `-EINTR/-ETIMEDOUT`；调度器只在这一层被调用。
- pid 分配器：位图 + 轮转；回收后 pid 不立即复用（防 `kill` 打错对象）。

### 3.4 时间

- 开机用 PIT 校准 TSC（`port_cycles()`），得到 ns 精度单调时钟；
- `OsTimerQueue`：按 deadline 排序的链表/时间轮，tick 驱动；
- 墙钟：读 CMOS RTC（新 `port_rtc_read()`），失败则用构建时注入的时间基准；
- `clock_gettime/gettimeofday/clock_getres/clock_nanosleep/nanosleep/定时器`
  全部走同一时钟源；`nanosleep` 不再忙等。

### 3.5 VFS 与设备

```c
typedef struct OsFileOps {
    ssize_t (*read)(OsFile *, void *, size_t);
    ssize_t (*write)(OsFile *, const void *, size_t);
    int     (*poll)(OsFile *, u32 events);
    int     (*ioctl)(OsFile *, u32 req, u64 arg);
    int     (*stat)(OsFile *, struct stat *);
    int     (*close)(OsFile *);
} OsFileOps;

typedef struct OsFsOps {           /* 文件系统后端 */
    int (*lookup)(OsMount *, const char *rel, OsInode **);
    int (*create)(...); int (*mkdir)(...); int (*unlink)(...);
    int (*rename)(...); int (*truncate)(...); int (*sync)(...);
    int (*read)(...);   int (*write)(...);  int (*readdir)(...);
    int (*statfs)(...);
} OsFsOps;
```

- 路径解析只有一份：`vfs_lookup(at_dirfd, path, flags)`，负责挂载表最长前缀、
  `..`、符号链接（深度上限 8）、绝对/相对、`AT_EMPTY_PATH`。
- `LOS_Fs*`（LiteOS 面）是这套 ops 的门面，保持现有签名与返回值语义。
- 挂载点本身（`/data`）必须有 `stat/readdir`；`chdir/execve` 也走同一解析器。
- **devfs**：`null/zero/full/random/urandom/tty/console`，后续 `ptmx`。
- **procfs**：`/proc/<pid>/{stat,status,cmdline,maps}`、`/proc/self`、
  `/proc/meminfo`、`/proc/cpuinfo`、`/proc/uptime`、`/proc/mounts`。
- **ramfs**：节点和 arena 改成堆上按需分配，去掉 32/8 KB 上限。

### 3.6 网络

- `OsSock` 挂在 `OsFile` 后面：`read/write/poll/ioctl` 复用语义；
- AF_UNIX 在核内实现（socketpair、bind/listen/accept、SOCK_STREAM/DGRAM）；
- AF_INET 接 SAL（UDP 已有；TCP 补窗口/重传/选项，先够 libc 和 wget）；
- `socket/bind/connect/listen/accept/sendto/recvfrom/sendmsg/recvmsg/
  shutdown/setsockopt/getsockopt/getsockname/getpeername` 全补；
- 阻塞/非阻塞、`SIGPIPE`、`EINTR` 与文件路径共用同一等待队列。

### 3.7 信号与作业控制

- 64 信号，`sigset_t` 内核侧 8 字节；`siginfo_t` 按 Linux 布局；
- 每线程挂起/屏蔽 + 进程级共享挂起（`CLONE_SIGHAND`）；
- 状态机：`RUN/STOPPED/CONTINUED`；`SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU` 停、
  `SIGCONT` 续；`waitpid(WUNTRACED/WCONTINUED)` 能看见；
- pgrp/session/控制终端：`setsid/setpgid/getpgrp/tcsetpgrp` 语义；
  `kill(-pgid)`、`kill(0)` 广播；
- 阻塞系统调用返回前检查未屏蔽信号 → `-EINTR`（`SA_RESTART` 自动重启）；
- `SIGPIPE` 由 pipe/socket 写产生；`sigsuspend/sigwait/rt_sigtimedwait/sigpending` 补上。

---

## 4. 里程碑

每个里程碑独立可回归：**上一级不退**，且 `C6OS_LINUX_ABI=0` 的目标不受影响。

| 里程碑 | 内容 | 验收（qemu 真跑） |
|---|---|---|
| **M13 内存与生命周期** | 页引用计数/回收；`OsAs/OsVma`；COW fork；按需分页与栈增长；`mprotect/munmap/msync/mremap` 真语义；动态任务表/fd 表/内核栈 | `make check-linuxabi-m13`：① 200 次 `fork+exec+exit` 后 `free_kb` 偏差 ≤2%；② 100 次 fork 的 COW 只多一份内存；③ 递归到 8 MB 栈不崩；④ `munmap` 后空闲页回升 |
| **M14 进程/信号/exec** | `wait4` 选项 + `waitid`；envp/shebang/CLOEXEC；`*at` dirfd；64 信号 + stop/cont + EINTR/`SA_RESTART` + `SIGPIPE`；`kill/tgkill` 进程组；`fcntl` 真语义；`readv/dup3` | `make check-linuxabi-m14`：用户态逐项 PASS；busybox `trap`/`timeout`/`kill -STOP` 行为正确 |
| **M15 VFS 与设备** | ops 分发表；统一路径解析（挂载点/`..`/符号链接/`*at`）；写类与属性类 syscall；`statfs`；devfs；ramfs 动态化 | `make check-linuxabi-m15`：busybox `cp/mv/rm/mkdir/rmdir/ln/touch/chmod/df/du/find` 矩阵 + 宿主侧校验；`ls /data` 正常；`#!/bin/sh` 脚本可直接执行 |
| **M16 事件、时间与阻塞** | 等待队列接入所有阻塞点；`select/pselect6/epoll/eventfd/timerfd/signalfd`；`clock_*`/`gettimeofday`/RTC；`O_NONBLOCK` | `make check-linuxabi-m16`：epoll 多路管道 + timerfd + signalfd 用例；`timeout 1 sleep 5` 准时；`date` 与宿主差 ≤2 s |
| **M17 终端与 /proc** | pty（`/dev/ptmx`、`/dev/pts/*`）、控制终端、作业控制；procfs 最小集；sysfs 最小集 | `make check-linuxabi-m17`：交互 shell 里 `ps/top/free/df` 有输出；Ctrl-Z + `bg`/`fg` 正常；`/proc/self/maps`、`/proc/meminfo` 可读 |
| **M18 网络** | socket 族接 SAL；AF_UNIX；阻塞/非阻塞；TCP 完善；DNS；epoll 集成 | `make check-linuxabi-m18`：qemu slirp 下 `wget` 拿 200；`nc`/`nslookup`；`getaddrinfo` 可用 |
| **M19 验收矩阵** | musl `libc-test` 子集、busybox applet 矩阵、Alpine 包冒烟、回归脚本 | `make check-linuxabi-m19`：矩阵报告 + 失败清单（带豁免理由） |
| **M20 性能与加固** | syscall 快路径、日志限速、auxv 补全、W^X、SMAP、ASLR、ptrace 调试通道 | `make check-linuxabi-m20`：性能基线不回归；W^X 用例 |

### 里程碑之间的依赖

```
M13 ──► M14 ──► M15 ──► M16 ──► M17
                 │        │
                 └──► M18 ┘        M18 依赖 M16 的等待队列/epoll
M13..M18 全绿 ──► M19
M20 独立，但建议在 M15 后做日志限速
```

---

## 5. 验收矩阵

| 测试 | 内容 | 落点 |
|---|---|---|
| `libc-test` functional | musl 官方测试里不依赖 `/proc`、网络的子集 | `tests/linux/libctest/`（按需裁剪，固定 commit） |
| busybox applet 矩阵 | 文件类、进程类、文本类、系统类四组，逐 applet 记 PASS/FAIL/豁免 | `tools/busybox-matrix.sh` |
| Alpine 基础包 | `busybox`、`musl`、`apk-tools`（本地仓库）、`openssl`、`dropbear` | `tools/alpine-smoke.sh` + 固定 minirootfs |
| 资源回归 | 反复 `fork/exec/mmap/munmap/open/close` 后 `free_kb` 回基线 | `tests/linux/churn.c` |
| 性能基线 | syscall 往返、fork+exec、pipe 吞吐、页缺页次数 | `tools/perf-linuxabi.sh`（宽松阈值，只防大退化） |
| 零回归 | L1–L8、M9、docker、alpine 全绿 | 现有 `check-*` |

---

## 6. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| 页回收/COW 的引用计数错 | 高 | 随机丢数据/漏页 | 引用计数只在 PMM 一处改；`static_assert` + 每次映射/解映射都走封装；测试先做 `churn` |
| 等待队列改造牵动所有阻塞点 | 高 | 大面积回归 | 先做"兼容层"：旧 `lx_proc_block` 改成等待队列的单等待者特例，逐点替换 |
| VFS ops 重构撞上 FAT16 写 | 中 | 写盘回退 | M15 先补分发表再迁 FAT；`check-fat-write` 每步都跑 |
| c6 编译器在复杂结构上的限制 | 中 | 工期 | 布局敏感/指针密集的留在 C；核心表结构用 c6 但避免函数指针数组初始化 |
| 单核假设被多线程/中断破坏 | 中 | 难查的竞态 | 内核不可抢占（`SFMASK` 清 IF）保持不变；等待队列只在确定点切换 |
| 时间校准误差导致 sleep 漂移 | 中 | 超时不准 | 用 PIT 多点校准 TSC，测试给 ±2 s 容差 |
| 每步都动到已验证路径 | 高 | 回归 | 每个里程碑独立 `check-*`；改一处跑一遍 L1–L8 |

---

## 7. 目录与命名规划

```
kernel/mm/        页框引用计数封装、OsAs/OsVma、COW、页缓存
kernel/proc/      任务表、pid 分配、等待队列、信号投递、作业控制
kernel/fs/        vfs ops、路径解析、devfs、procfs、ramfs/fatfs/romfs 后端
kernel/net/       SAL + socket 对象（AF_UNIX / AF_INET）
kernel/rt/        定时器队列、时钟
linux/            只留：syscall 入口/参数翻译、ELF 加载、Linux 布局（stat/sigframe/auxv）
port/x86_64/      硬件：页表、CR3、#PF、PIT/RTC、ATA、e1000
```

命名：LiteOS 有对应物的用 `LOS_*`；没有的用 `Os*`；Linux 布局的私有函数留在
`linux/` 内 `static`，不导出到内核面。

---

## 8. 明确不做（本轮）

- namespace（pid/mnt/net/uts/ipc/user）、cgroup、overlayfs、seccomp-bpf、
  capabilities、完整 ptrace —— 这些是"容器/沙箱"语义，不是"程序功能正确"的前提；
- KPTI、SMAP、完整 ASLR —— M20 视需要；
- i386/32 位 ABI、其他架构的 Linux personality；
- SMP；
- 内核内完整 TCP（拥塞控制/大量选项）——做到能支撑 libc、wget、ssh 即可；
- GUI、音频、GPU。

> "在 c6LOS 上跑 Linux 容器"这个需求不走 T4，走另一条线：**镜像 → 微虚机 + runtime shim**
> ——容器照旧、镜像照旧，隔离边界交给 VMM，见 [oci-microvm-design.md](oci-microvm-design.md)。
> 本文（T 线）只负责"镜像里的程序能跑多远"。

---

## 9. 施工记录

### M13 之前（本轮）

- 修 `Makefile` 的 `dyn-disk.img` 规则（B15），让 L5–L8 在干净树上可验收。
- L8 验收的 `[preempt] ticks=... switch=...` 在 M12-3 被当调试噪音删掉，但
  `check-linuxabi-l8` 仍在 grep 它。现在只在 pid 1 退出时打一次（交互式子进程
  不刷屏），验收与实现重新对齐。
- 本文件建立缺口清单与里程碑；后续每个里程碑在此追加"做了什么/踩到什么/验收输出"。

### M13-1 物理页生命周期（页框引用计数 + 页表真回收）

做了什么：

- `port/x86_64/mmu.c`：加"每帧一字节"引用计数（`port_phys_ref/put`，alloc 置 1，
  put 归零才回 free list）；`port_as_destroy` 真正走完 叶子页→PT→PD→PDPT→PML4
  五层并归还；`port_as_unmap` 归还叶子页，并在整张 PT 空掉时回收 PT 页
  （munmap 允许范围里有洞）；`port_as_map` 覆盖映射前先还旧页框（MAP_FIXED 语义）；
  新增 `port_as_protect`（mprotect 直接改 PTE 的 W/NX，不再假成功）；
  `port_as_ref/unref/gc` 做地址空间引用计数：线程组共享同一个 AS，最后一个成员
  退出才拆；当前 CR3 上的 AS 延后到切页表之后（`port_as_activate` 末尾）回收。
- `linux/proc.c`：clone 线程 `port_as_ref`；线程退出、组长收尸、execve 换 AS 时
  `port_as_unref`；wait4 收僵尸时才放最后一个引用。
- `linux/sys.c`：mprotect 接到 `port_as_protect`，失败返回 ENOMEM。
- 验收：`make check-linuxabi-m13`（300 轮 建/映射/改权限/解映射/覆盖映射/销毁），
  空闲物理页必须精确回到基线：`free base=257888 KB, after=257888 KB`。

踩到的坑（重要）：第一版把引用计数表放进 `.bss`（按 1GB 上限 → 256KB），内核
`__bss_end` 从 ~0x83000 涨到 0xC3BE0，正好越过 **0xA0000 的 VGA/BIOS 空洞**，
`g_kstack` 落在 0xAD488。从 ring3 进中断时 CPU 按 TSS.rsp0 压帧，写进 VGA MMIO，
QEMU 报 `qemu_mutex_lock_iothread_impl: assertion failed` 后 abort（现场看像
内核 bug，其实是被 QEMU 的 BQL 断言兜住了）。修法不是把数组改小，而是把表改成
**启动后从 PMM 取连续页、经直接映射访问**（表多大随 RAM 走，1GB 也只 256KB，
但不占内核镜像），并在 `port/x86_64/stage2.ld` 加
`ASSERT(__bss_end <= 0xA0000)`：把"镜像长过洞"从运行期 QEMU abort 变成链接期报错。

### M13-2a 写时复制 fork

做了什么：

- `port_as_copy` 不再深拷贝：可写页父子都把 PTE 的 `W` 清掉、置软件位
  `PTE_COW=(1<<9)`（置 COW **必清 W**，否则硬件写不会被拦），只读页原样共享，
  两种情况都 `port_phys_ref` 记一次引用。48MB 匿名映射 fork 6 次，深拷贝必 OOM，
  COW 只多页表页。
- 缺页路径：`x86_exc_dump` 见到 #PF（vec 14）且当前有 `LxProc` 时先调
  `port_pf_resolve(cr2, err)`。它只接管 `(err & 7) == 7`（present 的用户态写）
  且 PTE 带 `P|U|COW` 的缺页：引用计数为 1 就原地恢复可写，否则新分配一页复制、
  `port_phys_put` 旧页，最后 `invlpg` 返回 1 让用户态重试。其它缺页照旧走
  信号/异常，所以 L6 的 SIGSEGV 用例不受影响。
- `mprotect`：给 COW 页改权限前先经 `cow_break_` 私有化，避免把共享页框的属性
  一起改掉。
- `port_as_user_range_ok`：COW 页（已清 W）对内核 `copy_to/from_user` 必须仍视为
  可写，否则 fork 后第一次 `wait` 写 status 就失败。
- 顺带修一个真的 Linux ABI bug（B16）：`wait4` 的 pid 是 `int`，glibc 传 -1 时
  上游高 32 位不可信；按 u64 直接用会变成 +4294967295，进程表里找不到任何子进程，
  `wait()` 恒 `ECHILD`。入口统一 `(i64)(i32)a0`。

踩到的坑：Makefile 里 L5–L8 的载荷镜像把 `$(X86)/dyn-disk.img` 当普通文件依赖，
内核改了但镜像没重建，跑出来的是旧内核（症状是"改了没效果"，还容易误判成并发
打架）。全部改成依赖递归目标 `x86-dyn`。

验收：`make check-linuxabi-m13b` → 用户态 `PASS: cow`；回归 L2/L4/L5/L6/L7/L8/M13
全 PASS（M9-4 连跑 3 次 PASS；整套循环里偶发一次失败，属环境/镜像时序抖动，
非本次改动引入）。

### M13-2b 地址空间：内核堆 + VMA 层

M13-2b-1 内核堆：

- `port/x86_64/kmem.c`：小对象走页级 slab（7 个类 32B–2KB，页头存 magic/类号，
  空闲对象自身串链），大对象取连续页、整块归还。元信息全在页头，`.bss` 里只有
  7 个类头指针 —— 没有重蹈"表放 `.bss` 长过 0xA0000"的坑。
- `port/x86_64/mmu.c`：新增 `port_phys_alloc_pages(n)`（连续帧，绕回低端时重置
  连续性）、`port_phys_free_pages`、`port_dmap_base`；`port_mmu_init` 末尾初始化
  内核堆。
- 验收：`tests/t_x86_mem.c6` 扩用例 —— 同类对象地址不重叠、大对象释放后
  `free_kb` 回升、释放回链后还能再拿到；`check-linuxabi-m13` 仍 PASS。

M13-2b-2 VMA 层：

- `linux/vma.c` + `include/c6os/linux_vma.h`：每个 AS 一条按 start 有序的 VMA
  链（anon/file/stack/heap/shared/lazy 标记），提供 find/add/remove/protect/
  free_all；注册表本身也是 `port_kcalloc` 倍增的，没有条数上限。
- 生命周期：`port_as_set_free_hook` 让 port 层在 AS 真正拆掉时发事件，
  `linux/vma.c` 借此还掉整链并摘除条目 —— port 层不认识 VMA，不做反向依赖。
  这一条很关键：PML4 的物理地址会被下一个 AS 复用，不摘除就会把旧 VMA 算到
  别人头上（只在反复 fork 后偶发）。
- 接线：ELF 段逐页登记（相邻同属性自动合并）、栈登记为 `LX_VM_STACK`、
  mmap/MAP_FIXED/munmap/mprotect 全部记账。munmap 先作废记账再还页（反过来会
  在"页已还、VMA 还在"的窗口里被缺页重新填回来）；mprotect 对未映射区间返回
  ENOMEM，与 Linux 一致。

拆段 bug（两个，都是"先改后拷"和"边界方向写反"）：

1. `lx_vma_remove` 中间挖洞时先 `cur->end = start` 再 `*tail = *cur`，把
   `end < start` 的倒挂节点拷进链里；之后所有插入都判重叠，ld.so 加载
   libresolv 报 `failed to map segment`。
2. 同一函数"砍头部"分支把 `cur->start` 又设回 `start`（应为 `end`），前缀
   munmap 变成空操作，MAP_FIXED 覆盖时旧 VMA 残留。

教训：区间切分要么先拷贝再改，要么让新节点从旧节点派生、旧节点最后改；这类
bug 的表象是"某个共享库偶尔加载失败"，不顺着 VMA 链看根本定位不到。

验收：L4/L5/L6/L7/L8/M9/M13/M13b 全 PASS。COW 用例连跑 13 次全过；其间有一次
make 轮次里出现早期 #DE 崩溃，未能复现（同一镜像随后 13/13 通过），记为环境
抖动，若再出现按 §6 风险项单独跟踪。

### M13-2b-3 按需分页与验收

- `port_as_map_zero(as, va, prot)`：分配→清零→挂表。清零必须在挂表之前
  （`port_as_map` 的分配路径不清零，漏了就是"新进程读到上一个进程的数据"）。
- 惰性 anon mmap：`sys_mmap2` 对 `MAP_ANONYMOUS` 只记账（`LX_VM_LAZY`），
  物理页等第一次访问；MAP_FIXED 覆盖时显式 `port_as_unmap` 旧页。
- 缺页分两类：`x86_exc_dump` 里 #PF 的 err bit0=1 走 COW（`port_pf_resolve`），
  bit0=0 走 `lx_vma_fault`（惰性首访补零页 / 栈 VMA 下方 8MB 窗口内向下增长）；
  两者都不接管就照旧 `lx_sig_deliver_exc`，SIGSEGV 语义不变。
- `munmap` 先作废 VMA 再还页；`brk` 收缩真还页；`mprotect` 对惰性页只改 VMA
  （没有 PTE 可改也算成功）；`msync` 匿名页按 VMA 覆盖返回 0/ENOMEM。
- `lx_user_ok`：页表检查失败后先 `lx_vma_fault_range` 把惰性页补上再复查，
  否则内核 `copy_to_user` 会把"还没被碰过的 malloc 缓冲"误判成 EFAULT。
- 验收：新目标 `check-linuxabi-m13c`（`tests/linux/vma.c`）—— 256MB 惰性 mmap
  成功、零页初值、未触碰页 mprotect 两次成功、msync、munmap 后立刻再映射
  256MB、4.8MB 深递归（初始栈只有 128KB）不崩。`check-linuxabi-m13` 另加内核侧
  记账：64MB 惰性 VMA 只补 2 页 → `free_kb` 降幅 ≤ 64KB；AS 销毁后回升。
- 回归：L2/L4/L5/L6/L7/L8/M9/M13/M13b 全 PASS。期间 L8 与 M13b 各出现过一次
  早期 #DE 崩溃（vec=0、ring0、rip 在 0x23xxx），单独重跑即过，未能复现；
  先按环境抖动记录，若再出现按 §6 风险项抓栈跟踪。

