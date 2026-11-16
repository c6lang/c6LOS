/* linux/sig.c — L6：信号
 *
 * 只做"内核视角"的信号语义：每进程的 handler/mask/pending，投递时在用户栈上
 * 压一个 **Linux 布局**的 rt_sigframe（pretcode + ucontext + siginfo），再把
 * iret 帧改成"跳进 handler"。restorer 是 glibc 的 __restore_rt（sigaction 时
 * 随 SA_RESTORER 一起传进来），它执行 rt_sigreturn(15) 回到内核，我们把整份
 * 上下文恢复回去 —— 于是"handler 正常返回"和"handler 里 longjmp 跑掉"两条路
 * 都能走通。
 *
 * 帧布局（相对帧基址，全部按 Linux x86-64 的偏移，程序里 《ucontext_t》 能用）：
 *
 *   0      pretcode          （返回地址：restorer）
 *   8      ucontext
 *            +0   uc_flags
 *            +8   uc_link
 *            +16  uc_stack.ss_sp
 *            +24  uc_stack.ss_flags
 *            +32  uc_stack.ss_size
 *            +48  uc_mcontext.gregs[0..22]  （R8..R15, RDI, RSI, RBP, RBX,
 *                                            RDX, RAX, RCX, RSP, RIP, EFL,
 *                                            CSGSFS, ERR, TRAPNO, OLDMASK, CR2）
 *            +304 uc_sigmask
 *   976    siginfo_t         （si_signo / si_errno / si_code）
 *
 * 注意：本文件的 gregs 顺序与 LxRegs 的压栈顺序**不同**，不要互抄。
 */
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"
#include "c6os/port.h"

/* 帧内偏移 */
#define LX_SF_PRETCODE   0ull
#define LX_SF_UC         8ull
#define LX_SF_INFO       976ull
#define LX_SF_SIZE       1280ull

/* ucontext 内偏移（相对 uc） */
#define LX_UC_STACK      16ull
#define LX_UC_GREGS      48ull
#define LX_UC_SIGMASK    304ull

/* gregs 下标（Linux x86-64） */
#define LX_GREGS_RSP     15u
#define LX_GREGS_RIP     16u
#define LX_GREGS_EFL     17u

/* 系统调用陷入帧里"用户 RSP"的位置（LxFrame.user_rsp，见 user_entry.S） */
#define LX_FRAME_USER_RSP 120ull

static void zero_(u64 base, u64 n)
{
    u64 i;

    for (i = 0ull; i < n; i += 8ull) {
        *(u64 *)(base + i) = 0ull;
    }
}

static void put_dec_(u64 v)
{
    char b[24];
    u32 k = 0u;

    if (v == 0ull) {
        port_putc('0');
        return;
    }
    while (v > 0ull && k < 23u) {
        b[k] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
        k++;
    }
    while (k > 0u) {
        k--;
        port_putc((int)b[k]);
    }
}

/* 一份"用户可见上下文"的视图：系统调用帧和异常帧的 RIP/RSP/RFLAGS 位置不同，
 * 这里用指针统一起来，投递逻辑只认这一组指针。 */
typedef struct {
    LxRegs *r;
    u64    *rip;
    u64    *rsp;
    u64    *rfl;
} SigView;

void lx_sig_reset(LxProc *p)
{
    u32 i;

    p->sig_pending = 0ull;
    p->sig_mask = 0ull;
    p->alt_sp = 0ull;
    p->alt_flags = 2u;                      /* SS_DISABLE */
    p->alt_size = 0ull;
    for (i = 0u; i < (u32)LX_NSIG; i++) {
        p->sa[i].handler = LX_SIG_DFL;
        p->sa[i].flags = 0ull;
        p->sa[i].restorer = 0ull;
        p->sa[i].mask = 0ull;
        p->sig_code[i] = (u32)LX_SI_USER;
    }
}

void lx_sig_fork(LxProc *p, const LxProc *par)
{
    u32 i;

    p->sig_pending = 0ull;                  /* 挂起的信号不继承 */
    p->sig_mask = par->sig_mask;
    p->alt_sp = par->alt_sp;
    p->alt_flags = par->alt_flags;
    p->alt_size = par->alt_size;
    for (i = 0u; i < (u32)LX_NSIG; i++) {
        p->sa[i] = par->sa[i];
        p->sig_code[i] = (u32)LX_SI_USER;
    }
}

/* execve：handler 全部回到 SIG_DFL（被 SIG_IGN 的保持忽略），屏蔽字与挂起保留。 */
void lx_sig_exec(LxProc *p)
{
    u32 i;

    for (i = 0u; i < (u32)LX_NSIG; i++) {
        if (p->sa[i].handler == LX_SIG_IGN) {
            continue;
        }
        p->sa[i].handler = LX_SIG_DFL;
        p->sa[i].flags = 0ull;
        p->sa[i].restorer = 0ull;
        p->sa[i].mask = 0ull;
    }
    p->alt_sp = 0ull;
    p->alt_flags = 2u;
    p->alt_size = 0ull;
}

u64 lx_sig_action(u32 sig, u64 uact, u64 uold)
{
    LxProc *p = lx_cur_proc();

    if (p == (LxProc *)0 || sig == 0u || sig >= (u32)LX_NSIG) {
        return (u64)0 - LX_EINVAL;
    }
    if (sig == (u32)LX_SIGKILL || sig == (u32)LX_SIGSTOP) {
        return (u64)0 - LX_EINVAL;
    }
    if (uold != 0ull) {
        if (lx_user_ok(uold, 32ull, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        *(u64 *)uold = p->sa[sig].handler;
        *(u64 *)(uold + 8ull) = p->sa[sig].flags;
        *(u64 *)(uold + 16ull) = p->sa[sig].restorer;
        *(u64 *)(uold + 24ull) = p->sa[sig].mask;
    }
    if (uact != 0ull) {
        if (lx_user_ok(uact, 32ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        p->sa[sig].handler = *(const u64 *)uact;
        p->sa[sig].flags = *(const u64 *)(uact + 8ull);
        p->sa[sig].restorer = *(const u64 *)(uact + 16ull);
        p->sa[sig].mask = *(const u64 *)(uact + 24ull);
    }
    return 0ull;
}

u64 lx_sig_procmask(u32 how, u64 uset, u64 uold)
{
    LxProc *p = lx_cur_proc();
    u64 set = 0ull;
    u64 hard = (1ull << ((u32)LX_SIGKILL - 1u)) |
               (1ull << ((u32)LX_SIGSTOP - 1u));

    if (p == (LxProc *)0) {
        return (u64)0 - LX_EINVAL;
    }
    if (uold != 0ull) {
        if (lx_user_ok(uold, 8ull, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        *(u64 *)uold = p->sig_mask;
    }
    if (uset != 0ull) {
        if (lx_user_ok(uset, 8ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        set = *(const u64 *)uset;
        if (how == (u32)LX_SIG_BLOCK) {
            p->sig_mask |= set;
        } else if (how == (u32)LX_SIG_UNBLOCK) {
            p->sig_mask &= ~set;
        } else if (how == (u32)LX_SIG_SETMASK) {
            p->sig_mask = set;
        } else {
            return (u64)0 - LX_EINVAL;
        }
        p->sig_mask &= ~hard;               /* KILL/STOP 不可屏蔽 */
    }
    return 0ull;
}

u64 lx_sig_altstack(u64 uss, u64 uoss)
{
    LxProc *p = lx_cur_proc();

    if (p == (LxProc *)0) {
        return (u64)0 - LX_EINVAL;
    }
    if (uoss != 0ull) {
        if (lx_user_ok(uoss, 24ull, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        *(u64 *)uoss = p->alt_sp;
        *(u32 *)(uoss + 8ull) = p->alt_flags;
        *(u64 *)(uoss + 16ull) = p->alt_size;
    }
    if (uss != 0ull) {
        if (lx_user_ok(uss, 24ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        p->alt_sp = *(const u64 *)uss;
        p->alt_flags = *(const u32 *)(uss + 8ull);
        p->alt_size = *(const u64 *)(uss + 16ull);
    }
    return 0ull;
}

u64 lx_sig_kill(i64 pid, u32 sig)
{
    LxProc *t;

    if (sig == 0u) {
        return 0ull;                        /* 0 号信号只探测存在性 */
    }
    if (sig >= (u32)LX_NSIG) {
        return (u64)0 - LX_EINVAL;
    }
    if (pid <= 0) {
        t = lx_cur_proc();
    } else {
        t = lx_proc_find(pid);
    }
    if (t == (LxProc *)0) {
        return (u64)0 - LX_ESRCH;
    }
    t->sig_pending |= 1ull << (sig - 1u);
    t->sig_code[sig] = (u32)LX_SI_USER;
    lx_proc_wake(t);                        /* 阻塞中的话叫醒它去处理 */
    return 0ull;
}

/* 默认动作：多数致命，SIGCHLD/SIGCONT/SIGWINCH/SIGURG 默认忽略。 */
static int default_(LxProc *p, u32 sig)
{
    (void)p;
    if (sig == (u32)LX_SIGCHLD || sig == (u32)LX_SIGCONT ||
        sig == (u32)LX_SIGWINCH || sig == (u32)LX_SIGURG) {
        return 1;
    }
    port_puts("\n[lx] 进程被信号终止 sig=");
    put_dec_((u64)sig);
    port_puts("\n");
    lx_proc_exit_sig(sig);                  /* 不返回 */
    return 1;
}

/* 在用户栈上压 rt_sigframe，并把 iret 帧改成"进 handler"。 */
static void build_(LxProc *p, SigView *v, u32 sig, u32 code)
{
    u64 sp = *v->rsp;
    u64 base = (sp - 128ull - LX_SF_SIZE) & ~15ull;   /* 留出 red zone */
    u64 uc = base + LX_SF_UC;
    u64 gi = uc + LX_UC_GREGS;
    u64 newmask;
    u64 n;

    zero_(base, LX_SF_SIZE);

    *(u64 *)(base + LX_SF_PRETCODE) = p->sa[sig].restorer;
    *(u64 *)(uc + 0ull) = 0ull;                       /* uc_flags */
    *(u64 *)(uc + 8ull) = 0ull;                       /* uc_link */
    *(u64 *)(uc + LX_UC_STACK + 0ull) = p->alt_sp;
    *(u32 *)(uc + LX_UC_STACK + 8ull) = p->alt_flags;
    *(u64 *)(uc + LX_UC_STACK + 16ull) = p->alt_size;

    /* gregs：R8 R9 R10 R11 | R12 R13 R14 R15 | RDI RSI RBP RBX | RDX RAX RCX */
    *(u64 *)(gi + 0ull) = v->r->r8;
    *(u64 *)(gi + 8ull) = v->r->r9;
    *(u64 *)(gi + 16ull) = v->r->r10;
    *(u64 *)(gi + 24ull) = v->r->r11;
    *(u64 *)(gi + 32ull) = v->r->r12;
    *(u64 *)(gi + 40ull) = v->r->r13;
    *(u64 *)(gi + 48ull) = v->r->r14;
    *(u64 *)(gi + 56ull) = v->r->r15;
    *(u64 *)(gi + 64ull) = v->r->rdi;
    *(u64 *)(gi + 72ull) = v->r->rsi;
    *(u64 *)(gi + 80ull) = v->r->rbp;
    *(u64 *)(gi + 88ull) = v->r->rbx;
    *(u64 *)(gi + 96ull) = v->r->rdx;
    *(u64 *)(gi + 104ull) = v->r->rax;
    *(u64 *)(gi + 112ull) = v->r->rcx;                /* RCX：syscall 已覆盖成返回地址 */
    *(u64 *)(gi + (u64)LX_GREGS_RSP * 8ull) = sp;
    *(u64 *)(gi + (u64)LX_GREGS_RIP * 8ull) = *v->rip;
    *(u64 *)(gi + (u64)LX_GREGS_EFL * 8ull) = *v->rfl;

    /* 新屏蔽字 = 旧 | sa_mask | 本信号（SA_NODEFER 时不加） */
    newmask = p->sig_mask | p->sa[sig].mask;
    if ((p->sa[sig].flags & LX_SA_NODEFER) == 0ull) {
        newmask |= 1ull << (sig - 1u);
    }
    *(u64 *)(uc + LX_UC_SIGMASK) = newmask;

    *(u32 *)(base + LX_SF_INFO) = (u32)sig;
    *(u32 *)(base + LX_SF_INFO + 4ull) = 0u;
    *(u32 *)(base + LX_SF_INFO + 8ull) = (u32)code;

    n = p->sa[sig].handler;
    p->sig_mask = newmask;
    v->r->rdi = (u64)sig;
    v->r->rsi = base + LX_SF_INFO;
    v->r->rdx = uc;
    v->r->rax = 0ull;
    *v->rip = n;
    *v->rsp = base;
}

/* 投递一个信号：force != 0 时只投它（异常路径用），否则投最低位的挂起信号。 */
static int deliver_(LxProc *p, SigView *v, u32 code, u32 force)
{
    u64 pend = p->sig_pending & ~p->sig_mask;
    u64 h;
    u32 sig;

    if (force != 0u) {
        if ((p->sig_pending & (1ull << (force - 1u))) == 0ull) {
            return 0;
        }
        sig = force;
    } else {
        if (pend == 0ull) {
            return 0;
        }
        sig = 1u;
        while (sig < (u32)LX_NSIG && (pend & (1ull << (sig - 1u))) == 0ull) {
            sig++;
        }
        if (sig >= (u32)LX_NSIG) {
            return 0;
        }
    }
    p->sig_pending &= ~(1ull << (sig - 1u));

    h = p->sa[sig].handler;
    if (h == LX_SIG_IGN) {
        return 1;                           /* 忽略掉了，算处理完 */
    }
    if (h == LX_SIG_DFL) {
        return default_(p, sig);
    }
    if (code == (u32)LX_SI_USER && p->sig_code[sig] != (u32)LX_SI_USER) {
        code = p->sig_code[sig];
    }
    build_(p, v, sig, code);
    return 1;
}

/* 从"调度恢复 / 系统调用返回"用：f 是 LxFrame（rcx=RIP, r11=RFLAGS）。 */
void lx_sig_deliver_frame(LxProc *p, LxFrame *f)
{
    SigView v;

    v.r = &f->r;
    v.rip = &f->r.rcx;
    v.rsp = &f->user_rsp;
    v.rfl = &f->r.r11;
    while (deliver_(p, &v, (u32)LX_SI_USER, 0u) != 0) {
        /* 一个信号处理完可能还有下一个；忽略类的直接循环掉 */
    }
}

/* 异常 → 信号。返回 1 表示已投递（调用方应该 iretq 回用户态），0 表示没得救。 */
int lx_sig_deliver_exc(LxProc *p, LxRegs *r, u64 cr2, u32 vec)
{
    SigView v;
    u32 sig;
    u32 code;

    (void)cr2;
    if (vec == 14u) {                       /* #PF */
        sig = (u32)LX_SIGSEGV;
        code = 1u;                          /* SEGV_MAPERR */
    } else if (vec == 13u) {                /* #GP */
        sig = (u32)LX_SIGSEGV;
        code = (u32)LX_SI_KERNEL;
    } else if (vec == 0u) {                 /* #DE */
        sig = (u32)LX_SIGFPE;
        code = 1u;
    } else if (vec == 6u) {                 /* #UD */
        sig = (u32)LX_SIGILL;
        code = 2u;
    } else if (vec == 17u) {                /* #AC */
        sig = (u32)LX_SIGBUS;
        code = 1u;
    } else {
        return 0;
    }
    p->sig_pending |= 1ull << (sig - 1u);
    p->sig_code[sig] = code;

    v.r = r;
    v.rip = (u64 *)r + 17u;                 /* 异常帧：vec, err, RIP, CS, RFLAGS, RSP, SS */
    v.rsp = (u64 *)r + 20u;
    v.rfl = (u64 *)r + 19u;
    return deliver_(p, &v, code, sig);
}

/* rt_sigreturn(15)：restorer 进来，从帧里恢复整份上下文。 */
u64 lx_sig_return(LxRegs *r)
{
    LxProc *p = lx_cur_proc();
    u64 base;
    u64 uc;
    u64 gi;

    if (p == (LxProc *)0) {
        return (u64)0 - LX_EINVAL;
    }
    /* restorer 里 RSP 指向 pretcode 之上那一格（ret 已弹掉 pretcode） */
    base = *(u64 *)((u8 *)r + LX_FRAME_USER_RSP) - 8ull;
    uc = base + LX_SF_UC;
    gi = uc + LX_UC_GREGS;

    r->r8 = *(const u64 *)(gi + 0ull);
    r->r9 = *(const u64 *)(gi + 8ull);
    r->r10 = *(const u64 *)(gi + 16ull);
    r->r12 = *(const u64 *)(gi + 32ull);
    r->r13 = *(const u64 *)(gi + 40ull);
    r->r14 = *(const u64 *)(gi + 48ull);
    r->r15 = *(const u64 *)(gi + 56ull);
    r->rdi = *(const u64 *)(gi + 64ull);
    r->rsi = *(const u64 *)(gi + 72ull);
    r->rbp = *(const u64 *)(gi + 80ull);
    r->rbx = *(const u64 *)(gi + 88ull);
    r->rdx = *(const u64 *)(gi + 96ull);
    r->rax = *(const u64 *)(gi + 104ull);
    r->rcx = *(const u64 *)(gi + (u64)LX_GREGS_RIP * 8ull);     /* 返回地址 */
    r->r11 = *(const u64 *)(gi + (u64)LX_GREGS_EFL * 8ull);     /* 返回 RFLAGS */
    *(u64 *)((u8 *)r + LX_FRAME_USER_RSP) =
        *(const u64 *)(gi + (u64)LX_GREGS_RSP * 8ull);
    p->sig_mask = *(const u64 *)(uc + LX_UC_SIGMASK);
    return r->rax;
}
