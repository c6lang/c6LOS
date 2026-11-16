/* L5 进程模型：每进程地址空间 / 内核栈 / fd 表 / 协作调度。
 *
 * 为什么是"协作"调度：ring3 里 RFLAGS.IF=0（还不收中断），所以进程切换只发生
 * 在系统调用的边界上。对 fork/exec/wait/pipe 这条路径足够：父进程 wait4 或者
 * 读空管道时主动让出 CPU，被唤醒后再从让出点继续。真正的抢占式调度要等
 * L5+ 的 IRQ_USER（时钟中断 + TSS 栈切换），届时这里只换 pick_next 的触发源。
 *
 * 每个进程的内核栈是**静态**的（LX_MAX_PROC × LX_KSTACK_SIZE），这样切换时
 * 不需要在中断上下文里分配内存，也不会被 PMM 耗尽卡死。
 */
#ifndef C6OS_LINUX_PROC_H
#define C6OS_LINUX_PROC_H

#include <c6os/linux_abi.h>

#define LX_MAX_PROC      8u
#define LX_KSTACK_SIZE   8192ull

/* 进程状态 */
#define LX_P_FREE     0u
#define LX_P_READY    1u
#define LX_P_RUN      2u
#define LX_P_ZOMBIE   3u
#define LX_P_BLOCK    4u

/* fd 表里的特殊标记（与 linux/fs.c 共用） */
#define LX_FD_NONE    0xffffffffffffffffull
#define LX_FD_CONSOLE 0xfffffffffffeull
#define LX_FD_ROOT    0xfffffffffffdull

/* fd 种类 */
#define LX_FD_FILE    0u
#define LX_FD_PIPE_R  1u
#define LX_FD_PIPE_W  2u
#define LX_FD_VFS     3u   /* /tmp 等走内核 VFS（M9-4） */

/* 内核栈上的陷入帧：15 个通用寄存器（与 LxRegs 同序）+ 用户 RSP。
 * 顺序必须与 port/x86_64/user_entry.S 的压栈顺序逐字节一致，
 * 因为 x86_resume_user 直接把它当栈用。 */
typedef struct LxFrame {
    LxRegs r;
    u64    user_rsp;
} LxFrame;

typedef struct LxFile {
    u64 node;                 /* 文件节点号 / 管道号 / 特殊 fd 标记 */
    u64 pos;                  /* 文件读写位置 */
    u32 is_dir;
    u32 kind;                 /* LX_FD_FILE / LX_FD_PIPE_R / LX_FD_PIPE_W */
} LxFile;

/* L6：信号。内核侧 sa 的布局与 Linux 一致：{handler, flags, restorer, mask}，
 * 因为 glibc 的 sigaction() 就是这样转给 rt_sigaction(2) 的。 */
#define LX_NSIG 32

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
    LxFile fd[C6OS_LINUX_MAX_FD];
} LxProc;

/* 进程表与调度（linux/proc.c） */
void     lx_proc_attach(u64 as, u64 brk);      /* 把当前上下文登记成 0 号进程 */
LxProc  *lx_cur_proc(void);
void     lx_proc_yield(void);                  /* 系统调用末尾让出一次 */
void     lx_proc_block(void);                  /* 标 BLOCK 并立刻切走 */
void     lx_proc_exit(u64 code);               /* 不返回 */
void     lx_sched_tick(void);
u64      lx_proc_fork(LxRegs *r);
u64      lx_proc_wait4(i64 pid, u64 ustatus, u64 options, u64 urusage);
u64      lx_proc_execve(u64 upath, u64 uargv, u64 uenvp, LxRegs *r);
u64      lx_proc_pipe2(u64 ufds, u64 flags);
u64      lx_proc_close(u64 fd);
u64      lx_proc_dup(u64 oldfd);
u64      lx_proc_dup2(u64 oldfd, u64 newfd);
u64      lx_proc_read(u64 fd, u64 ubuf, u64 len);
u64      lx_proc_write(u64 fd, u64 ubuf, u64 len);
i64      lx_proc_poll(u64 ufds, u64 nfds, i64 timeout);
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
u64  lx_sig_kill(i64 pid, u32 sig);
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
void lx_fs_set_fdtab(LxFile *t);                  /* linux/fs.c */

#endif
