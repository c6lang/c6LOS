/* linux/proc.c —— L5 进程模型。
 *
 * 协作调度：ring3 里 IF=0，没有时钟中断，所以进程只在系统调用边界上切换。
 * fork/exec/wait/pipe 这条路径够用：父进程 wait4 或读空管道时主动让出，
 * 被唤醒后从让出点继续。抢占式调度要等 IRQ_USER，届时只换触发源。
 */

#include <c6os/linux_abi.h>
#include <c6os/linux_proc.h>
#include <c6os/port.h>

/* LX_ARG_MAX / LX_ARG_BYTES 见 linux_abi.h（elf64.c 压栈时也用同一份）。 */

/* 管道（M13-3c）：对象在 kernel heap，环形缓冲区按需从 4KB 长到 64KB。
 * Linux 的策略就是"写多少给多少、上限 pipe-max-size"，我们照这个做：
 * 一次 write 需要的空间比现有容量大就先把缓冲扩到够（封顶 64KB），
 * 所以要往空管道里写 64KB 只需一次调用，不需要用户侧循环。 */
#define LX_PIPE_MIN   4096ull
#define LX_PIPE_MAX   65536ull
#define LX_PIPE_MAX_CAP 4096u    /* 管道**对象**数的保护性上限（不是缓冲大小） */

typedef struct LxPipe {
    u32 readers;
    u32 writers;
    u64 head;
    u64 tail;
    u64 cap;
    u8 *buf;
} LxPipe;

/* 进程表（M13-3b）：槽位数组在内核堆里按需倍增，每个槽位有独立的 LxProc 与
 * 内核栈。槽位一旦建立就留下复用（和 Linux 的 task_struct/内核栈 slab 一样），
 * 所以内存上限由**峰值并发**决定，而不是由历史 fork 总数决定。
 *
 * 为什么 LxProc 与槽位分开分配：fork 里会持有父进程的 LxProc* 去调 slot_alloc_，
 * 如果 LxProc 内嵌在数组里，数组倍增会让那个指针失效。 */
typedef struct LxSlot {
    LxProc *p;            /* 0 = 未使用 */
    u8     *kstk;         /* 内核栈基址；栈顶 = kstk + LX_KSTACK_SIZE */
} LxSlot;

static LxSlot *g_proc;             /* 槽位数组：内核堆，倍增 */
static u32     g_proc_cap;
static LxPipe **g_pipe;            /* 管道槽位（指针表，内核堆，倍增） */
static u32       g_pipe_cap;
static int     g_cur = -1;

/* L8：抢占。定时器每 tick 进来一次，攒够时间片就让出 CPU。 */
#define LX_SLICE_TICKS 2u

static u32 g_irq_ticks;
static u32 g_preempt_switch;
static u32 g_slice;

static u64     g_pid_next = 1ull;
static int     g_resched;

/* L8 的定量证据（ticks/switch）。只在 pid 1 退出时打一次，子进程退出不刷屏。 */
static void pdec_(u32 v)
{
    char b[12];
    u32 k = 0u;

    if (v == 0u) {
        port_putc('0');
        return;
    }
    while (v > 0u && k < 11u) {
        b[k] = (char)('0' + (int)(v % 10u));
        v /= 10u;
        k++;
    }
    while (k > 0u) {
        k--;
        port_putc((int)b[k]);
    }
}

/* 汇编侧（port/x86_64/user_entry.S） */
extern void lx_ctx_switch(u64 *save_sp, u64 new_sp);
extern void x86_resume_user(void);
extern u64  x86_resume_frame;

/* 外部依赖 */
i64 lx_user_cstr(u64 upath, char *dst, u64 max);              /* linux/sys.c */
i64 lx_fs_lookup(const char *path);                           /* linux/fs.c */


static LxProc *cur_(void)
{
    if (g_cur < 0 || (u32)g_cur >= g_proc_cap) {
        return (LxProc *)0;
    }
    return g_proc[g_cur].p;
}

LxProc *lx_cur_proc(void)
{
    return cur_();
}

static void pipe_drop_(LxFile *fe);
static void pipe_ref_(const LxFile *fe);
static u64  n_(u64 v);             /* 负 errno（定义在本文件后部） */

static void mem_zero_(u8 *p, u64 n)
{
    u64 i;

    for (i = 0; i < n; i++) {
        p[i] = 0u;
    }
}

static void mem_copy_(u8 *d, const u8 *s, u64 n)
{
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/* ---- 进程表 -------------------------------------------------------------- */

/* 槽位定义见文件头：数组本身在 kernel heap，按需倍增。 */
static int slot_grow_(void)
{
    u32 ncap = (g_proc_cap == 0u) ? 8u : (g_proc_cap * 2u);
    LxSlot *ns;
    u32 i;

    if (g_proc_cap >= LX_PROC_MAX_CAP) {
        return -1;
    }
    if (ncap > LX_PROC_MAX_CAP) {
        ncap = LX_PROC_MAX_CAP;
    }
    ns = (LxSlot *)port_kcalloc((u64)ncap, (u64)sizeof(LxSlot));
    if (ns == (LxSlot *)0) {
        return -1;
    }
    for (i = 0u; i < g_proc_cap; i++) {
        ns[i] = g_proc[i];
    }
    if (g_proc != (LxSlot *)0) {
        port_kfree((void *)g_proc);
    }
    g_proc = ns;
    g_proc_cap = ncap;
    return 0;
}

/* 拿一个可用槽位：优先复用已建立但空出来的（保留它的 LxProc 与内核栈），
 * 没有再扩容。返回的 LxProc 是清零的、kstack_top 已填好的。 */
static int slot_alloc_(void)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *p = g_proc[i].p;

        if (p != (LxProc *)0 && p->state == LX_P_FREE) {
            mem_zero_((u8 *)p, (u64)sizeof(*p));
            p->kstack_top = (u64)g_proc[i].kstk + LX_KSTACK_SIZE;
            return (int)i;
        }
    }
    if (slot_grow_() != 0) {
        return -1;
    }
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *p;
        u8 *ks;

        if (g_proc[i].p != (LxProc *)0) {
            continue;
        }
        p = (LxProc *)port_kcalloc(1ull, (u64)sizeof(*p));
        ks = (u8 *)port_kmalloc(LX_KSTACK_SIZE);
        if (p == (LxProc *)0 || ks == (u8 *)0) {
            if (p != (LxProc *)0) {
                port_kfree((void *)p);
            }
            if (ks != (u8 *)0) {
                port_kfree((void *)ks);
            }
            return -1;
        }
        g_proc[i].p = p;
        g_proc[i].kstk = ks;
        p->kstack_top = (u64)ks + LX_KSTACK_SIZE;
        return (int)i;
    }
    return -1;
}

void lx_proc_attach(u64 as, u64 brk)
{
    LxProc *p;
    int s;

    if (g_cur < 0) {
        s = slot_alloc_();
        if (s < 0) {
            port_puts("[lx] 进程表没有内存\n");
            port_panic_halt();
        }
        g_cur = s;
        p = cur_();
        p->pid = (u32)g_pid_next;
        g_pid_next++;
        p->ppid = 0u;
        p->pgid = p->pid;                  /* 首个进程自成一个进程组和会话 */
        p->sid = p->pid;
        /* fd 表销毁时逐槽通知管道减 readers/writers：fdt 不认识管道，走钩子。 */
        lx_fdtab_set_free_hook(pipe_drop_);
    }
    p = cur_();
    if (p == (LxProc *)0) {
        port_puts("[lx] 进程表坏了\n");
        port_panic_halt();
    }
    if (p->fd == (LxFdTab *)0) {
        /* 单进程载荷可能在登记进程之前就 open 过（走 fs.c 的兜底表）：接管它，
         * 那些 fd 属于这个进程，不能丢。 */
        p->fd = lx_fs_get_fdtab();
        if (p->fd == (LxFdTab *)0) {
            p->fd = lx_fdtab_new();
        }
        if (p->fd == (LxFdTab *)0) {
            port_puts("[lx] 没有内存分配 fd 表\n");
            port_panic_halt();
        }
    }
    lx_fs_set_fdtab(p->fd);                /* fd 表从此跟着进程走 */
    lx_fs_set_cwd(p->cwd);
    p->state = LX_P_RUN;
    p->as = as;
    p->brk = brk;
    p->mmap_top = C6OS_LINUX_MMAP_BASE;
    p->fs_base = 0ull;                    /* 新镜像自己会 arch_prctl 设 TLS */
    lx_sig_reset(p);
    p->tgid = p->pid;
    p->started = 1u;
    port_user_set_kstack(p->kstack_top);
}

/* ---- L7：线程 -------------------------------------------------------------
 *
 * 线程共享地址空间 / brk / mmap_top / fd 表 / sa 表。fd 表现在已经是带引用计数
 * 的共享对象（M13-3a，clone 时 ref+1），不再需要抄数组；as/brk/mmap_top/sa 仍然
 * 在**切换点**与组长做双向同步：让出前写回组长，上 CPU 前从组长取新值。这样
 * fork/exec/wait/信号那些已经在跑的路径一行不用改，也不会出现"某个 syscall
 * 忘了刷"的漏网点。
 */
static void sync_to_leader_(LxProc *p)
{
    LxProc *lead;
    u64 k;

    if (p->tgid == p->pid || p->tgid == 0u || p->state == LX_P_FREE) {
        return;                            /* 自己就是组长 */
    }
    lead = lx_proc_find((i64)p->tgid);
    if (lead == (LxProc *)0 || lead == p || lead->state == LX_P_FREE) {
        return;
    }
    lead->as = p->as;
    lead->brk = p->brk;
    lead->mmap_top = p->mmap_top;
    for (k = 0ull; k < (u64)LX_NSIG; k++) {
        lead->sa[k] = p->sa[k];
    }
}

static void sync_from_leader_(LxProc *p)
{
    LxProc *lead;
    u64 k;

    if (p->tgid == p->pid || p->tgid == 0u) {
        return;
    }
    lead = lx_proc_find((i64)p->tgid);
    if (lead == (LxProc *)0 || lead == p || lead->state == LX_P_FREE) {
        return;
    }
    p->as = lead->as;
    p->brk = lead->brk;
    p->mmap_top = lead->mmap_top;
    for (k = 0ull; k < (u64)LX_NSIG; k++) {
        p->sa[k] = lead->sa[k];
    }
}

static int pick_next_(int from)
{
    int i;

    /* i 只能绕一圈：resched_ 在调用前已经把"当前"置成 READY，
     * 若最后一圈绕回 from 就会把当前槽位自己挑出来 -> switch_to_(自己)
     * 用陈旧 ksp 自切换、换坏栈。 */
    for (i = 1; i < (int)g_proc_cap; i++) {
        int k = ((from + i) % (int)g_proc_cap + (int)g_proc_cap) % (int)g_proc_cap;
        LxProc *q = g_proc[k].p;

        if (q != (LxProc *)0 && q->state == LX_P_READY) {
            return k;
        }
    }
    return -1;
}

/* 切到 next：换页表 / 内核栈 / fd 表 / brk 与 mmap 状态。
 * 回来时（本进程被再次调度）从 lx_ctx_switch 之后继续。 */
static void switch_to_(int next)
{
    LxProc *p = g_proc[next].p;
    LxProc *c = cur_();
    u64 prev_sp;

    if (c != (LxProc *)0) {
        if (c->irq_frame != 0u) {
            /* 被定时器切走的次数：这里数最准 —— 抢占后的 resched_ 要等这个
             * 进程再被调度回来才返回，那时候 g_cur 早就变回去了。 */
            g_preempt_switch++;
        }
        /* 让出前必须把**活**的 brk/mmap_top 存回自己的 PCB —— 否则切回来时
         * lx_set_as 那几行会用陈旧值把 g_brk/g_mmap_top 还原，后续 mmap 就会
         * 和已有映射重叠（pthread 的 8MB 栈一出现就必炸）。 */
        c->brk = lx_brk_get();
        c->mmap_top = lx_mmap_top_get();
        sync_to_leader_(c);                /* 再刷给组长（线程共享） */
    }
    g_cur = next;
    sync_from_leader_(p);                  /* 上 CPU 前从组长取最新的 */
    p->state = LX_P_RUN;
    lx_set_as(p->as);
    lx_brk_set(p->brk);
    lx_mmap_top_set(p->mmap_top);
    lx_fs_set_fdtab(p->fd);
    lx_fs_set_cwd(p->cwd);
    port_user_set_kstack(p->kstack_top);
    port_user_set_fsbase(p->fs_base);
    if (p->started == 0u) {
        lx_sig_deliver_frame(p, &p->f);     /* 还没跑过：p->f 的快照才是权威 */
    }
    /* 已经在用户态跑过的进程**不要**在这里投递信号：它多半是从
     * lx_proc_block() 被唤醒、正走在某个系统调用的返回路上，此时内核栈顶那份
     * 陷入帧的 rax 还是**进系统调用时**的旧值（read 的调用号 0）。在这里
     * build_() 会把旧 rax 存进 signal frame，rt_sigreturn 再 restore 回来 ——
     * 于是 read 明明写了 1 字节给用户，返回值却是 0（M14 case 11）。
     * 统一等 lx_syscall() 出口投递，那儿拿到的 rax 才是真正返回值。 */
    (void)p->irq_frame;
    x86_resume_frame = (u64)(&p->f);
    if (c == (LxProc *)0) {
        return;
    }
    lx_ctx_switch(&c->ksp, p->ksp);
}

void lx_proc_yield(void)
{
    g_resched = 1;
}

/* 还有别的进程可能改变等待条件吗？
 * 协作式调度里所有阻塞都是"别人来叫醒我"，如果没有任何别人在跑，
 * 叫醒永远不会发生 —— 这时候宁可返回失败也不能挂死。 */
static int others_live_(int self);
static void wake_all_(void);
static void wake_(u32 pid);
static void stop_self_(LxProc *c, u32 sig);
static void notify_parent_(LxProc *p, u32 pid, int cld);
static u32 sig_first_handled_(LxProc *q);

/* "没人可跑"时的现场抢救（M14-2）。
 *
 * 以前这里什么都不做：进程被标成 BLOCK 后 resched_ 找不到 READY，就直接切回来，
 * 于是队列里那个进程状态悬在 BLOCK、内核在 emit_next 里空转，整个系统看起来
 * 就是死机（B14 的一个具体形态）。三件事要做，顺序不能变：
 *
 *   1. 已停止的作业：SIGCONT 是**不阻塞**的作业控制信号，哪怕进程正停在
 *      SIGSTOP 上也必须能唤醒它。没有这步，shell 一停自己就再也起不来。
 *   2. 终端读的简化：POSIX 里后台进程读终端会收到 SIGTTIN 停下，那是给
 *      有作业控制的 shell 用的。我们没有终端驱动线程，照搬只会让 shell
 *      "读一下键盘就停住"。所以这里只停"作业组不是前台组"的读，且放行
 *      shell 自己所属的会话首进程 —— 也就是交互 shell 永远读得到。
 *   3. 死锁兜底：当前进程阻塞着、又没有别人能推进，就把它当"永远醒不来"，
 *      按 Linux 上同样局面会得到的结果让阻塞方返回（pipe_read_ 拿 0 = EOF，
 *      wait4 拿 ECHILD，poll 返回 0）。宁可给一个错误答案，也不能挂死。 */
static u32  g_fore;                  /* 前台进程组（0 = 没有作业控制） */
u32 lx_proc_fore(void) { return g_fore; }

/* 续**一个**进程（按 pid）。组广播是 lx_proc_send 的职责：它已经逐个成员
 * 调用 lx_sig_send_one 了，这里再按 pgid 匹配就会把"给单个进程发 SIGCONT"
 * 误变成"给它所在的整个组发"，而 `kill(pid, SIGCONT)` 的 pid 根本不是
 * pgid —— 结果谁也没被续上，父子一起挂死（M14 case 7）。 */
static void job_cont_(u32 pid)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q->state == LX_P_FREE || q->pid != pid) {
            continue;
        }
        q->cont_seen = 1ull;           /* 之后 waitpid(WCONTINUED) 看这个 */
        if (q->state == LX_P_STOP) {
            /* 停止态 → 从 lx_proc_block() 的让出点继续；不清 cont_seen，
             * 让父进程还有机会收到 WIFCONTINUED。 */
            q->stopsig = 0u;
            q->resumed = 1u;
            q->state = LX_P_READY;
        } else if (q->state == LX_P_BLOCK) {
            /* 被收着的阻塞调用的进程（比如正读终端）：续行信号对它是可中断
             * 的，Linux 上会以 EINTR 返回；这里先唤醒，调用点重新检查条件。 */
            q->stopsig = 0u;
            q->resumed = 1u;
            q->state = LX_P_READY;
        }
    }
    wake_all_();
}

static void stall_check_(LxProc *c)
{
    u32 i;
    int woke = 0;

    if (c == (LxProc *)0 || c->state != LX_P_BLOCK) {
        /* 已经有 READY 了（pick_next_ 只会在没有 READY 时返回 <0），
         * 或者当前进程压根没阻塞：都不该动别人的状态。 */
        return;
    }
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q->state != LX_P_STOP) {
            continue;
        }
        if ((q->sig_pending & (1ull << ((u32)LX_SIGCONT - 1u))) != 0ull) {
            q->sig_pending &= ~(1ull << ((u32)LX_SIGCONT - 1u));
            job_cont_(q->pid);
            woke = 1;
        }
    }
    if (woke != 0) {
        return;
    }
    /* 后台作业读终端：停它，让前台（或 shell）接着跑。
     * 刚被 SIGCONT 续过的那一次放过（Linux 的 SIGTTIN 判定也一样有例外，
     * 否则 `bg; fg` 之后第一次读就又停了）。 */
    if (c->wait_kind == LX_WAIT_TTY && c->pgid != g_fore && g_fore != 0u &&
        c->sid != c->tgid && c->resumed == 0u) {
        stop_self_(c, (u32)LX_SIGTTIN);
        return;
    }
    /* 最后一招：让阻塞在"永远等不到的唤醒"上的进程自己醒。 */
    if (!others_live_(g_cur)) {
        c->state = LX_P_READY;
    }
}

/* ---- M14-2：作业控制状态机 ------------------------------------------------
 *
 * 停止态的进程不进 pick_next_（LX_P_STOP 既不是 READY 也不是 RUN），所以
 * c->ksp 停在 lx_proc_block() 让出点的下一行，SIGCONT 只要把状态改回 READY
 * 就"从暂停处继续"，不需要额外的上下文魔法。
 *
 * SIGCHLD 的 si_code 精确到"怎么结束的"：shell 的 `WIFSTOPPED`/`WIFCONTINUED`
 * 判定依赖它，所以退出/停止/继续三条路径都要各自报码。 */
static void notify_parent_(LxProc *p, u32 pid, int cld)
{
    LxProc *par = lx_proc_find((i64)p->ppid);

    if (par == (LxProc *)0) {
        return;
    }
    par->sig_pending |= 1ull << ((u32)LX_SIGCHLD - 1u);
    par->sig_code[LX_SIGCHLD] = (u32)cld;
    wake_(par->pid);
    wake_(p->ppid);
}

static void stop_self_(LxProc *c, u32 sig)
{
    if (c == (LxProc *)0) {
        return;
    }
    c->stopsig = sig;
    c->state = LX_P_STOP;
    notify_parent_(c, c->pid, (int)LX_CLD_STOPPED);
}

static void stop_(u32 pid, u32 sig)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q->state == LX_P_FREE || q->state == LX_P_ZOMBIE ||
            q->pid != pid) {
            continue;
        }
        if (q->state == LX_P_STOP) {
            return;                    /* 已经停着：Linux 不重复上报 */
        }
        stop_self_(q, sig);
        return;
    }
}

void lx_proc_job_stop(u32 pid, u32 sig)
{
    stop_(pid, sig);
}

void lx_proc_job_cont(u32 pid)
{
    job_cont_(pid);
}

/* 信号广播（M14-2 的 kill(2) 语义）。进程表在 proc.c 里，sig.c 只负责
 * "往某个进程投递"，所以选择目标这一步留在这里。
 *
 *   pid > 0   → 那个进程所在的**整个线程组**（Linux 的 kill 定义）
 *   pid == 0  → 调用者所在进程组
 *   pid == -1 → 除自己以外所有进程
 *   pid < -1  → 进程组 = |pid| */
int lx_proc_send(i64 pid, u32 sig)
{
    LxProc *self = cur_();
    u32 i;
    int hit = 0;

    if (pid > 0) {
        LxProc *t = lx_proc_find(pid);
        u32 tgid;

        if (t == (LxProc *)0) {
            return -1;
        }
        tgid = t->tgid;
        for (i = 0u; i < g_proc_cap; i++) {
            LxProc *q = g_proc[i].p;

            if (q != (LxProc *)0 && q->state != LX_P_FREE && q->tgid == tgid) {
                lx_sig_send_one(q, sig, 0);
                hit = 1;
            }
        }
        return (hit != 0) ? 0 : -1;
    }
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q->state == LX_P_FREE) {
            continue;
        }
        if (pid == 0) {
            if (self == (LxProc *)0 || q->pgid != self->pgid) {
                continue;
            }
        } else if (pid == -1) {
            if (q == self) {
                continue;
            }
        } else if ((i64)q->pgid != -pid) {
            continue;
        }
        lx_sig_send_one(q, sig, 1);
        hit = 1;
    }
    return (hit != 0) ? 0 : -1;
}

/* ---- 前台进程组（SIGTTIN/SIGTTOU 的判据）-------------------------------- */
u64 lx_proc_tcsetpgrp(i64 fd, i64 pgid)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0 || fd < 0 || fd > 2) {
        return n_((u64)LX_ENOTTY);
    }
    if (pgid < 0) {
        return n_((u64)LX_EINVAL);
    }
    if (p->sid != p->tgid) {
        return n_((u64)LX_ENOTTY);     /* 只有会话首进程能改（Linux 同义） */
    }
    g_fore = (u32)pgid;
    return 0ull;
}

u64 lx_proc_tcgetpgrp(i64 fd)
{
    if (fd < 0 || fd > 2) {
        return n_((u64)LX_ENOTTY);
    }
    return (g_fore != 0u) ? (u64)g_fore : lx_proc_getpgrp();
}

/* alarm(2)：TSC 当作纳秒用（和 clock_gettime 的口径一致）。QEMU 里 TSC
 * 通常是 1GHz 量级，误差只影响"几点响"，不影响语义。 */
#define LX_ALARM_HZ  1000000000ull

u64 lx_proc_alarm(u64 secs)
{
    LxProc *p = cur_();
    u64 now;
    u64 old = 0ull;

    if (p == (LxProc *)0) {
        return 0ull;
    }
    now = port_cycles();
    if (p->alarm_deadline != 0ull && now < p->alarm_deadline) {
        old = (p->alarm_deadline - now + LX_ALARM_HZ - 1ull) / LX_ALARM_HZ;
    }
    p->alarm_deadline = (secs == 0ull) ? 0ull : now + secs * LX_ALARM_HZ;
    return old;
}

/* 到点投递 SIGALRM。两个调用点：
 *   - resched_()：进程每次 lx_proc_block() 都会走这里。光靠 PIT tick 不够 ——
 *     被阻塞的进程不占 CPU，而内核态关着中断，tick 根本进不来，闹钟会永远
 *     不响（在 pipe read 上等SIGALRM 就是这种局面）。
 *   - lx_sched_preempt()：进程在用户态跑、别的进程设了闹钟时要能及时响。 */
void lx_proc_alarm_check(void)
{
    u64 now = port_cycles();
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q->state == LX_P_FREE || q->alarm_deadline == 0ull) {
            continue;
        }
        if (now >= q->alarm_deadline) {
            q->alarm_deadline = 0ull;
            lx_sig_send_one(q, (u32)LX_SIGALRM, 0);
        }
    }
}

/* 让出 CPU 一次；没有别的可运行进程时立刻回来。 */
static void resched_(void)
{
    LxProc *c = cur_();
    int next;

    if (c == (LxProc *)0) {
        return;
    }
    lx_proc_alarm_check();
    if (c->state == LX_P_RUN) {
        c->state = LX_P_READY;
    }
    next = pick_next_(g_cur);
    if (next < 0) {
        /* 没人可跑：先把"死锁"变成"活锁"，再让当前进程体面地失败，
         * 而不是永远卡在 emit_next 里空转（见 stall_check_ 的说明）。 */
        stall_check_(c);
        if (c->state == LX_P_READY) {
            c->state = LX_P_RUN;
        }
        return;
    }
    switch_to_(next);
}

/* 系统调用末尾统一入口：有让出请求就切一次。 */
void lx_sched_tick(void)
{
    if (g_resched == 0) {
        return;
    }
    g_resched = 0;
    resched_();
}

/* L8：定时器 tick（只从 ring3 的 IRQ 路径进来）。到点就让出一次 CPU；
 * 被抢占的进程重新上 CPU 时从这里继续返回它的中断帧。 */
void lx_sched_preempt(void)
{
    LxProc *c = cur_();
    int before;

    lx_proc_alarm_check();
    if (c == (LxProc *)0 || c->state != LX_P_RUN) {
        return;
    }
    g_irq_ticks++;
    g_slice++;
    if (g_slice < LX_SLICE_TICKS) {
        return;
    }
    g_slice = 0u;
    c->irq_frame = 1u;
    resched_();
    c->irq_frame = 0u;
    (void)before;
}

void lx_proc_set_wait(u32 kind)
{
    LxProc *c = cur_();

    if (c != (LxProc *)0) {
        c->wait_kind = kind;
    }
}

void lx_proc_block(void)
{
    LxProc *c = cur_();

    if (c == (LxProc *)0) {
        return;
    }
    c->state = LX_P_BLOCK;
    c->intr = 0u;
    c->intr_sig = 0u;
    if (c->wait_kind == LX_WAIT_NONE) {
        c->wait_kind = LX_WAIT_PIPE;   /* 没声明的当"别人会来叫醒我" */
    }
    resched_();
    c->wait_kind = LX_WAIT_NONE;
    /* 被唤醒后从这里继续：调用点必须重新检查条件 */
}

/* 刚醒过来：是被信号打断的吗？（M14-3）。取一次就清掉，避免下次误报。 */
int lx_proc_intr(void)
{
    LxProc *c = cur_();
    int r;

    if (c == (LxProc *)0 || c->intr == 0u) {
        return 0;
    }
    r = 1;
    c->intr = 0u;                      /* intr_sig 留着给 lx_proc_restart() 查 */
    return r;
}

/* 打断它的那个信号是否带 SA_RESTART：带了就不该返回 EINTR，调用点继续等。 */
int lx_proc_restart(void)
{
    LxProc *c = cur_();

    if (c == (LxProc *)0 || c->intr_sig == 0u || c->intr_sig >= (u32)LX_NSIG) {
        return 0;
    }
    if ((c->sa[c->intr_sig].flags & LX_SA_RESTART) == 0ull) {
        return 0;
    }
    c->intr_sig = 0u;
    return 1;
}

/* 叫醒一个阻塞进程。M14-3：如果它就是有**装了 handler 的信号**待处理的那个，
 * 醒来后必须报 EINTR（除非 SA_RESTART），所以顺手把 intr/intr_sig 记上。
 * 这是唯一能区分"条件满足"和"被信号叫醒"的位置——Linux 也是这样。 */
static void wake_(u32 pid)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q->pid != pid || q->state != LX_P_BLOCK) {
            continue;
        }
        q->state = LX_P_READY;
        if (q->sig_pending != 0ull) {
            u32 s = sig_first_handled_(q);

            if (s != 0u) {
                q->intr = 1u;
                q->intr_sig = s;
            }
        }
    }
}

/* pending 里第一个"会真正执行用户代码"的信号（装了 handler 的）。
 * 默认动作/忽略的信号不算：它们不会打断系统调用。 */
static u32 sig_first_handled_(LxProc *q)
{
    u32 s;

    for (s = 1u; s < (u32)LX_NSIG; s++) {
        u64 h;

        if ((q->sig_pending & (1ull << (s - 1u))) == 0ull) {
            continue;
        }
        if ((q->sig_mask & (1ull << (s - 1u))) != 0ull) {
            continue;                    /* 被屏蔽：等 unblock 才投递，不打断 */
        }
        h = q->sa[s].handler;
        if (h != LX_SIG_DFL && h != LX_SIG_IGN) {
            return s;
        }
    }
    return 0u;
}

LxProc *lx_proc_find(i64 pid)
{
    u32 i;

    if (pid <= 0) {
        return (LxProc *)0;
    }
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q != (LxProc *)0 && q->state != LX_P_FREE && (i64)q->pid == pid) {
            return q;
        }
    }
    for (i = 0u; i < g_proc_cap; i++) {    /* kill(pid) 打的是整个线程组 */
        LxProc *q = g_proc[i].p;

        if (q != (LxProc *)0 && q->state != LX_P_FREE && (i64)q->tgid == pid) {
            return q;
        }
    }
    return (LxProc *)0;
}

void lx_proc_wake(LxProc *p)
{
    if (p->state == LX_P_BLOCK) {
        u32 s;

        p->state = LX_P_READY;
        /* M14-3：叫醒它的如果是一个装了 handler 的信号，阻塞系统调用必须
         * 返回 EINTR。以前只有 wake_()（管道/子进程那个）会记 intr，而
         * lx_sig_send_one 走的是这里 —— 于是信号叫醒的 read 醒来后看不到
         * intr，会继续回去阻塞，EINTR 永远不发生。 */
        s = sig_first_handled_(p);
        if (s != 0u) {
            p->intr = 1u;
            p->intr_sig = s;
        }
    }
}

/* ---- 进程组 / 会话（M14-1c，修 B1）---------------------------------------
 *
 * B1 的原文是"setpgid 和 chdir 共用一个 case，setpgid() 实际执行
 * chdir(pid 的指针)"。补 pgid/sid 两个字段之后，getpgrp/getpgid 也不再是
 * 硬编码的 1 —— 那是为了骗过 ash 的 job control 写的，一旦有人真的 setsid
 * 就会自相矛盾。 */

u64 lx_proc_getpgrp(void)
{
    LxProc *p = cur_();

    return (p != (LxProc *)0) ? (u64)p->pgid : 1ull;
}

u64 lx_proc_getpgid(i64 pid)
{
    LxProc *p;

    if (pid == 0) {
        p = cur_();
    } else {
        p = lx_proc_find(pid);
    }
    if (p == (LxProc *)0) {
        return n_((u64)LX_ESRCH);
    }
    return (u64)p->pgid;
}

u64 lx_proc_setpgid(i64 pid, i64 pgid)
{
    LxProc *self = cur_();
    LxProc *p;
    u32 want;

    if (self == (LxProc *)0) {
        return n_((u64)LX_ESRCH);
    }
    if (pid == 0) {
        p = self;
    } else {
        p = lx_proc_find(pid);
    }
    if (p == (LxProc *)0) {
        return n_((u64)LX_ESRCH);
    }
    /* pid 指向的必须是"进程"而不是随便一个线程：Linux 只在
     * pid == 该线程组的 tgid 时才认。 */
    if (p->tgid != p->pid) {
        return n_((u64)LX_ESRCH);
    }
    /* Linux 的真实约束只有三条（见 setpgid(2)）：
     *   ① 不能给**已经 exec 过的会话首进程**改组 —— 我们这里没有
     *      "是否 exec 过"这个状态，所以只能保留"会话首进程"这一半；
     *   ② 只能在调用者自己的会话里改组；
     *   ③ 目标组必须属于同一会话。
     * 以前把 ① 写成"sid == pid 一概 EPERM"，连 `setpgid(0,0)` 都被拒 ——
     * 而 0 号进程自成一个会话，于是**所有**进程的 setpgid(0,0) 全失败，
     * ash 只能退回无作业控制模式。现在按上面三条重写。 */
    if (pid != 0 && lx_proc_find(pid) == (LxProc *)0) {
        return n_((u64)LX_ESRCH);
    }
    if (p->sid != self->sid && p != self) {
        return n_((u64)LX_EPERM);          /* ② 跨会话 */
    }
    if (pgid < 0) {
        return n_((u64)LX_EINVAL);
    }
    if (pgid == 0) {
        /* 0 = 加入自己的组。已经是组首就是幂等的成功（Linux 允许）。 */
        want = p->pid;
    } else {
        want = (u32)pgid;
        if ((u64)want != (u64)p->pid) {
            u32 i;
            int found = 0;

            for (i = 0u; i < g_proc_cap; i++) {
                LxProc *q = g_proc[i].p;

                if (q != (LxProc *)0 && q->state != LX_P_FREE && q->pgid == want) {
                    if (q->sid != p->sid) {
                        return n_((u64)LX_EPERM);   /* ③ 目标组在别的会话 */
                    }
                    found = 1;
                    break;
                }
            }
            if (found == 0) {
                return n_((u64)LX_EPERM);       /* 目标组不存在 */
            }
        }
    }
    /* 组内所有线程一起动：pgid 是线程组的属性（proc.c 里每个 LxProc 都存，
     * clone 出来的兄弟线程跟着走，否则同一个进程里 getpgid 会不一致）。 */
    {
        u32 i;

        for (i = 0u; i < g_proc_cap; i++) {
            LxProc *q = g_proc[i].p;

            if (q != (LxProc *)0 && q->state != LX_P_FREE && q->tgid == p->tgid) {
                q->pgid = want;
            }
        }
    }
    return 0ull;
}

u64 lx_proc_setsid(void)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0) {
        return n_((u64)LX_ESRCH);
    }
    if (p->sid == p->pid) {
        return n_((u64)LX_EPERM);          /* 已经是会话首进程 */
    }
    p->sid = p->pid;
    p->pgid = p->pid;
    return (u64)p->pid;
}

/* 信号导致的终止：状态字是信号号（wait4 侧按 WIFSIGNALED 解释）。 */
void lx_proc_exit_sig(u32 sig)
{
    LxProc *c = cur_();

    if (c == (LxProc *)0) {
        port_panic_halt();
    }
    c->killed = 1u;
    lx_proc_exit((u64)sig);
}

/* 把同组其它线程直接释放（不动 self）。共享地址空间随之少一个引用。 */
static void reap_peers_(LxProc *self)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || q == self || q->state == LX_P_FREE ||
            q->tgid != self->tgid) {
            continue;
        }
        q->state = LX_P_FREE;
        q->pid = 0u;
        lx_fdtab_unref(q->fd);             /* 每个线程持一份 fd 表引用 */
        q->fd = (LxFdTab *)0;
        port_as_unref(q->as);
        q->as = 0ull;
    }
}

/* 非组长线程退出：只结束自己，清 clear_tid（pthread_join 等的就是它）并叫醒别人。 */
static void thread_exit_(LxProc *c)
{
    u64 ct = c->clear_tid;
    u32 i;
    int next;

    if (ct != 0ull && lx_user_ok(ct, 4ull, C6OS_PROT_W) != 0) {
        *(u32 *)ct = 0u;
    }
    c->clear_tid = 0ull;
    c->state = LX_P_FREE;
    c->pid = 0u;
    lx_fdtab_unref(c->fd);                 /* 只放自己那一份，线程组还有别的引用 */
    c->fd = (LxFdTab *)0;
    /* 组长还持有这个 AS 的引用，所以这里不会把页表拆掉 */
    port_as_unref(c->as);
    c->as = 0ull;
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q != (LxProc *)0 && q->state == LX_P_BLOCK) {
            q->state = LX_P_READY;
        }
    }
    next = pick_next_(g_cur);
    if (next < 0) {
        port_puts("\n[lx] 所有进程都已退出，内核停在这里\n");
        port_panic_halt();
    }
    switch_to_(next);
    port_panic_halt();
}

void lx_proc_exit(u64 code)
{
    LxProc *c = cur_();
    int next;

    if (c == (LxProc *)0) {
        port_puts("[lx] exit 但当前没有进程\n");
        port_panic_halt();
    }
    if (c->tgid != 0u && c->tgid != c->pid) {
        thread_exit_(c);                   /* 线程退出：整组还在 */
        return;                            /* 不返回 */
    }
    reap_peers_(c);                        /* 组长退出 = 整个线程组没了 */
    /* 关掉自己那份 fd 表（引用归零时才逐槽通知管道减 readers/writers）。
     * 线程组共享一份：组长退出时最后一个引用在自己手上，正好一起释放。 */
    lx_fdtab_unref(c->fd);
    c->fd = (LxFdTab *)0;
    c->code = (i64)code;
    c->state = LX_P_ZOMBIE;
    c->stopsig = 0u;                       /* 停止态退出：不再报 stop */
    c->wait_kind = LX_WAIT_NONE;
    /* 注意：AS 的最后一个引用留到父进程 wait4 收尸时才放（见 wait4）。
     * SIGCHLD 的 si_code 必须精确到"怎么结束的"：shell 靠 WIFSIGNALED 分支
     * 决定回显什么，笼统报 SI_USER 会让它把"被信号杀"当成正常退出。 */
    notify_parent_(c, c->pid,
                   (c->killed != 0u) ? (int)LX_CLD_KILLED : (int)LX_CLD_EXITED);
    wake_(c->ppid);
    if (c->pid == 1u) {
        port_puts("\n[preempt] ticks=");
        pdec_(g_irq_ticks);
        port_puts(" switch=");
        pdec_(g_preempt_switch);
        port_puts("\n");
    }
    next = pick_next_(g_cur);
    if (next < 0) {
        port_puts("\n[lx] 所有进程都已退出，内核停在这里\n");
        port_panic_halt();
    }
    switch_to_(next);
    /* 僵尸不该被调度回来 */
    port_puts("[lx] 僵尸进程被重新调度：进程表坏了\n");
    port_panic_halt();
}

/* ---- fork ---------------------------------------------------------------- */

/* 首次运行的进程靠一张"蹦床"栈进入：lx_ctx_switch 的 ret 直接落到
 * x86_resume_user，由它从 LxFrame 恢复寄存器并 iretq。 */
static void setup_trampoline_(LxProc *p)
{
    u64 top = p->kstack_top;
    u64 *s = (u64 *)(top - 56ull);

    s[0] = 0ull;                          /* r15 */
    s[1] = 0ull;                          /* r14 */
    s[2] = 0ull;                          /* r13 */
    s[3] = 0ull;                          /* r12 */
    s[4] = 0ull;                          /* rbx */
    s[5] = 0ull;                          /* rbp */
    s[6] = (u64)&x86_resume_user;         /* ret 落点 */
    p->ksp = (u64)s;
}

u64 lx_proc_fork(LxRegs *r)
{
    LxProc *par = cur_();
    LxFrame *pf = (LxFrame *)r;
    LxProc *ch;
    u64 as;
    u64 i;
    int slot;

    if (par == (LxProc *)0) {
        return (u64)0 - LX_ECHILD;
    }
    slot = slot_alloc_();
    if (slot < 0) {
        return (u64)0 - LX_EAGAIN;
    }
    as = port_as_create();
    if (as == 0ull) {
        return (u64)0 - LX_ENOMEM;
    }
    if (port_as_copy(as, par->as) != 0) {
        port_as_destroy(as);
        return (u64)0 - LX_ENOMEM;
    }
    ch = g_proc[slot].p;                   /* slot_alloc_ 已清零并填好 kstack_top */
    ch->pid = (u32)g_pid_next;
    g_pid_next++;
    ch->ppid = par->pid;
    ch->tgid = ch->pid;                   /* fork 出来的是新线程组 */
    ch->pgid = par->pgid;                 /* fork 继承进程组与会话（Linux 语义） */
    ch->sid = par->sid;
    ch->as = as;
    ch->brk = par->brk;
    ch->mmap_top = par->mmap_top;
    ch->f.r = *r;                         /* 继承父进程的用户寄存器 */
    ch->f.r.rax = 0ull;                   /* fork 在子进程里返回 0 */
    ch->f.user_rsp = pf->user_rsp;
    ch->fs_base = par->fs_base;
    ch->started = 0u;
    lx_sig_fork(ch, par);
    ch->state = LX_P_READY;
    {
        u64 k_;

        for (k_ = 0ull; k_ < (u64)sizeof(ch->cwd); k_++) {
            ch->cwd[k_] = par->cwd[k_];
            if (ch->cwd[k_] == '\0') {
                break;
            }
        }
    }
    {
        /* fd 表：fork 是**复制**（父子各自的 open/close 互不影响）。管道端点
         * 的引用计数在 copy 成功后逐槽补上。 */
        ch->fd = lx_fdtab_copy(par->fd);
        if (ch->fd == (LxFdTab *)0) {
            ch->state = LX_P_FREE;
            port_as_destroy(as);
            return (u64)0 - LX_ENOMEM;
        }
        for (i = 0ull; i < (u64)ch->fd->cap; i++) {
            if (ch->fd->ent[i].node == LX_FD_NONE) {
                continue;
            }
            pipe_ref_(&ch->fd->ent[i]);        /* 复制出的端点各持一份引用 */
        }
    }
    setup_trampoline_(ch);
    lx_proc_yield();                      /* 给子进程一次机会 */
    return (u64)ch->pid;
}

/* ---- wait4 --------------------------------------------------------------- */

/* 子进程是否落在 wait4 的 pid 选择范围内（Linux 的四条语义，一条都不能省）：
 *   pid > 0   → 那一个
 *   pid == 0  → 和调用者同一个进程组
 *   pid == -1 → 任意
 *   pid < -1  → 进程组 = |pid|  */
static int waited_(const LxProc *par, const LxProc *c, i64 pid)
{
    if (c->ppid != par->pid) {
        return 0;
    }
    if (pid > 0) {
        return (i64)c->pid == pid;
    }
    if (pid == 0) {
        return c->pgid == par->pgid;
    }
    if (pid == -1) {
        return 1;
    }
    return (i64)c->pgid == -pid;
}

/* 把一条子进程状态翻译成 wait4 的 status 字（Linux x86-64 布局）。 */
static u64 wstatus_(const LxProc *c)
{
    if (c->state == LX_P_ZOMBIE) {
        return (c->killed != 0u) ? ((u64)c->code & 0x7full)
                                 : (((u64)c->code & 0xffull) << 8);
    }
    if (c->state == LX_P_STOP) {
        return ((u64)c->stopsig & 0xffull) << 8 | 0x7full;
    }
    return 0xffffull;                      /* WIFCONTINUED：低字节 0xffff */
}

/* rusage 只要"有且合理"：/usr/bin/time 之外没人细看，但字段不能是垃圾，
 * 否则 busybox 的 time 会打印出天文数字。32 字节 = 4 个微秒级 u64，
 * 正好是 glibc `struct rusage` 前 4 项（utime/stime/cutime/cstime）对齐的部分。 */
static void fill_rusage_(u64 urusage)
{
    if (urusage == 0ull) {
        return;
    }
    if (port_as_user_range_ok(cur_()->as, urusage, 32ull, C6OS_PROT_W) == 0) {
        return;
    }
    for (u64 k = 0ull; k < 4ull; k++) {
        *(u64 *)(urusage + k * 8ull) = 0ull;
    }
}

/* wait4(2)（M14-2 重写）。
 *
 * 以前这里只认 pid > 0、"有子进程就一直等到僵尸"，`options`/`urusage` 直接
 * 丢掉。对 `sh -c 'cmd &'`、job control、以及 shell 的 `wait` 内建都不够用：
 *   - WNOHANG 没有可报告的返回 0（不是 ECHILD，也不是阻塞）；
 *   - WUNTRACED 让"停下来的子进程"也算可报告事件；
 *   - WCONTINUED 让"被 SIGCONT 续过的子进程"算可报告事件；
 *   - pid < -1 按进程组选，pid == 0 按调用者所在的组选。
 * 非法 option 位必须 EINVAL —— busybox 的 wait 会靠这个判断能不能用 waitid 简化。 */
u64 lx_proc_wait4(i64 pid, u64 ustatus, u64 options, u64 urusage)
{
    LxProc *par = cur_();
    int wflags = (int)(options & 0xffffffffull);

    if (par == (LxProc *)0) {
        return n_((u64)LX_ECHILD);
    }
    if ((options & ~((u64)LX_WNOHANG | (u64)LX_WUNTRACED | (u64)LX_WCONTINUED)) != 0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (ustatus != 0ull &&
        port_as_user_range_ok(par->as, ustatus, 4ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    for (;;) {
        u32 i;
        int has_child = 0;

        for (i = 0u; i < g_proc_cap; i++) {
            LxProc *c = g_proc[i].p;
            int report = 0;
            u32 cpid;

            if (c == (LxProc *)0 || c->state == LX_P_FREE || !waited_(par, c, pid)) {
                continue;
            }
            has_child = 1;
            if (c->state == LX_P_ZOMBIE) {
                report = 1;
            } else if (c->state == LX_P_STOP && (wflags & (int)LX_WUNTRACED) != 0) {
                report = 1;
            } else if (c->state != LX_P_STOP && c->cont_seen != 0ull &&
                       (wflags & (int)LX_WCONTINUED) != 0) {
                report = 1;
            }
            if (report == 0) {
                c->cont_seen = 0ull;       /* 没要 WCONTINUED：续行事件就地丢弃 */
                continue;
            }
            cpid = c->pid;
            if (ustatus != 0ull) {
                u64 st = wstatus_(c);

                *(u32 *)ustatus = (u32)st;
                fill_rusage_(urusage);
                /* 不管正常退出还是被信号杀，僵尸都要在这里真正收掉。以前用
                 * tag==1 表示"被杀"且只有那一支才设 FREE —— 正常退出的子进程
                 * 永远是 ZOMBIE，sh 的 waitpid(-1) 会一遍遍看到同一个死孩子，
                 * 后面命令的子进程永远收不到（alpine M12 卡在 uname 之后）。
                 * killed 只影响 status 字，不影响是否收尸。 */
                if (c->state == LX_P_ZOMBIE) {
                    c->state = LX_P_FREE;
                    port_as_unref(c->as);  /* 最后一个线程的 AS：到这里才真正拆 */
                    c->as = 0ull;
                } else if (c->state == LX_P_STOP) {
                    c->resumed = 0u;       /* 停止事件已交付，停止状态继续保留 */
                } else {
                    c->cont_seen = 0ull;   /* 续行事件已交付 */
                }
                return (u64)cpid;
            }
            /* ustatus == 0：调用者不关心状态字，但事件照收（Linux 同样语义）。 */
            fill_rusage_(urusage);
            if (c->state == LX_P_ZOMBIE) {
                c->state = LX_P_FREE;
                port_as_unref(c->as);
                c->as = 0ull;
            } else if (c->state != LX_P_STOP) {
                c->cont_seen = 0ull;
            }
            return (u64)cpid;
        }
        if (has_child == 0) {
            return n_((u64)LX_ECHILD);
        }
        if ((wflags & (int)LX_WNOHANG) != 0) {
            return 0ull;
        }
        /* 阻塞前必须声明原因：万一下一次调度发现"没人能叫醒我"，
         * stall_check_ 会用 LX_WAIT_CHILD 走"父进程等不到子进程"的兜底。 */
        par->wait_kind = LX_WAIT_CHILD;
        lx_proc_block();
        /* M14-3：被信号打断的 wait 返回 EINTR（wait 不认 SA_RESTART 之外
         * 的例外——Linux 上确实是"有 handler 就打断，SA_RESTART 就重来"）。 */
        if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
            return n_((u64)LX_EINTR);
        }
    }
}

/* ---- execve -------------------------------------------------------------- */

/* 从用户态读一个字符串向量（argv 或 envp）：指针数组 + 字符串一起搬进内核缓冲。
 * `*off` 是缓冲里已用字节数，argv 和 envp 共用一个缓冲时接着往后放。 */
static u32 read_vec_(u64 uvec, char **vec, char *buf, u64 bufbytes, u64 *off)
{
    u64 i;

    if (uvec == 0ull) {
        return 0u;
    }
    for (i = 0; i < (u64)LX_ARG_MAX; i++) {
        u64 up;
        i64 n;

        if (port_as_user_range_ok(cur_()->as, uvec + i * 8ull, 8ull,
                                  C6OS_PROT_R) == 0) {
            break;
        }
        up = *(u64 *)(uvec + i * 8ull);
        if (up == 0ull) {
            break;
        }
        if (*off >= bufbytes) {
            break;
        }
        n = lx_user_cstr(up, buf + *off, bufbytes - *off);
        if ((i64)n < 0) {
            break;
        }
        vec[i] = buf + *off;
        *off += (u64)n + 1ull;
    }
    return (u32)i;
}

/* execve 的 fd 语义：只关 FD_CLOEXEC 的那些，其余（含管道、重定向）跨 exec 保留。
 * 内核堆里的管道要连带减引用计数，所以这个扫描必须在 proc.c 里做——
 * fdt 不认识管道，fs.c 也不该认识。 */
static void cloexec_close_(LxProc *p)
{
    u32 i;

    if (p == (LxProc *)0 || p->fd == (LxFdTab *)0) {
        return;
    }
    for (i = 0u; i < p->fd->cap; i++) {
        LxFile *e = &p->fd->ent[i];

        if (e->node == LX_FD_NONE || (e->flags & LX_FD_CLOEXEC) == 0u) {
            continue;
        }
        if (e->kind != LX_FD_FILE) {
            pipe_drop_(e);
        }
        lx_fdtab_release(e);
    }
}

/* shebang（`#!/bin/sh`）：Alpine 里大量脚本靠它，缺了等于所有 shell 脚本
 * 都跑不起来。返回 0 = 是脚本且已把解释器填进 interp（可带一个可选参数），
 * -1 = 不是脚本。只认第一行的 `#!`，和 Linux 的 prep_binprm 一致。 */
static int shebang_(u64 img_pa, u64 img_size, char *interp, u64 icap,
                    char *opt, u64 ocap)
{
    const u8 *img = (const u8 *)port_phys_map(img_pa);
    u64 i = 2ull;
    u64 n = 0ull;
    u64 o = 0ull;

    if (img_size < 4ull || img[0] != '#' || img[1] != '!') {
        return -1;
    }
    while (i < img_size && (img[i] == ' ' || img[i] == '\t')) {
        i++;
    }
    while (i < img_size && img[i] != ' ' && img[i] != '\t' && img[i] != '\n') {
        if (n + 1ull >= icap) {
            return -1;
        }
        interp[n] = (char)img[i];
        n++;
        i++;
    }
    if (n == 0ull) {
        return -1;
    }
    interp[n] = '\0';
    while (i < img_size && (img[i] == ' ' || img[i] == '\t')) {
        i++;
    }
    while (i < img_size && img[i] != '\n') {
        if (o + 1ull >= ocap) {
            return -1;
        }
        opt[o] = (char)img[i];
        o++;
        i++;
    }
    while (o > 0ull && (opt[o - 1ull] == ' ' || opt[o - 1ull] == '\r')) {
        o--;
    }
    opt[o] = '\0';
    return 0;
}

u64 lx_proc_execve(u64 upath, u64 uargv, u64 uenvp, LxRegs *r)
{
    char path[128];
    char interp[128];
    char opt[128];
    char *buf = (char *)0;
    char **argv = (char **)0;
    char **envp = (char **)0;
    u64 as = 0ull;
    u64 entry = 0ull;
    u64 rsp = 0ull;
    u64 brk = 0ull;
    u64 off = 0ull;
    u64 ipa = 0ull;
    u64 isz = 0ull;
    i64 n;
    i64 idx;
    u32 argc;
    u32 envc;
    u32 base_argc;
    u32 i2;
    u64 rc;

    n = lx_user_cstr(upath, path, (u64)sizeof(path));
    if (n <= 0) {
        return (u64)0 - LX_EFAULT;
    }
    idx = lx_fs_lookup(path);
    if (idx < 0) {
        return (u64)0 - LX_ENOENT;
    }
    /* argv/envp 都走内核堆：Linux 允许 128KB 的参数区，放内核栈上会直接压穿。 */
    buf = (char *)port_kmalloc(LX_ARG_BYTES);
    argv = (char **)port_kcalloc((u64)LX_ARG_MAX + 1ull, 8ull);
    envp = (char **)port_kcalloc((u64)LX_ARG_MAX + 1ull, 8ull);
    if (buf == (char *)0 || argv == (char **)0 || envp == (char **)0) {
        rc = (u64)0 - LX_ENOMEM;
        goto fail;
    }
    argc = read_vec_(uargv, argv, buf, LX_ARG_BYTES, &off);
    envc = read_vec_(uenvp, envp, buf, LX_ARG_BYTES, &off);
    if (argc == 0u) {
        argv[0] = path;
        argc = 1u;
    }
    argv[argc] = (char *)0;
    envp[envc] = (char *)0;
    if (uenvp == 0ull) {
        envp[0] = (char *)0;
        envc = 0u;
    }

    ipa = lx_fs_node_data_pa((u32)idx);
    isz = lx_fs_node_size((u32)idx);
    /* #! 脚本：argv 变成 [interp, opt, path, argv[1..]]（Linux 的写法），
     * 映像换成解释器自己的。 */
    if (shebang_(ipa, isz, interp, (u64)sizeof(interp),
                 opt, (u64)sizeof(opt)) == 0) {
        i64 iidx = lx_fs_lookup(interp);

        if (iidx < 0) {
            rc = (u64)0 - LX_ENOENT;
            goto fail;
        }
        if ((u64)argc + 3ull > (u64)LX_ARG_MAX) {
            rc = (u64)0 - LX_E2BIG;
            goto fail;
        }
        base_argc = argc;
        for (i2 = base_argc; i2 > 1u; i2--) {   /* 原 argv[1..] 往后挪 */
            argv[i2 + 2u] = argv[i2 - 1u];
        }
        argv[0] = interp;
        argv[1] = (opt[0] != '\0') ? opt : path;
        argv[2] = path;
        argc = base_argc + ((opt[0] != '\0') ? 2u : 1u);
        argv[argc] = (char *)0;
        ipa = lx_fs_node_data_pa((u32)iidx);
        isz = lx_fs_node_size((u32)iidx);
    }

    as = port_as_create();
    if (as == 0ull) {
        rc = (u64)0 - LX_ENOMEM;
        goto fail;
    }
    if (lx_exec_build(as, ipa, isz, path,
                      (const char *const *)argv, argc,
                      (const char *const *)envp, envc,
                      &entry, &rsp, &brk) != 0) {
        port_as_destroy(as);
        as = 0ull;
        rc = (u64)0 - LX_ENOEXEC;
        goto fail;
    }
    port_kfree((void *)buf);              /* 新栈上已经拷好了副本 */
    port_kfree((void *)argv);
    port_kfree((void *)envp);
    buf = (char *)0;
    argv = (char **)0;
    envp = (char **)0;
    cloexec_close_(cur_());               /* Linux：跨 exec 只保留非 CLOEXEC 的 fd */
    {
        u64 old_as = cur_()->as;

        cur_()->as = as;
        cur_()->brk = brk;                /* 新映像的堆起点，不能留旧程序的 */
        cur_()->mmap_top = C6OS_LINUX_MMAP_BASE;
        cur_()->fs_base = 0ull;
        lx_sig_exec(cur_());
        lx_set_as(as);
        lx_brk_set(brk);
        lx_mmap_top_set(cur_()->mmap_top);
        port_as_unref(old_as);            /* 旧映像的页框在这里回收 */
    }
    /* 直接从陷入帧返回新程序入口，不走原来的调用栈 */
    r->r15 = 0ull;
    r->r14 = 0ull;
    r->r13 = 0ull;
    r->r12 = 0ull;
    r->r11 = 2ull;                        /* RFLAGS：IF=0 */
    r->r10 = 0ull;
    r->r9 = 0ull;
    r->r8 = 0ull;
    r->rbp = 0ull;
    r->rdi = 0ull;
    r->rsi = 0ull;
    r->rdx = 0ull;
    r->rcx = entry;                       /* 返回 RIP */
    r->rbx = 0ull;
    r->rax = 0ull;
    ((LxFrame *)r)->user_rsp = rsp;
    return 0ull;

fail:
    if (as != 0ull) {
        port_as_destroy(as);
    }
    if (buf != (char *)0) {
        port_kfree((void *)buf);
    }
    if (argv != (char **)0) {
        port_kfree((void *)argv);
    }
    if (envp != (char **)0) {
        port_kfree((void *)envp);
    }
    return rc;
}

/* ---- fd 与管道 ----------------------------------------------------------- */

static u64 n_(u64 v)
{
    return (u64)0 - v;
}

/* 把所有权在等待的进程都叫醒（协作式调度下没有等待队列，统一全表扫）。
 * 管道可读写、原子操作完成、线程退出都用它。 */
static void wake_all_(void)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q != (LxProc *)0 && q->state == LX_P_BLOCK) {
            q->state = LX_P_READY;
        }
    }
}

/* 管道号就是指针表里的槽位号（fd 的 node 字段）。槽位空 = 该槽尚未分配。 */
static LxPipe *pipe_at_(u64 node)
{
    if (node >= (u64)g_pipe_cap) {
        return (LxPipe *)0;
    }
    return g_pipe[node];
}

static int pipe_tab_grow_(void)
{
    u32 ncap = (g_pipe_cap == 0u) ? 8u : (g_pipe_cap * 2u);
    LxPipe **ns;
    u32 i;

    if (g_pipe_cap >= LX_PIPE_MAX_CAP) {
        return -1;
    }
    if (ncap > LX_PIPE_MAX_CAP) {
        ncap = LX_PIPE_MAX_CAP;
    }
    ns = (LxPipe **)port_kcalloc((u64)ncap, (u64)sizeof(LxPipe *));
    if (ns == (LxPipe **)0) {
        return -1;
    }
    for (i = 0u; i < g_pipe_cap; i++) {
        ns[i] = g_pipe[i];
    }
    if (g_pipe != (LxPipe **)0) {
        port_kfree((void *)g_pipe);
    }
    g_pipe = ns;
    g_pipe_cap = ncap;
    return 0;
}

/* 建一个管道：对象 + 最小容量环缓冲都从内核堆来。 */
static LxPipe *pipe_new_(u64 *o_idx)
{
    u32 i;

    for (i = 0u; i < g_pipe_cap; i++) {
        if (g_pipe[i] == (LxPipe *)0) {
            break;
        }
    }
    if (i >= g_pipe_cap) {
        if (pipe_tab_grow_() != 0) {
            return (LxPipe *)0;
        }
        for (i = 0u; i < g_pipe_cap; i++) {
            if (g_pipe[i] == (LxPipe *)0) {
                break;
            }
        }
        if (i >= g_pipe_cap) {
            return (LxPipe *)0;
        }
    }
    {
        LxPipe *pp = (LxPipe *)port_kcalloc(1ull, (u64)sizeof(LxPipe));
        u8 *buf = (u8 *)port_kmalloc(LX_PIPE_MIN);

        if (pp == (LxPipe *)0 || buf == (u8 *)0) {
            if (pp != (LxPipe *)0) {
                port_kfree((void *)pp);
            }
            if (buf != (u8 *)0) {
                port_kfree((void *)buf);
            }
            return (LxPipe *)0;
        }
        pp->cap = LX_PIPE_MIN;
        pp->buf = buf;
        g_pipe[i] = pp;
        *o_idx = (u64)i;
        return pp;
    }
}

static void pipe_free_(LxPipe *pp)
{
    port_kfree((void *)pp->buf);
    port_kfree((void *)pp);
}

static void pipe_drop_(LxFile *fe)
{
    LxPipe *pp = pipe_at_(fe->node);

    if (pp == (LxPipe *)0) {
        return;
    }
    if (fe->kind == LX_FD_PIPE_R && pp->readers > 0u) {
        pp->readers--;
    } else if (fe->kind == LX_FD_PIPE_W && pp->writers > 0u) {
        pp->writers--;
    }
    /* 端点计数是"谁还能读写这个管道"的**唯一**依据：readers 归零要立刻让
     * 阻塞中的写端看到 EPIPE/SIGPIPE，writers 归零要立刻让读端拿到 EOF。
     * 这里以前只等两个计数都归零才唤醒，导致 close(读端) 之后写端还在傻等。 */
    wake_all_();
    if (pp->readers == 0u && pp->writers == 0u) {
        g_pipe[fe->node] = (LxPipe *)0;
        pipe_free_(pp);
    }
}

/* 增加一个端点引用（dup2 / fork 复制 fd 表时用）。 */
static void pipe_ref_(const LxFile *fe)
{
    LxPipe *pp = pipe_at_(fe->node);

    if (pp == (LxPipe *)0) {
        return;
    }
    if (fe->kind == LX_FD_PIPE_R) {
        pp->readers++;
    } else if (fe->kind == LX_FD_PIPE_W) {
        pp->writers++;
    }
}

u64 lx_proc_pipe2(u64 ufds, u64 flags)
{
    LxProc *p = cur_();
    LxPipe *pp;
    LxFile *rf;
    LxFile *wf;
    u64 pi = 0ull;
    u64 rfd;
    u64 wfd;
    u32 pair[2];

    if (p == (LxProc *)0) {
        return n_((u64)LX_EINVAL);
    }
    if ((flags & ~(u64)(LX_O_CLOEXEC | LX_O_NONBLOCK)) != 0ull) {
        return n_((u64)LX_EINVAL);          /* pipe2 只接受这两位 */
    }
    if (port_as_user_range_ok(p->as, ufds, 8ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    rfd = lx_fdtab_alloc(p->fd);
    if (rfd == LX_FD_NONE) {
        return n_((u64)LX_EMFILE);
    }
    /* 先占位再要第二个：alloc 靠"node != LX_FD_NONE"判断空闲，不占位的话
     * 第二次扫描会从 3 号重新开始，把同一个槽发两遍（管道两端变成同一个 fd）。 */
    lx_fdtab_release(&p->fd->ent[rfd]);
    p->fd->ent[rfd].node = LX_FD_ROOT;
    wfd = lx_fdtab_alloc(p->fd);
    if (wfd == LX_FD_NONE) {                   /* 装不下第二个：把第一个还回去 */
        p->fd->ent[rfd].node = LX_FD_NONE;
        return n_((u64)LX_EMFILE);
    }
    pp = pipe_new_(&pi);
    if (pp == (LxPipe *)0) {
        p->fd->ent[rfd].node = LX_FD_NONE;
        p->fd->ent[wfd].node = LX_FD_NONE;
        return n_((u64)LX_EMFILE);
    }
    pp->readers = 1u;
    pp->writers = 1u;
    rf = &p->fd->ent[rfd];
    wf = &p->fd->ent[wfd];
    rf->node = pi;
    rf->pos = 0ull;
    rf->is_dir = 0u;
    rf->kind = LX_FD_PIPE_R;
    wf->node = pi;
    wf->pos = 0ull;
    wf->is_dir = 0u;
    wf->kind = LX_FD_PIPE_W;
    if ((flags & (u64)LX_O_CLOEXEC) != 0ull) {
        rf->flags = (u32)LX_FD_CLOEXEC;
        wf->flags = (u32)LX_FD_CLOEXEC;
    }
    if ((flags & (u64)LX_O_NONBLOCK) != 0ull) {
        rf->oflags = (u32)LX_O_NONBLOCK;
        wf->oflags = (u32)LX_O_NONBLOCK;
    }
    pair[0] = (u32)rfd;
    pair[1] = (u32)wfd;
    *(u32 *)ufds = pair[0];
    *(u32 *)(ufds + 4ull) = pair[1];
    return 0ull;
}

u64 lx_proc_close(u64 fd)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);

    if (fe == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (fe->kind != LX_FD_FILE) {
        pipe_drop_(fe);
        fe->node = LX_FD_NONE;
        wake_all_();
        return 0ull;
    }
    fe->node = LX_FD_NONE;                 /* 只读 initramfs：文件没有可回收的状态 */
    return 0ull;
}

u64 lx_proc_dup(u64 oldfd)
{
    LxProc *p = cur_();
    u64 newfd;

    if (p == (LxProc *)0 || lx_fdtab_get(p->fd, oldfd) == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    newfd = lx_fdtab_alloc(p->fd);
    if (newfd == LX_FD_NONE) {
        return n_((u64)LX_EMFILE);
    }
    return lx_proc_dup2(oldfd, newfd);
}

u64 lx_proc_dup2(u64 oldfd, u64 newfd)
{
    LxProc *p = cur_();
    LxFile *of;
    LxFile *nf;

    if (p == (LxProc *)0) {
        return n_((u64)LX_EBADF);
    }
    of = lx_fdtab_get(p->fd, oldfd);
    if (of == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (oldfd == newfd) {
        return newfd;
    }
    /* newfd 可以超过当前容量：dup2 到高位 fd 是合法用法 */
    if (lx_fdtab_grow(p->fd, newfd + 1ull) != 0) {
        return n_((u64)LX_EBADF);
    }
    nf = &p->fd->ent[newfd];
    if (nf->node != LX_FD_NONE && nf->kind != LX_FD_FILE) {
        pipe_drop_(nf);
    }
    *nf = *of;
    pipe_ref_(nf);
    return newfd;
}

/* dup3：dup2 + 只接受 O_CLOEXEC；oldfd == newfd 要报 EINVAL（dup2 则直接成功）。 */
u64 lx_proc_dup3(u64 oldfd, u64 newfd, u64 flags)
{
    LxProc *p = cur_();
    u64 r;

    if ((flags & ~(u64)LX_O_CLOEXEC) != 0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (oldfd == newfd) {
        return n_((u64)LX_EINVAL);
    }
    r = lx_proc_dup2(oldfd, newfd);
    if ((i64)r < 0) {
        return r;
    }
    if ((flags & (u64)LX_O_CLOEXEC) != 0ull) {
        p = cur_();
        if (p != (LxProc *)0 && lx_fdtab_get(p->fd, newfd) != (LxFile *)0) {
            p->fd->ent[newfd].flags |= (u32)LX_FD_CLOEXEC;
        }
    }
    return r;
}

/* fcntl：以前一律返回 0（假成功），后果是 O_NONBLOCK / FD_CLOEXEC 全部被丢掉，
 * socketpair 与管道上的非阻塞语义静默失效。这里按 Linux 的 5 条常用命令做真语义。 */
u64 lx_proc_fcntl(u64 fd, u64 cmd, u64 arg)
{
    LxProc *p = cur_();
    LxFile *fe;

    if (p == (LxProc *)0) {
        return n_((u64)LX_EBADF);
    }
    switch (cmd) {
    case 0ull:                                  /* F_DUPFD：最小的 >= arg 的空槽 */
    {
        u64 cand;

        if (lx_fdtab_grow(p->fd, arg + 1ull) != 0) {
            return n_((u64)LX_EMFILE);
        }
        for (cand = arg; cand < (u64)p->fd->cap; cand++) {
            if (p->fd->ent[cand].node == LX_FD_NONE) {
                return lx_proc_dup2(fd, cand);
            }
        }
        return n_((u64)LX_EMFILE);
    }
    case 1ull:                                  /* F_GETFD */
        fe = lx_fdtab_get(p->fd, fd);
        if (fe == (LxFile *)0) {
            return n_((u64)LX_EBADF);
        }
        return (u64)(u32)fe->flags;
    case 2ull:                                  /* F_SETFD */
        fe = lx_fdtab_get(p->fd, fd);
        if (fe == (LxFile *)0) {
            return n_((u64)LX_EBADF);
        }
        fe->flags = (u32)(arg & (u64)LX_FD_CLOEXEC);
        return 0ull;
    case 3ull:                                  /* F_GETFL */
        fe = lx_fdtab_get(p->fd, fd);
        if (fe == (LxFile *)0) {
            return n_((u64)LX_EBADF);
        }
        return (u64)(u32)fe->oflags;
    case 4ull:                                  /* F_SETFL：只有这三位可改 */
        fe = lx_fdtab_get(p->fd, fd);
        if (fe == (LxFile *)0) {
            return n_((u64)LX_EBADF);
        }
        fe->oflags = (u32)((fe->oflags & (u32)LX_O_ACCMODE) |
                          (u32)(arg & (u64)(LX_O_APPEND | LX_O_NONBLOCK)));
        return 0ull;
    case 1030ull:                               /* F_DUPFD_CLOEXEC */
    {
        u64 r = lx_proc_fcntl(fd, 0ull, arg);

        if ((i64)r >= 0 && lx_fdtab_get(p->fd, r) != (LxFile *)0) {
            p->fd->ent[r].flags |= (u32)LX_FD_CLOEXEC;
        }
        return r;
    }
    default:
        return n_((u64)LX_EINVAL);
    }
}

/* readv / writev：iov 是 {base,len} 的数组。逐项走同一套分发，所以管道、
 * 控制台、VFS 文件的行为与 read/write 完全一致。 */
u64 lx_proc_readv(u64 fd, u64 uiov, u64 cnt)
{
    LxProc *p = cur_();
    u64 i;
    u64 total = 0ull;

    if (p == (LxProc *)0) {
        return n_((u64)LX_EBADF);
    }
    if (cnt > 1024ull) {
        return n_((u64)LX_EINVAL);
    }
    if (cnt != 0ull && port_as_user_range_ok(p->as, uiov, cnt * 16ull,
                                             C6OS_PROT_R) == 0) {
        return n_((u64)LX_EFAULT);
    }
    for (i = 0ull; i < cnt; i++) {
        u64 base = *(const u64 *)(uiov + i * 16ull);
        u64 len = *(const u64 *)(uiov + i * 16ull + 8ull);
        u64 r = lx_proc_read(fd, base, len);

        if ((i64)r < 0) {
            return (total != 0ull) ? total : r;
        }
        total += r;
        if (r < len) {
            break;                  /* 短读：够一次返回了 */
        }
    }
    return total;
}

u64 lx_proc_writev(u64 fd, u64 uiov, u64 cnt)
{
    LxProc *p = cur_();
    u64 i;
    u64 total = 0ull;

    if (p == (LxProc *)0) {
        return n_((u64)LX_EBADF);
    }
    if (cnt > 1024ull) {
        return n_((u64)LX_EINVAL);
    }
    if (cnt != 0ull && port_as_user_range_ok(p->as, uiov, cnt * 16ull,
                                             C6OS_PROT_R) == 0) {
        return n_((u64)LX_EFAULT);
    }
    for (i = 0ull; i < cnt; i++) {
        u64 base = *(const u64 *)(uiov + i * 16ull);
        u64 len = *(const u64 *)(uiov + i * 16ull + 8ull);
        u64 r = lx_proc_write(fd, base, len);

        if ((i64)r < 0) {
            return (total != 0ull) ? total : r;
        }
        total += r;
        if (r < len) {
            break;
        }
    }
    return total;
}

/* ---- 读写：控制台 / 文件 / 管道 ------------------------------------------ */

/* 环缓冲扩容：数据按逻辑顺序搬到新缓冲，head 归零。
 * 这不是"提前分配"，而是 Linux 语义的一次写尽量一次完成（封顶 LX_PIPE_MAX）。 */
static int pipe_grow_(LxPipe *pp, u64 need)
{
    u64 ncap = pp->cap;
    u64 data = pp->tail - pp->head;
    u8 *nb;
    u64 i;

    if (need <= pp->cap) {
        return 0;
    }
    while (ncap < need) {
        if (ncap >= LX_PIPE_MAX) {
            return -1;
        }
        ncap *= 2ull;
        if (ncap > LX_PIPE_MAX) {
            ncap = LX_PIPE_MAX;
        }
    }
    nb = (u8 *)port_kmalloc(ncap);
    if (nb == (u8 *)0) {
        return -1;
    }
    for (i = 0ull; i < data; i++) {
        nb[i] = pp->buf[(pp->head + i) % pp->cap];
    }
    port_kfree((void *)pp->buf);
    pp->buf = nb;
    pp->cap = ncap;
    pp->head = 0ull;
    pp->tail = data;
    return 0;
}

static u64 pipe_read_(LxProc *p, LxFile *fe, u64 ubuf, u64 len)
{
    LxPipe *pp = pipe_at_(fe->node);

    (void)p;
    if (pp == (LxPipe *)0) {
        return n_((u64)LX_EBADF);
    }
    for (;;) {
        u64 done = 0ull;

        while (pp->head != pp->tail && done < len) {
            *(u8 *)(ubuf + done) = pp->buf[pp->head % pp->cap];
            pp->head++;
            done++;
        }
        if (done > 0ull) {
            wake_all_();                   /* 腾出空间，写端可能正等着 */
            return done;
        }
        if (pp->writers == 0u) {
            return 0ull;                   /* 写端全关：EOF */
        }
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
        /* M14-3：被装了 handler 的信号打断 → EINTR。带 SA_RESTART 的继续等。 */
        if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
            return n_((u64)LX_EINTR);
        }
    }
}

static u64 pipe_write_(LxProc *p, LxFile *fe, u64 ubuf, u64 len)
{
    LxPipe *pp = pipe_at_(fe->node);
    u64 done = 0ull;

    if (pp == (LxPipe *)0) {
        return n_((u64)LX_EBADF);
    }
    /* B13/M14-3：Linux 在写**之前**就检查读端 —— 管道没有读者时，哪怕缓冲
     * 还有空间也必须立刻 SIGPIPE + EPIPE（`yes | head` 靠这个让 yes 死掉）。
     * 以前只在"缓冲写满、准备阻塞"时才查，于是 close(读端) 之后一次小写入
     * 照样成功，EPIPE/SIGPIPE 永远不出现。 */
    if (pp->readers == 0u) {
        if (p != (LxProc *)0) {
            lx_sig_send_one(p, (u32)LX_SIGPIPE, 0);
        }
        return n_((u64)LX_EPIPE);
    }
    /* 阻塞写要"全写完才返回"（Linux）：先把缓冲扩到够用（封顶 64KB），
     * 剩下的部分靠边写边等读者腾地方。 */
    (void)pipe_grow_(pp, (pp->tail - pp->head) + len);
    for (;;) {
        while ((pp->tail - pp->head) < pp->cap && done < len) {
            pp->buf[pp->tail % pp->cap] = *(const u8 *)(ubuf + done);
            pp->tail++;
            done++;
        }
        if (done == len) {
            wake_all_();                   /* 有数据了，读者可能正等着 */
            return done;
        }
        if (pp->readers == 0u) {
            /* B13/M14-3：读者全跑光了 —— Linux 先给写端补一个 SIGPIPE
             * （默认动作是终止，所以 `yes | head` 才会死），再报 EPIPE。
             * 装了 handler 或设了 SIG_IGN 的进程要活下来拿到 EPIPE。
             * SIGPIPE 不打断已经写出去的那部分：先把 done 返回掉。 */
            if (p != (LxProc *)0) {
                lx_sig_send_one(p, (u32)LX_SIGPIPE, 0);
            }
            return (done > 0ull) ? done : n_((u64)LX_EPIPE);
        }
        wake_all_();
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
        if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
            /* 已经写出去的那部分是"部分写"，Linux 允许——先把它交回去，
             * 一个字节都没写成才报 EINTR。 */
            return (done > 0ull) ? done : n_((u64)LX_EINTR);
        }
    }
}

u64 lx_proc_read(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe;

    fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    if (fe == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (len == 0ull) {
        return 0ull;
    }
    if (port_as_user_range_ok(p->as, ubuf, len, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    if (fe->kind == LX_FD_PIPE_R) {
        return pipe_read_(p, fe, ubuf, len);
    }
    if (fd <= 2ull) {
        /* M12：init 显式开了行规程之后，fd 0 才是真终端（阻塞读一行）；
         * 没开就还是老行为——永远 EOF，L2..L6 的验收依赖这个。 */
        if (fd == 0ull && lx_tty_enabled() != 0u) {
            return lx_tty_read(ubuf, len, lx_proc_yield);
        }
        return 0ull;                       /* stdin 还没接键盘：EOF */
    }
    return lx_fs_read(fd, ubuf, len);
}

/* 还有别的进程可能改变管道状态吗？没有就别死等（协作式调度里等不到人）。 */
static int others_live_(int self)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q == (LxProc *)0 || (int)i == self) {
            continue;
        }
        if (q->state == LX_P_READY || q->state == LX_P_RUN) {
            return 1;
        }
    }
    return 0;
}

static u16 poll_events_(LxProc *p, i32 fd)
{
    const LxFile *f;

    if (fd < 0) {
        return (u16)LX_POLLNVAL;
    }
    f = lx_fdtab_get(p->fd, (u64)fd);
    if (f == (const LxFile *)0) {
        return (u16)LX_POLLNVAL;
    }
    if (f->kind == LX_FD_PIPE_R) {
        const LxPipe *pp = pipe_at_(f->node);
        u16 ev = 0u;

        if (pp == (const LxPipe *)0) {
            return (u16)LX_POLLNVAL;
        }
        if (pp->tail != pp->head) {
            ev = (u16)(ev | (u16)LX_POLLIN);
        }
        if (pp->writers == 0u) {
            ev = (u16)(ev | (u16)LX_POLLHUP);
        }
        return ev;
    }
    if (f->kind == LX_FD_PIPE_W) {
        const LxPipe *pp = pipe_at_(f->node);
        u16 ev = 0u;

        if (pp == (const LxPipe *)0) {
            return (u16)LX_POLLNVAL;
        }
        if ((pp->tail - pp->head) < pp->cap) {
            ev = (u16)(ev | (u16)LX_POLLOUT);
        }
        if (pp->readers == 0u) {
            ev = (u16)(ev | (u16)LX_POLLERR);
        }
        return ev;
    }
    if (f->node == LX_FD_CONSOLE && lx_tty_enabled() != 0u) {
        /* M12：真终端 —— 只有真有整行可读时才算"可读"，
         * 否则 select/poll 会立刻返回然后 read 又阻塞住。
         * 按 **node** 而不是 fd 号判断：ash 会把 stdin dup 成别的 fd 再用，
         * 那些 fd 也是终端（以前只看 fd == 0，dup 出来的会被当成"永久就绪"）。 */
        return (lx_tty_ready() != 0u) ? (u16)LX_POLLIN : (u16)0;
    }
    return (u16)LX_POLLIN;        /* 普通文件 / console：不阻塞 */
}

/* poll(2)：struct pollfd { i32 fd; i16 events; i16 revents; }，8 字节一项。
 * 只关心"管道能不能读/写"；普通 fd 一律报就绪，避免 shell 卡在等 console 上。 */
i64 lx_proc_poll(u64 ufds, u64 nfds, i64 timeout)
{
    LxProc *p = cur_();
    u64 deadline = 0ull;           /* port_cycles 口径；0=无限等 */
    int have_deadline = 0;

    if (p == (LxProc *)0) {
        return 0;
    }
    if (timeout > 0) {
        deadline = port_cycles() + (u64)timeout * 1000000ull;
        have_deadline = 1;
    }
    for (;;) {
        u64 i;
        i64 cnt = 0;
        u32 tty = 0u;                  /* 这轮轮询里有没有终端 fd */

        for (i = 0ull; i < nfds; i++) {
            u8 *e = (u8 *)(ufds + i * 8ull);
            i32 fd = *(i32 *)e;
            u16 rev;

            if (fd >= 0) {
                const LxFile *qe = lx_fdtab_get(p->fd, (u64)fd);

                if (qe != (const LxFile *)0 && qe->node == LX_FD_CONSOLE &&
                    lx_tty_enabled() != 0u) {
                    tty = 1u;
                }
            }
            rev = poll_events_(p, fd);
            *(u16 *)(e + 6ull) = rev;
            if (rev != 0u) {
                cnt++;
            }
        }
        if (cnt > 0 || timeout == 0) {
            return cnt;
        }
        if (have_deadline && port_cycles() >= deadline) {
            return cnt;
        }
        (void)lx_tty_pump();           /* 轮询式串口：每一轮都是一次机会 */
        /* 终端：输入来自串口，没有哪个进程会来唤醒我们，就地继续泵。
         * 这里**不能**用 lx_proc_block()：它把进程标成 BLOCK 再 resched，
         * 没人可切时状态就悬在 BLOCK 上，而且再也等不到唤醒。
         * 纯管道才值得真阻塞——但也得先确认还有别人能往管道里写。 */
        if (tty == 0u) {
            if (!others_live_(g_cur)) {
                return cnt;
            }
            lx_proc_set_wait(LX_WAIT_PIPE);
            lx_proc_block();
            if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
                return (cnt > 0ull) ? cnt : n_((u64)LX_EINTR);
            }
        }
    }
}

u64 lx_proc_write(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe;

    fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    if (fe == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (fe->kind == LX_FD_PIPE_W) {
        if (port_as_user_range_ok(p->as, ubuf, len, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        return pipe_write_(p, fe, ubuf, len);
    }
    /* 按 node 判目标，而不是 fd 号：dup 出来的高位 fd 也可能是终端或 VFS 文件。 */
    if (fe->node == LX_FD_CONSOLE) {
        return lx_console_put(ubuf, len);
    }
    return lx_console_write(fd, ubuf, len);     /* 其余（/tmp 等）走 VFS */
}

/* ---- L7：线程创建 / 退出 / futex ----------------------------------------- */

/* clone(CLONE_VM)：新线程共享页表，只在 clone 给的栈上跑。 */
u64 lx_proc_clone_thread(LxRegs *r, u64 flags, u64 child_stack, u64 ptid,
                         u64 ctid, u64 tls)
{
    LxProc *par = cur_();
    LxProc *ch;
    u32 tid;
    int slot;

    if (par == (LxProc *)0 || child_stack == 0ull) {
        return (u64)0 - LX_EINVAL;
    }
    slot = slot_alloc_();
    if (slot < 0) {
        return (u64)0 - LX_EAGAIN;
    }
    ch = g_proc[slot].p;                   /* slot_alloc_ 已清零并填好 kstack_top */
    ch->pid = (u32)g_pid_next;
    g_pid_next++;
    tid = ch->pid;
    ch->tgid = par->tgid;
    ch->ppid = par->ppid;
    ch->as = par->as;                      /* 共享页表：不复制 */
    port_as_ref(ch->as);                   /* 线程组每个成员持一个引用 */
    ch->brk = par->brk;
    ch->mmap_top = par->mmap_top;
    ch->f.r = *r;                          /* 继承父线程的陷入帧 */
    ch->f.r.rax = 0ull;                    /* 线程里 clone 返回 0 */
    ch->f.user_rsp = child_stack;          /* 跑在 clone 给的栈上 */
    ch->state = LX_P_READY;
    ch->started = 0u;
    ch->clear_tid = ((flags & LX_CLONE_CHILD_CLEARTID) != 0ull) ? ctid : 0ull;
    ch->fs_base = ((flags & LX_CLONE_SETTLS) != 0ull) ? tls : par->fs_base;
    lx_sig_fork(ch, par);                  /* 继承 handler/屏蔽，清挂起 */
    /* 线程共享**同一个** fd 表对象（ref+1），不复制数组：同一线程组里一个线程
     * open 出来的 fd，另一个线程立刻能看到；关闭也是整组生效。 */
    ch->fd = par->fd;
    lx_fdtab_ref(ch->fd);
    setup_trampoline_(ch);
    if (ptid != 0ull && lx_user_ok(ptid, 4ull, C6OS_PROT_W) != 0) {
        *(u32 *)ptid = tid;
    }
    if (((flags & LX_CLONE_CHILD_SETTID) != 0ull) && ctid != 0ull &&
        lx_user_ok(ctid, 4ull, C6OS_PROT_W) != 0) {
        *(u32 *)ctid = tid;
    }
    lx_proc_yield();                       /* 先让新线程跑一段 */
    return (u64)tid;
}

/* exit_group：整组一起死。非组长调用时让组长替全组"死"给父进程看。 */
void lx_proc_exit_group(u64 code)
{
    LxProc *c = cur_();
    LxProc *lead;
    int next;

    if (c == (LxProc *)0) {
        port_panic_halt();
    }
    if (c->tgid != 0u && c->tgid != c->pid) {
        /* 先把自己从线程组摘掉：reap_peers_ 只处理别人，AS 引用也只放一次。 */
        c->clear_tid = 0ull;
        c->state = LX_P_FREE;
        c->pid = 0u;
        port_as_unref(c->as);
        c->as = 0ull;
        lead = lx_proc_find((i64)c->tgid);
        if (lead != (LxProc *)0 && lead != c) {
            lead->code = (i64)code;
            lead->killed = c->killed;
            lead->state = LX_P_ZOMBIE;
            reap_peers_(lead);
            {
                LxProc *par = lx_proc_find((i64)lead->ppid);

                if (par != (LxProc *)0) {
                    par->sig_pending |= 1ull << ((u32)LX_SIGCHLD - 1u);
                    par->sig_code[LX_SIGCHLD] = (u32)LX_SI_USER;
                }
            }
            wake_(lead->ppid);
        }
        wake_all_();
        next = pick_next_(g_cur);
        if (next < 0) {
            port_puts("\n[lx] 所有进程都已退出，内核停在这里\n");
            port_panic_halt();
        }
        switch_to_(next);
        port_panic_halt();
    }
    reap_peers_(c);
    lx_proc_exit(code);                    /* 组长：走正常的 ZOMBIE 流程 */
}

u64 lx_proc_set_tid_address(u64 ptr)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0) {
        return 1ull;
    }
    p->set_tid = ptr;
    p->clear_tid = ptr;                    /* Linux：同时设 clear_child_tid */
    return (u64)p->pid;
}

/* futex：只用得到 WAIT/WAKE（pthread 的 join/mutex 全靠它）。
 * 协作式调度下"唤醒"一律走全表扫；WAIT 时如果已经没有别的可运行任务就直接
 * 返回（否则会死等一个永远不会来的唤醒，等于把自己挂死）。 */
u64 lx_futex(u64 uaddr, u32 op, u32 val, u64 utime)
{
    u32 cmd = op & 0x7fu;
    LxProc *p = cur_();

    (void)utime;
    if (p == (LxProc *)0) {
        return 0ull;
    }
    if (cmd == 0u) {                       /* FUTEX_WAIT */
        if (lx_user_ok(uaddr, 4ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        if (*(const u32 *)uaddr != val) {
            return (u64)0 - LX_EAGAIN;
        }
        if (!others_live_(g_cur)) {
            return 0ull;
        }
        lx_proc_block();
        return 0ull;
    }
    if (cmd == 1u) {                       /* FUTEX_WAKE */
        u64 n = 0ull;
        u32 i;

        for (i = 0u; i < g_proc_cap && n < (u64)val; i++) {
            LxProc *q = g_proc[i].p;

            if (q != (LxProc *)0 && q->state == LX_P_BLOCK) {
                q->state = LX_P_READY;
                n++;
            }
        }
        return n;
    }
    return 0ull;
}

u32 lx_proc_tgid(void)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0) {
        return 0u;
    }
    return (p->tgid != 0u) ? p->tgid : p->pid;
}

u32 lx_proc_tid(void)
{
    LxProc *p = cur_();

    return (p == (LxProc *)0) ? 0u : p->pid;
}
