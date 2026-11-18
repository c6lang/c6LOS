#ifndef C6OS_LINUX_ABI_H
#define C6OS_LINUX_ABI_H

#include "c6os/types.h"
#include "c6os/config.h"
#include "c6os/port.h"

/* Linux ELF 二进制兼容层（personality）的内部契约。
 *
 * 注意：本层用 C 而不是 c6 编写，这是**刻意的**——它必须逐字节复现 Linux 的
 * C ABI 产物（ELF64 结构、auxv、stat/sigaction 布局）。用 c6 的 LP64 模型去
 * 描述 ILP32/LP64 混合的 ABI 表，正是 architecture.md §3 警告的那类风险。
 * 规则：linux/ 下的文件只有在 C6OS_LINUX_ABI=1 时才进入编译单元列表。 */

/* execve 的参数区上限（Linux 的 ARG_MAX 语义：指针个数与总字节两个维度）。
 * 这两个值同时被 linux/proc.c（读用户 argv/envp）和 linux/elf64.c（压栈）用，
 * 放在这里避免两处各写一份而漂移。 */
#define LX_ARG_MAX    4096u
#define LX_ARG_BYTES  131072ull       /* 单个参数字符串上限 128KB（同 Linux） */

/* mmap 的标志（只列用到的） */
#define LX_AT_EMPTY_PATH  0x1000ull
#define LX_AT_SYMLINK_NOFOLLOW 0x100ull
#define LX_MAP_SHARED     0x01ull
#define LX_MAP_PRIVATE    0x02ull
#define LX_MAP_FIXED      0x10ull
#define LX_MAP_ANONYMOUS  0x20ull

/* x86-64 Linux 系统调用号（只列已实现/即将实现的） */
#define LX_SYS_read            0
#define LX_SYS_write           1
#define LX_SYS_open            2
#define LX_SYS_close           3
#define LX_SYS_stat            4
#define LX_SYS_fstat           5
#define LX_SYS_lstat           6
#define LX_SYS_nanosleep      35
#define LX_SYS_alarm          37
#define LX_SYS_clock_nanosleep 230
#define LX_SYS_clock_gettime 228
#define LX_SYS_lseek           8
#define LX_SYS_mmap            9
#define LX_SYS_mprotect       10
#define LX_SYS_munmap         11
#define LX_SYS_brk            12
#define LX_SYS_ioctl          16
#define LX_SYS_pipe           22
#define LX_SYS_dup            32
#define LX_SYS_dup2           33
#define LX_SYS_rt_sigaction   13
#define LX_SYS_rt_sigprocmask 14
#define LX_SYS_clone          56
#define LX_SYS_fork           57
#define LX_SYS_vfork          58
#define LX_SYS_execve         59
#define LX_SYS_wait4          61
#define LX_SYS_fcntl          72
#define LX_SYS_chdir          80
#define LX_SYS_umask          95
#define LX_SYS_getppid       110
#define LX_SYS_setpgid       109
#define LX_SYS_getpgrp       111
#define LX_SYS_setsid        112
#define LX_SYS_getpgid       121     /* ash 的 job control 用它，必须和 getpgrp 同值 */
#define LX_SYS_sigaltstack   131
#define LX_SYS_pipe2         293

/* poll(2) 的事件位（与 Linux 一致） */
#define LX_POLLIN   0x0001
#define LX_POLLOUT  0x0004
#define LX_POLLERR  0x0008
#define LX_POLLHUP  0x0010
#define LX_POLLNVAL 0x0020
#define LX_SYS_pread64        17
#define LX_SYS_readv          19
#define LX_SYS_dup3          292
#define LX_SYS_writev         20
#define LX_SYS_access         21
#define LX_SYS_exit           60
#define LX_SYS_ioctl          16
#define LX_SYS_pipe           22
#define LX_SYS_dup            32
#define LX_SYS_dup2           33
#define LX_SYS_rt_sigaction   13
#define LX_SYS_rt_sigprocmask 14
#define LX_SYS_clone          56
#define LX_SYS_fork           57
#define LX_SYS_vfork          58
#define LX_SYS_execve         59
#define LX_SYS_wait4          61
#define LX_SYS_fcntl          72
#define LX_SYS_chdir          80
#define LX_SYS_umask          95
#define LX_SYS_getppid       110
#define LX_SYS_setpgid       109
#define LX_SYS_getpgrp       111
#define LX_SYS_setsid        112
#define LX_SYS_getpgid       121     /* ash 的 job control 用它，必须和 getpgrp 同值 */
#define LX_SYS_sigaltstack   131
#define LX_SYS_pipe2         293

/* poll(2) 的事件位（与 Linux 一致） */
#define LX_POLLIN   0x0001
#define LX_POLLOUT  0x0004
#define LX_POLLERR  0x0008
#define LX_POLLHUP  0x0010
#define LX_POLLNVAL 0x0020
#define LX_SYS_getuid        102
#define LX_SYS_getgid        104
#define LX_SYS_setuid        105
#define LX_SYS_setgid        106
#define LX_SYS_geteuid       107
#define LX_SYS_getegid       108
#define LX_SYS_getpid         39
#define LX_SYS_waitid         247
#define LX_SYS_clone          56
#define LX_SYS_execve         59
#define LX_SYS_wait4          61
#define LX_SYS_uname          63
#define LX_SYS_time          201
#define LX_SYS_getcwd         79
#define LX_SYS_chdir          80
#define LX_SYS_poll          7
#define LX_SYS_ppoll         271
#define LX_SYS_arch_prctl    158
#define LX_SYS_gettid        186
#define LX_SYS_getdents64    217
/* clone(2) 的共享标志（与 Linux 同值，只列用到的） */
#define LX_CLONE_VM              0x00000100ull
#define LX_CLONE_FS              0x00000200ull
#define LX_CLONE_FILES           0x00000400ull
#define LX_CLONE_SIGHAND         0x00000800ull
#define LX_CLONE_THREAD          0x00010000ull
#define LX_CLONE_SETTLS          0x00080000ull
#define LX_CLONE_PARENT_SETTID   0x00100000ull
#define LX_CLONE_CHILD_CLEARTID  0x00200000ull
#define LX_CLONE_CHILD_SETTID    0x01000000ull

#define LX_SYS_sched_yield   24
#define LX_SYS_msync         26
#define LX_SYS_madvise       28
#define LX_SYS_prctl        157
#define LX_SYS_sched_getaffinity 204
#define LX_SYS_rt_sigreturn   15
#define LX_SYS_kill           62
#define LX_SYS_tgkill         234
#define LX_SYS_futex         202

/* 信号编号（x86-64 Linux 原值） */
#define LX_SIGHUP   1
#define LX_SIGINT   2
#define LX_SIGQUIT  3
#define LX_SIGILL   4
#define LX_SIGTRAP  5
#define LX_SIGABRT  6
#define LX_SIGBUS   7
#define LX_SIGFPE   8
#define LX_SIGKILL  9
#define LX_SIGUSR1  10
#define LX_SIGSEGV  11
#define LX_SIGUSR2  12
#define LX_SIGPIPE  13
#define LX_SIGALRM  14
#define LX_SIGTERM  15
#define LX_SIGCHLD  17
#define LX_SIGCONT  18
#define LX_SIGSTOP  19
#define LX_SIGTSTP  20
#define LX_SIGTTIN  21
#define LX_SIGTTOU  22
#define LX_SIGURG   23
#define LX_SIGXCPU  24
#define LX_SIGWINCH 28

/* handler 的两个特殊值 + sigaction 标志（与 Linux 同值） */
#define LX_SIG_DFL       0ull
#define LX_SIG_IGN       1ull
#define LX_SA_SIGINFO    4ull
#define LX_SA_RESTORER   0x04000000ull
#define LX_SA_NODEFER    0x40000000ull
#define LX_SA_RESTART    0x10000000ull

/* rt_sigprocmask 的 how */
#define LX_SIG_BLOCK     0ull
#define LX_SIG_UNBLOCK   1ull
#define LX_SIG_SETMASK   2ull

/* siginfo.si_code */
#define LX_SI_USER       0
#define LX_SI_KERNEL     (-0x80)
#define LX_SYS_set_tid_address   218
#define LX_SYS_exit_group    231
#define LX_SYS_openat        257
#define LX_SYS_mkdirat       258
#define LX_SYS_newfstatat    262
#define LX_SYS_statx         332
#define LX_SYS_copy_file_range 326
#define LX_SYS_fallocate     285
#define LX_SYS_pwrite64      18
#define LX_SYS_preadv        295
#define LX_SYS_pwritev       296
#define LX_SYS_preadv2       327
#define LX_SYS_pwritev2      328
#define LX_FALLOC_FL_KEEP_SIZE   0x01u
#define LX_FALLOC_FL_PUNCH_HOLE  0x02u
#define LX_SYS_unlinkat      263
#define LX_SYS_renameat      264
#define LX_SYS_readlinkat    267
#define LX_SYS_fchmodat      268
/* 写类/属性类（legacy 与 *at 对，M15） */
#define LX_SYS_rename        82
#define LX_SYS_mkdir         83
#define LX_SYS_rmdir         84
#define LX_SYS_unlink        87
#define LX_SYS_chmod         90
#define LX_SYS_fchmod        91
#define LX_SYS_truncate      76
#define LX_SYS_ftruncate     77
#define LX_SYS_statfs        137
#define LX_SYS_fstatfs       138
#define LX_SYS_utimensat     280
/* M16：事件与时间 */
#define LX_SYS_select        23
#define LX_SYS_gettimeofday  96
#define LX_SYS_clock_settime 227
#define LX_SYS_clock_getres  229
#define LX_SYS_pselect6      270
#define LX_SYS_eventfd2      290
#define LX_SYS_epoll_create1 291
/* M20：epoll 事件接口 */
#define LX_SYS_epoll_wait    232
#define LX_SYS_epoll_pwait   281
#define LX_SYS_epoll_ctl     233
/* M21：timerfd */
#define LX_SYS_timerfd_create 283
#define LX_SYS_timerfd_settime 286
#define LX_SYS_timerfd_gettime 287
/* M22：signalfd */
#define LX_SYS_signalfd4      289
/* M18：socket 族 */
#define LX_SYS_socket        41
/* M19：busybox applet 矩阵暴露的缺口 */
#define LX_SYS_sendfile      40
#define LX_SYS_sysinfo       99
#define LX_SYS_getgroups     115
#define LX_SYS_connect       42
#define LX_SYS_accept        43
#define LX_SYS_sendto        44
#define LX_SYS_recvfrom      45
#define LX_SYS_sendmsg       46
#define LX_SYS_recvmsg       47
#define LX_SYS_shutdown      48
#define LX_SYS_bind          49
#define LX_SYS_listen        50
#define LX_SYS_getsockname   51
#define LX_SYS_getpeername   52
#define LX_SYS_socketpair    53
#define LX_SYS_send          64
#define LX_SYS_recv          65
#define LX_SYS_setsockopt    54
#define LX_SYS_getsockopt    55
#define LX_SYS_set_robust_list   273
#define LX_SYS_prlimit64     302
#define LX_SYS_getrandom     318
#define LX_SYS_rseq          334
/* M51：资源/调度类 syscall */
#define LX_SYS_getrlimit     97
#define LX_SYS_getrusage     98
#define LX_SYS_getpriority   140
#define LX_SYS_setpriority   141
#define LX_SYS_mlock         149
#define LX_SYS_munlock       150
#define LX_SYS_setrlimit     160
#define LX_SYS_sethostname   170
/* RLIMIT_* 资源 id（Linux 通用） */
#define LX_RLIMIT_CPU        0u
#define LX_RLIMIT_FSIZE      1u
#define LX_RLIMIT_DATA       2u
#define LX_RLIMIT_STACK      3u
#define LX_RLIMIT_CORE       4u
#define LX_RLIMIT_RSS        5u
#define LX_RLIMIT_NPROC      6u
#define LX_RLIMIT_NOFILE     7u
#define LX_RLIMIT_MEMLOCK    8u
#define LX_RLIMIT_AS         9u
#define LX_RLIMIT_LOCKS     10u
#define LX_RLIMIT_SIGPENDING 11u
#define LX_RLIMIT_MSGQUEUE  12u
#define LX_RLIMIT_NICE      13u
#define LX_RLIMIT_RTPRIO    14u
#define LX_RLIMIT_RTTIME    15u
#define LX_NLIMITS          16u
#define LX_RLIM_INFINITY    (~0ull)

/* arch_prctl 的 code */
#define LX_ARCH_SET_FS    0x1002u
#define LX_ARCH_GET_FS    0x1003u

/* errno（负值返回） */
#define LX_EPERM   1
#define LX_ENOENT  2
#define LX_EBADF   9
#define LX_EFAULT  14
#define LX_ENOMEM  12
#define LX_EINVAL  22
#define LX_ENOSYS  38
#define LX_ENOTTY  25
#define LX_ENOEXEC 8
#define LX_ECHILD  10
#define LX_EAGAIN  11
#define LX_EPIPE   32
#define LX_EBUSY   16
#define LX_EMFILE  24
#define LX_ESRCH   3
#define LX_E2BIG   7
#define LX_EINTR   4
#define LX_ENOTDIR 20
#define LX_ENAMETOOLONG 36
#define LX_EISDIR  21
#define LX_EEXIST  17
#define LX_EACCES  13
#define LX_EROFS   30
#define LX_ENOSPC  28
#define LX_EAFNOSUPPORT 97
#define LX_EOPNOTSUPP 95
#define LX_EIO     5
#define LX_ESPIPE  29
#define LX_ECONNREFUSED 111
#define LX_EADDRINUSE 98

/* 与 port/x86_64/user_entry.S 的压栈顺序严格对应（低地址在前）。 */
typedef struct {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
} LxRegs;

/* 映像需要多少字节（max(p_offset+p_filesz)）；0 表示不是有效 ELF。
 * 内核据此决定从磁盘读多少扇区。 */
u64 lx_elf_image_size(u64 img_pa, u64 max_bytes);

/* L2/L4：装载一个 ELF64 映像到 as。ET_EXEC 忽略 base（vaddr 即绝对地址），
 * ET_DYN（PIE / ld.so）所有地址加 base。 */
int lx_elf_load(u64 as, u64 img_pa, u64 img_size, u64 base,
                u64 *o_entry, u64 *o_phdr, u64 *o_phnum, u64 *o_phentsize,
                u64 *o_brk_end);

/* 一步做完"装载主程序 + 解释器（若有）+ 建栈 + 登记进程"。
 * c6 侧只需要：lx_exec_prepare → lx_exec_entry/lx_exec_rsp → port_user_enter。 */
int lx_exec_prepare(u64 as, u64 img_pa, u64 img_size, const char *path);
/* 同上，但带完整 argv —— 用来跑 `sh -c '脚本'` 这类不依赖交互的确定性入口。 */
int lx_exec_preparev(u64 as, u64 img_pa, u64 img_size, const char *path,
                     const char *const *argv, u32 argc);
void lx_exec_arg1(const char *a);
int lx_exec_build(u64 as, u64 img_pa, u64 img_size, const char *path,
                  const char *const *argv, u32 argc,
                  const char *const *envp, u32 envc,
                  u64 *o_entry, u64 *o_rsp, u64 *o_brk);
u64 lx_exec_entry(void);
u64 lx_exec_rsp(void);

/* 登记当前进程（系统调用层用）。 */
void lx_proc_init(u64 as, u64 brk_start);
u64  lx_proc_brk(void);

/* 只读 initramfs（newc cpio）+ fd 表 + 文件类系统调用。 */
int  lx_fs_init(u64 pa, u64 len);
void lx_fs_fd_reset(void);
u32  lx_fs_nodes(void);
u64  lx_fs_node_size(u32 i);
u64  lx_fs_node_data_pa(u32 i);
u32  lx_fs_node_mode(u32 i);
u64  lx_fs_openat(u64 dirfd, u64 upath, u64 flags, u64 mode);
/* cd / pwd：cwd 是内核里的一份（多进程共享，见 fs.c 的说明） */
u64  lx_fs_chdir(u64 upath);
const char *lx_fs_cwd(void);
void lx_fs_set_cwd(char *p);                        /* 切进程时换 cwd 指针 */
u64  lx_fs_read(u64 fd, u64 ubuf, u64 len);
u64  lx_fs_close(u64 fd);
u64  lx_fs_lseek(u64 fd, u64 off, u64 whence);
u64  lx_fs_fstat(u64 fd, u64 ustat);
u64  lx_fs_statat(u64 dirfd, u64 upath, u64 ustat, u64 flags);
u64  lx_fs_statx(u64 dirfd, u64 upath, u64 flags, u64 ustatx);
u64  lx_fs_copy_file_range(u64 fd_in, u64 uoff_in, u64 fd_out,
                           u64 uoff_out, u64 len, u64 flags);
u64  lx_fs_fallocate(u64 fd, u64 mode, u64 off, u64 len);
u64  lx_fs_access(u64 upath, u64 mode);
u64  lx_fs_write(u64 fd, u64 ubuf, u64 len);
u64  lx_fs_getdents64(u64 fd, u64 ubuf, u64 len);
u64  lx_fs_fd_size(u64 fd);
u64  lx_fs_pread(u64 fd, u64 ubuf, u64 len, u64 off);
u64  lx_fs_pwrite(u64 fd, u64 ubuf, u64 len, u64 off);
u64  lx_fs_preadv(u64 fd, u64 uiov, u64 cnt, u64 off);
u64  lx_fs_pwritev(u64 fd, u64 uiov, u64 cnt, u64 off);
u64  lx_fs_pread_k(u64 fd, u64 kdst, u64 len, u64 off);
/* 内核侧路径查询（不做用户态权限检查）：给 ELF 加载器找解释器用。 */
u64  lx_fs_size_of(const char *path);
u64  lx_fs_pa_of(const char *path);

/* 系统调用分发（从汇编进来，可改写 r->rax 作为返回值）。 */
void lx_syscall(LxRegs *r);

/* 简易用户内存访问（L2：VMA 范围校验；L5 换 .extable + 缺页修复） */
int lx_user_ok(u64 va, u64 len, u32 prot);
u64 lx_copy_from_user(void *dst, u64 src, u64 len);

#endif
