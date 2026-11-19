/* L5 进程模型：每进程地址空间 / 内核栈 / fd 表 / 协作调度。
 *
 * 为什么是"协作"调度：ring3 里 RFLAGS.IF=0（还不收中断），所以进程切换只发生
 * 在系统调用的边界上。对 fork/exec/wait/pipe 这条路径足够：父进程 wait4 或者
 * 读空管道时主动让出 CPU，被唤醒后再从让出点继续。真正的抢占式调度要等
 * L5+ 的 IRQ_USER（时钟中断 + TSS 栈切换），届时这里只换 pick_next 的触发源。
 *
 * 进程表不是定长数组（M13-3b）：槽位指针数组与每个进程的内核栈都来自内核堆，
 * 随真实并发进程数增长，只有 LX_PROC_MAX_CAP 这个保护性上限。内核栈仍然在
 * 切换时**不做分配**——槽位建立时就把栈准备好，切换只是换 TSS.rsp0。
 */
#ifndef C6OS_LINUX_PROC_H
#define C6OS_LINUX_PROC_H

#include <c6os/linux_abi.h>

#define LX_PROC_MAX_CAP  4096u    /* 保护性上限，不是工程上限 */
#define LX_KSTACK_SIZE   8192ull

/* 进程状态 */
#define LX_P_FREE     0u
#define LX_P_READY    1u
#define LX_P_RUN      2u
#define LX_P_ZOMBIE   3u
#define LX_P_BLOCK    4u
#define LX_P_STOP     5u

/* 阻塞原因（M14-2）：只有 stall_check_ 需要区分，用来判断"停还是死等"。
 * 进程每次 lx_proc_block() 之前自己声明，醒来后置回 LX_WAIT_NONE。 */
#define LX_WAIT_NONE  0u
#define LX_WAIT_PIPE  1u
#define LX_WAIT_TTY   2u
#define LX_WAIT_CHILD 3u
#define LX_WAIT_FUTEX 4u

/* wait4 选项（Linux x86-64 的真实位值） */
#define LX_WNOHANG     1u
#define LX_WUNTRACED   2u
#define LX_WCONTINUED  8u
#define LX_WEXITED     4u                 /* waitid: WEXITED */
#define LX_WNOWAIT     0x1000000u         /* waitid: WNOWAIT（不收尸） */

/* 作业控制 / SIGCHLD 的 si_code */
#define LX_CLD_EXITED    1
#define LX_CLD_KILLED    2
#define LX_CLD_DUMPED    3
#define LX_CLD_TRAPPED   4
#define LX_CLD_STOPPED   5
#define LX_CLD_CONTINUED 6

/* fd 表里的特殊标记（与 linux/fs.c 共用） */
#define LX_FD_NONE    0xffffffffffffffffull
#define LX_FD_CONSOLE 0xfffffffffffeull
#define LX_FD_ROOT    0xfffffffffffdull
/* /dev/* 设备节点号（M15-2），放在 node 字段，和特殊标记/普通节点不冲突 */
#define LX_DEV_NULL     0xfffffff0ull
#define LX_DEV_ZERO     0xfffffff1ull
#define LX_DEV_FULL     0xfffffff2ull
#define LX_DEV_RANDOM   0xfffffff3ull
#define LX_DEV_URANDOM  0xfffffff4ull

/* fd 种类 */
#define LX_FD_FILE    0u
#define LX_FD_PIPE_R  1u
#define LX_FD_PIPE_W  2u
#define LX_FD_VFS     3u   /* /tmp 等走内核 VFS（M9-4） */
#define LX_FD_DEV     4u   /* /dev/* 设备（M15-2）：null/zero/full/random… */
#define LX_FD_EVENTFD 5u   /* eventfd2（M16-1b）：pos = 对象 id */
#define LX_FD_PROC    6u   /* procfs 虚拟文件（M17-1）：node = id, pos = 偏移 */
#define LX_FD_SOCK    7u   /* socket（M18）：node = SAL socket id */
#define LX_FD_EPOLL   8u   /* epoll（M20）：node = 对象 id */
#define LX_FD_TIMERFD 9u   /* timerfd（M21）：node = 对象 id */
#define LX_FD_SIGNALFD 10u /* signalfd（M22）：node = 对象 id */
#define LX_FD_UNIX    11u /* AF_UNIX socket（M25）：node = 对象 id */
#define LX_FD_PTY     12u /* pty 主/从（M45）：node = 对象 id，pos=0 主/1 从 */
#define LX_DEV_PTMX     0xfffffff3ull  /* /dev/ptmx 的 dev 哨兵 */

/* procfs 虚拟文件的 id（fs.c 内部生成内容） */
#define LX_PROC_meminfo   1u
#define LX_PROC_uptime    2u
#define LX_PROC_cpuinfo   3u
#define LX_PROC_status    4u
#define LX_PROC_cmdline   5u
#define LX_PROC_stat      6u
#define LX_PROC_pidstat   7u
#define LX_PROC_pidstatus 8u
#define LX_PROC_pidcmdline 9u
#define LX_PROC_mounts    10u

/* 内核栈上的陷入帧：15 个通用寄存器（与 LxRegs 同序）+ 用户 RSP。
 * 顺序必须与 port/x86_64/user_entry.S 的压栈顺序逐字节一致，
 * 因为 x86_resume_user 直接把它当栈用。 */
typedef struct LxFrame {
    LxRegs r;
    u64    user_rsp;
} LxFrame;

/* fd 标志（fcntl F_GETFD/F_SETFD） */
#define LX_FD_CLOEXEC   0x01u

/* open 状态标志里被内核真正用到的位（fcntl F_GETFL/F_SETFL） */
#define LX_O_ACCMODE    0x03u
#define LX_O_RDONLY     0x00u
#define LX_O_WRONLY     0x01u
#define LX_O_RDWR       0x02u
#define LX_O_CREAT      0x040u
#define LX_O_TRUNC      0x200u
#define LX_O_APPEND     0x400u
#define LX_O_NONBLOCK   0x800u
#define LX_O_DIRECTORY  0x10000u
#define LX_O_CLOEXEC    0x80000u

typedef struct LxFile {
    u64 node;                 /* 文件节点号 / 管道号 / 特殊 fd 标记 */
    u64 pos;                  /* 文件读写位置 */
    u32 is_dir;
    u32 kind;                 /* LX_FD_FILE / LX_FD_PIPE_R / LX_FD_PIPE_W */
    u32 flags;                /* FD_CLOEXEC 等 fd 标志 */
    u32 oflags;               /* O_APPEND / O_NONBLOCK / 访问模式 */
    u32 lock;                 /* M52：flock 咨询锁类型（0/SH/EX），fork/dup 共享 fd 表 */
    u32 file_pad;
} LxFile;

/* 动态 fd 表（linux/fdt.c，M13-3a）。
 *
 * 以前 fd 是 PCB 里的定长数组（16 槽封顶），线程组靠"切换时整份抄给组长"假装
 * 共享。现在是一个带引用计数的对象：线程组共享同一份（clone 时 ref+1），fork
 * 才复制；容量按需倍增，槽位数组在内核堆里，不占内核镜像也没有编译期上限。 */
#define LX_FD_MAX_CAP  1024u

typedef struct LxFdTab {
    u32 refs;
    u32 cap;
    LxFile *ent;
} LxFdTab;

LxFdTab *lx_fdtab_new(void);
LxFdTab *lx_fdtab_copy(const LxFdTab *src);
void     lx_fdtab_ref(LxFdTab *t);
void     lx_fdtab_unref(LxFdTab *t);
int      lx_fdtab_grow(LxFdTab *t, u64 need);
u64      lx_fdtab_alloc(LxFdTab *t);          /* 无空闲槽返回 LX_FD_NONE */
LxFile  *lx_fdtab_get(LxFdTab *t, u64 fd);    /* 越界/空槽返回 0 */
void     lx_fdtab_release(LxFile *e);         /* 清空一个槽（fd 关闭时用） */
/* 表销毁前通知每个非空槽（管道要减 readers/writers）。fdt 不认识管道，走钩子。 */
void     lx_fdtab_set_free_hook(void (*fn)(LxFile *f));

/* L6：信号。内核侧 sa 的布局与 Linux 一致：{handler, flags, restorer, mask}，
 * 因为 glibc 的 sigaction() 就是这样转给 rt_sigaction(2) 的。
 *
 * LX_NSIG 是**数组维数**：Linux 合法信号号是 1..64（_NSIG=64），所以按信号号
 * 直接下标要 65 项。以前是 32，SIGRTMIN(34) 一来就越界。 */
#define LX_NSIG 65

typedef struct {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
} LxSigAction;

typedef struct LxProc {
    u32 state;
    u32 pid;                  /* 线程 id（gettid） */
    u32 tgid;                 /* 线程组 id（= 组长的 pid，即 getpid） */
    u32 ppid;
    u32 pad;
    u64 as;                   /* PML4 物理地址 */
    u64 kstack_top;           /* 内核栈顶（高地址，向低增长） */
    u64 ksp;                  /* 让出时保存的内核 RSP */
    LxFrame f;                /* 首次被调度时从这里恢复用户寄存器 */
    i64 code;                 /* 退出码（ZOMBIE 时有效） */
    u64 brk;
    u64 mmap_top;
    u64 fs_base;              /* TLS 基址：切进程时必须跟着换（MSR_FS_BASE 是全局的） */
    u64 set_tid;              /* set_tid_address / clone 的 CLEARTID 地址 */
    u64 clear_tid;
    u64 sig_mask;                 /* 屏蔽字（bit sig-1） */
    u64 sig_pending;              /* 挂起（bit sig-1） */
    u64 alt_sp;                   /* sigaltstack */
    u64 alt_size;
    u32 alt_flags;
    u32 killed;                   /* wait4 状态：0=正常退出，1=被信号杀 */
    u32 started;                  /* 已经进过用户态：陷入帧在内核栈顶，不在 f 里 */
    u32 irq_frame;                /* 当前活帧是"定时器中断帧"（20 个 qword） */
    u32 sig_code[LX_NSIG];
    LxSigAction sa[LX_NSIG];
    char cwd[96];                 /* 当前目录（不带前导 '/'，空串=根）：必须跟进程走 */
    LxFdTab *fd;                  /* 动态 fd 表：线程组共享，fork 时复制 */
    u32 pgid;                     /* 进程组（B1：以前 setpgid 跟 chdir 共用一个 case） */
    u32 sid;                      /* 会话 */
    u32 wait_kind;                /* 阻塞原因（LX_WAIT_*），跨系统调用有效 */
    u32 stopsig;                  /* 停止它的信号：LX_P_STOP 时有效（=报告给父进程的 WSTOPSIG） */
    u32 resumed;                  /* 已用 SIGCONT 续过：下次上报走 WIFCONTINUED 而不是静默丢弃 */
    u32 job_pad;
    u32 intr;                     /* 醒来原因是"有信号待处理"（EINTR 的判据） */
    u32 intr_sig;                 /* 那个信号号：用来查 SA_RESTART */
    u64 cont_seen;                /* 续行已发生、还没被 wait4(WCONTINUED) 取走 */
    u64 alarm_deadline;           /* alarm(2)/ITIMER_REAL：TSC 截止时刻，0=没设 */
    u64 itimer_interval[3];       /* M53：三个 itimer 的周期（cycles），0=一次性 */
    u64 itimer_deadline[3];       /* M53：VIRTUAL/PROF 的截止（cycles）；REAL 用 alarm_deadline */
    u64 rlim[LX_NLIMITS];         /* M51：rlim_cur（fork 继承），~0=INFINITY */
    u64 rlim_max[LX_NLIMITS];     /* M51：rlim_max（硬上限） */
    i32 nice;                     /* getpriority/setpriority 的 nice 值 */
    u32 proc_pad;
} LxProc;

/* 进程表与调度（linux/proc.c） */
void     lx_proc_attach(u64 as, u64 brk);      /* 把当前上下文登记成 0 号进程 */
LxProc  *lx_cur_proc(void);
u32      lx_proc_list(u32 *out, u32 cap);        /* M58：活进程 pid 列表，给 /proc 目录用 */
void     lx_proc_yield(void);                  /* 系统调用末尾让出一次 */
void     lx_proc_block(void);                  /* 标 BLOCK 并立刻切走 */
u64      lx_proc_alarm(u64 secs);              /* alarm(2)：返回旧闹钟剩余秒数 */
void     lx_proc_alarm_check(void);            /* 到点投递 SIGALRM（resched_/tick 调用） */
int      lx_proc_intr(void);                    /* 刚醒来是被信号叫醒的？ */
int      lx_proc_restart(void);                /* 该信号带 SA_RESTART？ */
void     lx_proc_exit(u64 code);               /* 不返回 */
void     lx_sched_tick(void);
u64      lx_proc_fork(LxRegs *r);
u64      lx_proc_wait4(i64 pid, u64 ustatus, u64 options, u64 urusage);
u64      lx_proc_waitid(u64 idtype, u64 id, u64 uinfo, u64 options, u64 urusage);
u64      lx_proc_execve(u64 upath, u64 uargv, u64 uenvp, LxRegs *r);
u64      lx_proc_pipe2(u64 ufds, u64 flags);
u64      lx_proc_close(u64 fd);
u64      lx_proc_dup(u64 oldfd);
u64      lx_proc_dup2(u64 oldfd, u64 newfd);
u64      lx_proc_dup3(u64 oldfd, u64 newfd, u64 flags);
u64      lx_proc_fcntl(u64 fd, u64 cmd, u64 arg);
u64      lx_proc_readv(u64 fd, u64 uiov, u64 cnt);
u64      lx_proc_writev(u64 fd, u64 uiov, u64 cnt);
u64      lx_proc_setpgid(i64 pid, i64 pgid);
u64      lx_proc_getpgid(i64 pid);
u64      lx_proc_setsid(void);
u64      lx_proc_getpgrp(void);
u64      lx_proc_tcsetpgrp(i64 fd, i64 pgid);       /* 作业控制：前台组（M14-2） */
u64      lx_proc_tcgetpgrp(i64 fd);
/* 作业控制（linux/proc.c，M14-2）：sig.c 的停止/续行默认动作调这两个。 */
void     lx_proc_job_stop(u32 pid, u32 sig);       /* SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU */
void     lx_proc_job_cont(u32 pid);                /* SIGCONT：续行指定进程（组广播由 kill 负责） */
void     lx_proc_set_wait(u32 kind);               /* 声明本次阻塞原因（LX_WAIT_*） */
u32      lx_proc_fore(void);                      /* 当前前台进程组（0 = 没有作业控制） */
u64      lx_proc_read(u64 fd, u64 ubuf, u64 len);
u64      lx_proc_write(u64 fd, u64 ubuf, u64 len);
i64      lx_proc_poll(u64 ufds, u64 nfds, i64 timeout);
i64      lx_proc_select(i32 nfds, u64 rset, u64 wset, u64 eset, i64 timeout);
u64      lx_proc_eventfd2(u64 initval, u64 flags);
u64      lx_proc_eventfd_read(u64 fd, u64 ubuf, u64 len);
u64      lx_proc_eventfd_write(u64 fd, u64 ubuf, u64 len);
/* socket（linux/socket.c，M18）：C6OS_NET=0 时链接桩返回 -EAFNOSUPPORT */
u64      lx_sock_socket(u64 domain, u64 type, u64 proto);
u64      lx_sock_close(u64 sd);
u64      lx_sock_bind(u64 sd, u64 usockaddr, u64 ulen);
u64      lx_sock_connect(u64 sd, u64 usockaddr, u64 ulen);
u64      lx_sock_listen(u64 sd, u64 backlog);
u64      lx_sock_accept(u64 sd, u64 uaddr, u64 uaddrlen);
u64      lx_sock_sendto(u64 sd, u64 ubuf, u64 len, u64 flags,
                        u64 usockaddr, u64 ulen);
u64      lx_sock_recvfrom(u64 sd, u64 ubuf, u64 len, u64 flags,
                          u64 uaddr, u64 uaddrlen);
u64      lx_sock_send(u64 sd, u64 ubuf, u64 len, u64 flags);
u64      lx_sock_recv(u64 sd, u64 ubuf, u64 len, u64 flags);
u64      lx_sock_setsockopt(u64 sd, u64 level, u64 optname,
                            u64 uval, u64 optlen);
u64      lx_sock_getsockopt(u64 sd, u64 level, u64 optname,
                            u64 uval, u64 uoptlen);
u64      lx_sock_shutdown(u64 sd, u64 how);
u64      lx_sock_getpeername(u64 sd, u64 uaddr, u64 uaddrlen);
u64      lx_sock_getsockname(u64 sd, u64 uaddr, u64 uaddrlen);
/* M23：sendmsg/recvmsg（iovec 展开，控制消息暂忽略） */
u64      lx_sock_sendmsg(u64 sd, u64 umsg, u64 flags);
u64      lx_sock_recvmsg(u64 sd, u64 umsg, u64 flags);
/* M24：轮询式网卡——等待路径每轮泵一次，收 ARP 应答/DNS 响应 */
void     lx_net_pump(void);
u16      lx_sock_poll_one(u64 sd);   /* 返回 LX_POLLIN/POLLOUT 等就绪位 */
/* M25：AF_UNIX socket（linux/afunix.c），复用主 fd 表 */
u64      lx_unix_socket(u64 type);
u64      lx_unix_bind(u64 fd, u64 uaddr, u64 ulen);
u64      lx_unix_connect(u64 fd, u64 uaddr, u64 ulen);
u64      lx_unix_listen(u64 fd, u64 backlog);
u64      lx_unix_accept(u64 fd, u64 uaddr, u64 uaddrlen);
u64      lx_unix_sendmsg(u64 fd, u64 umsg, u64 flags);
u64      lx_unix_recvmsg(u64 fd, u64 umsg, u64 flags);
u64      lx_unix_sendto(u64 fd, u64 ubuf, u64 len, u64 uaddr, u64 ulen);
u64      lx_unix_recvfrom(u64 fd, u64 ubuf, u64 len, u64 uaddr, u64 uaddrlen);
u64      lx_unix_close(u64 fd);
u16      lx_unix_poll(u64 id);
void     lx_unix_wake(void);
/* SCM_RIGHTS：把 fd 状态副本安装到当前进程 fd 表，并按 kind 增引用。
 * 返回新 fd 或负 errno。src 是 LxFile 值副本。 */
u64      lx_unix_fd_install(const LxFile *src);
void     lx_unix_fd_acquire(u32 node, u32 kind);
void     lx_sock_poll(int sd, u64 ubuf, u64 nfds); /* fill pollfd revents */
/* M19：busybox id/df/cat 依赖 */
u64      lx_proc_getgroups(u64 size, u64 ugroups);
u64      lx_proc_sysinfo(u64 ubuf);
u64      lx_proc_sendfile(u64 outfd, u64 infd, u64 uoff, u64 count);
/* M20：epoll（linux/proc.c） */
u64      lx_epoll_create1(u64 flags);
u64      lx_epoll_ctl(u64 efd, u64 op, u64 fd, u64 uev);
u64      lx_epoll_wait(u64 efd, u64 uevents, u64 maxevents, i64 timeout);
/* M21：timerfd */
u64      lx_timerfd_create(u64 clockid, u64 flags);
u64      lx_timerfd_settime(u64 fd, u64 flags, u64 unew, u64 uold);
u64      lx_timerfd_gettime(u64 fd, u64 ucur);
u64      lx_timerfd_read(u64 fd, u64 ubuf, u64 len);
void     lx_timerfd_check(void);
/* M22：signalfd */
u64      lx_signalfd4(u64 fd, u64 umask, u64 masklen, u64 flags);
u64      lx_signalfd_read(u64 fd, u64 ubuf, u64 len);
/* sig.c 投递时把被 signalfd 监控的信号转入队列；返回 1=已转走 */
int      lx_signalfd_queue(u64 sig, u64 code, u64 pid);
void     lx_proc_exit_sig(u32 sig);
void     lx_proc_exit_group(u64 code);
void     lx_sched_preempt(void);
u64      lx_proc_clone_thread(LxRegs *r, u64 flags, u64 child_stack, u64 ptid,
                              u64 ctid, u64 tls);
u64      lx_proc_set_tid_address(u64 ptr);
u64      lx_futex(u64 uaddr, u32 op, u32 val, u64 utime);
u32      lx_proc_tgid(void);
u32      lx_proc_tid(void);
LxProc  *lx_proc_find(i64 pid);
void     lx_proc_wake(LxProc *p);

/* L6 信号（linux/sig.c） */
void lx_sig_reset(LxProc *p);
void lx_sig_fork(LxProc *p, const LxProc *par);
void lx_sig_exec(LxProc *p);
u64  lx_sig_action(u32 sig, u64 uact, u64 uold);
u64  lx_sig_procmask(u32 how, u64 uset, u64 uold);
u64  lx_sig_altstack(u64 uss, u64 uoss);
u64  lx_sig_timedwait(u64 uset, u64 uinfo, u64 utimeout, u64 sigsetsize);  /* M55 */
void lx_sig_send_one(LxProc *t, u32 sig, int to_group);   /* 投递给单个进程 */
int  lx_proc_send(i64 pid, u32 sig);                      /* kill(2) 选目标 + 广播 */
u64  lx_sig_return(LxRegs *r);
void lx_sig_deliver_frame(LxProc *p, LxFrame *f);
int  lx_sig_deliver_exc(LxProc *p, LxRegs *r, u64 cr2, u32 vec);

/* 内存状态随进程切换（linux/sys.c 提供） */
u64  lx_brk_get(void);
void lx_brk_set(u64 v);
u64  lx_mmap_top_get(void);
void lx_mmap_top_set(u64 v);
void lx_set_as(u64 as);
u64  lx_console_write(u64 fd, u64 ubuf, u64 len);
u64  lx_console_put(u64 ubuf, u64 len);             /* 按 node 判定后直写控制台 */
void lx_fs_set_fdtab(LxFdTab *t);                  /* linux/fs.c */
LxFdTab *lx_fs_get_fdtab(void);                    /* 登记进程时接管兜底表 */
/* M15：写类/属性类 syscall（linux/fs.c；sys.c 只做分派） */
u64  lx_fs_mkdirat(u64 dirfd, u64 upath, u64 mode);
u64  lx_fs_rmdir(u64 upath);
u64  lx_fs_unlinkat(u64 dirfd, u64 upath, u64 flags);
u64  lx_fs_renameat(u64 odirfd, u64 oldpath, u64 ndirfd, u64 newpath);
u64  lx_fs_ftruncate(u64 fd, u64 len);
u64  lx_fs_truncate(u64 upath, u64 len);
u64  lx_fs_chmod(u64 upath, u64 mode);
u64  lx_fs_chmod_pathat(u64 dirfd, u64 upath, u64 mode);
u64  lx_fs_statfs(u64 upath, u64 ubuf);          /* x86-64 struct statfs, 120B */
u64  lx_fs_utimensat(u64 dirfd, u64 upath, u64 times);

/* ---- 串口控制台的行规程（linux/tty.c，M12）----------------------------
 * 默认关着：fd 0 以前是"永远 EOF"，L2..L6 的既有验收（busybox sh 的管道
 * 那套）依赖这个行为。要让 0 号 fd 变成真终端，init 里显式调 lx_tty_init()。 */
void lx_tty_init(void);
u32  lx_tty_enabled(void);
u32  lx_tty_pump(void);                           /* 泵一次串口，返回吃到的字节数 */
u32  lx_tty_ready(void);                          /* 有整行可读 / 有 EOF 挂起 */
u64  lx_tty_read(u64 ubuf, u64 len, void (*yield_fn)(void));
u64  lx_tty_write(u64 ubuf, u64 len);             /* 输出侧：应答 ESC[6n 等 DSR */
u64  lx_console_put_k(u64 kbuf, u64 len);         /* M58：内核缓冲写控制台，不查用户权限 */
i64  lx_tty_ioctl(u64 fd, u64 req, u64 arg);

/* ---- pty（linux/pty.c，M45）：/dev/ptmx + /dev/pts/N ------------------- */
u64  lx_pty_open_master(void);              /* 返回 LxFile.node（对象 id）或负 errno */
u64  lx_pty_open_slave(u64 id);             /* 打开从设备，返回 node 或负 errno */
u64  lx_pty_read(u64 node, u64 ubuf, u64 len, u32 is_master);
u64  lx_pty_write(u64 node, u64 ubuf, u64 len, u32 is_master);
u64  lx_pty_write_k(u64 node, u64 kbuf, u64 len, u32 is_master);   /* M58：内核缓冲 */
void lx_pty_close(u64 node, u32 is_master);
i64  lx_pty_ioctl(u64 node, u64 req, u64 arg, u32 is_master);
u32  lx_pty_is_tty(u64 node);

#endif
