/* linux/proc.c —— L5 进程模型。
 *
 * 协作调度：ring3 里 IF=0，没有时钟中断，所以进程只在系统调用边界上切换。
 * fork/exec/wait/pipe 这条路径够用：父进程 wait4 或读空管道时主动让出，
 * 被唤醒后从让出点继续。抢占式调度要等 IRQ_USER，届时只换触发源。
 */

#include <c6os/linux_abi.h>
#include <c6os/linux_proc.h>
#include <c6os/linux_dev.h>
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
    u64 gen;                           /* 单调递增：每次写 +1，供 epoll ET 判边沿 */
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

/* M58：/proc 目录列举活进程用。只收 RUN/READY/STOP，排除 FREE 与 ZOMBIE
 * （ZOMBIE 在 Linux 里仍可见，但它们的 stat 由 wait 前的残留信息给出，
 * 这里统一不列举，避免 top/ps 看到读不出内容的幽灵项）。 */
u32 lx_proc_list(u32 *out, u32 cap)
{
    u32 n = 0u;
    u32 i;

    if (g_proc == (LxSlot *)0) {
        return 0u;
    }
    for (i = 0u; i < g_proc_cap && n < cap; i++) {
        LxProc *p = g_proc[i].p;
        u32 tgid;
        u32 j;
        int dup = 0;

        if (p == (LxProc *)0 || p->state == LX_P_FREE || p->state == LX_P_ZOMBIE) {
            continue;
        }
        tgid = (p->tgid != 0u) ? p->tgid : p->pid;
        if (tgid == 0u) {
            continue;
        }
        /* 线程组只列组长一次（Linux /proc 顶层只放 tgid） */
        for (j = 0u; j < n; j++) {
            if (out[j] == tgid) { dup = 1; break; }
        }
        if (dup == 0) {
            out[n++] = tgid;
        }
    }
    return n;
}

static void pipe_drop_(LxFile *fe);
static void fd_free_hook_(LxFile *fe);
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

static void proc_slots_prealloc_(void)
{
    u32 i;

    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *p;
        u8 *ks;

        if (g_proc[i].p != (LxProc *)0) {
            continue;
        }
        p = (LxProc *)port_kcalloc(1ull, (u64)sizeof(*p));
        ks = (u8 *)port_kmalloc(LX_KSTACK_SIZE);
        if (p == (LxProc *)0 || ks == (u8 *)0) {
            if (p != (LxProc *)0) port_kfree((void *)p);
            if (ks != (u8 *)0) port_kfree((void *)ks);
            break;
        }
        g_proc[i].p = p;
        g_proc[i].kstk = ks;
        p->kstack_top = (u64)ks + LX_KSTACK_SIZE;
    }
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
        /* Chrome 启动会瞬时创建几十个线程/进程；进程表在启动早期一次性扩到
         * 512 槽，避免运行中 heap 紧张时 slot_grow_ 失败导致 pthread_create
         * 返回 EAGAIN（BrowserThread:IO 起不来）。 */
        {
            u32 gi;
            for (gi = 0u; gi < 6u; gi++) {
                if (slot_grow_() != 0) break;
            }
            proc_slots_prealloc_();
        }
        p = cur_();
        p->pid = (u32)g_pid_next;
        g_pid_next++;
        p->ppid = 0u;
        p->pgid = p->pid;                  /* 首个进程自成一个进程组和会话 */
        p->sid = p->pid;
        /* fd 表销毁时逐槽通知管道减 readers/writers：fdt 不认识管道，走钩子。
         * VFS 文件（/data FAT）也要在这里还内核句柄，否则子进程一退出就漏一个
         * FAT_MAX_FILES 槽，几十次 fork 之后 open 就报 ENOMEM。 */
        lx_fdtab_set_free_hook(fd_free_hook_);
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
    if (p->exe[0] == '\0') {
        const char *d = "/hello";
        u32 di;
        for (di = 0u; di < 6u; di++) { p->exe[di] = d[di]; }
        p->exe[6] = '\0';
    }
    lx_fs_set_fdtab(p->fd);                /* fd 表从此跟着进程走 */
    lx_fs_set_cwd(p->cwd);
    {
        /* M51：资源上限默认无限（资源不受限策略）；nice 默认 0。 */
        u32 ri;
        for (ri = 0u; ri < LX_NLIMITS; ri++) {
            p->rlim[ri] = LX_RLIM_INFINITY;
            p->rlim_max[ri] = LX_RLIM_INFINITY;
        }
        p->nice = 0;
    }
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

u64 lx_proc_exe(char *out, u64 cap)
{
    LxProc *p = lx_cur_proc();
    u64 n = 0ull;

    if (p == (LxProc *)0 || out == (char *)0 || cap == 0ull) {
        return 0ull;
    }
    while (n + 1ull < cap && p->exe[n] != '\0') {
        out[n] = p->exe[n];
        n++;
    }
    out[n] = '\0';
    return n;
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

/* 当前进程最早的那个闹钟/定时器还有多少 cycles 到期？
 * return 0 = 没有设任何定时器（调用方按自己的预算等）。 */
u64 lx_proc_alarm_remain(void)
{
    LxProc *c = cur_();
    u64 now;
    u64 best = 0ull;
    u32 w;

    if (c == (LxProc *)0) {
        return 0ull;
    }
    now = port_cycles();
    for (w = 0u; w < 3u; w++) {
        u64 dl = (w == 0u) ? c->alarm_deadline : c->itimer_deadline[w];

        if (dl == 0ull) {
            continue;
        }
        if (dl <= now) {
            return 1ull;               /* 已经到点：让调用方尽快再检查一次 */
        }
        if (best == 0ull || (dl - now) < best) {
            best = dl - now;
        }
    }
    return best;
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

    lx_timerfd_check();       /* M21：timerfd 到期与 alarm 共用检查点 */
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;
        u32 w;

        if (q == (LxProc *)0 || q->state == LX_P_FREE) {
            continue;
        }
        /* M53：三个 itimer 共用一个检查点。REAL 复用 alarm_deadline
         * （与 alarm(2) 同一定时器），VIRTUAL/PROF 用 itimer_deadline[1/2]。
         * 到期后若有 interval 则重 armed 到下一个周期，否则清零。
         * VIRTUAL/PROF 按墙钟近似（协作内核无 CPU 时间统计），发同一 SIGALRM。 */
        for (w = 0u; w < 3u; w++) {
            u64 *dl = (w == 0u) ? &q->alarm_deadline : &q->itimer_deadline[w];
            u64 iv = q->itimer_interval[w];

            if (*dl == 0ull || now < *dl) { continue; }
            lx_sig_send_one(q, (u32)LX_SIGALRM, 0);
            *dl = (iv != 0ull) ? (now + iv) : 0ull;
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

/* ---- 自旋等待里的"定时器 + 串口"节拍 ------------------------------------
 *
 * 这个内核的等待分两种：真阻塞（lx_proc_block）和自旋（网卡必须由本进程
 * 泵，串口也只能在系统调用里泵）。真阻塞那条路有 resched_ 兜底，会给到点
 * 的 itimer 投 SIGALRM；**自旋那条路以前什么都没有**，于是两个后果：
 *
 *   1. busybox ping 的节奏是 setitimer(ITIMER_REAL)：到点发下一个包。它在
 *      recv 里自旋等应答，闹钟永远不响 —— `ping 8.8.8.8` 只发出第一个包，
 *      之后一直 recvfrom 拿 EAGAIN（屏幕上刷 "Resource temporarily
 *      unavailable"），应答回来也收不住，-c N / -W N 全都失效。
 *   2. 自旋期间串口也没人泵，Ctrl-C 根本进不了行规程。
 *
 * 所以凡是自旋等待的循环，每轮都调一次这里：走到点的闹钟/定时器 + 泵一次
 * 串口。返回 1 表示"该进程有未屏蔽的信号要处理"，调用方按 Linux 语义收尾
 * （见 lx_proc_wait_intr）。
 *
 * 这里**不能**调 lx_sig_deliver_frame：那需要 iret 帧，而自旋循环深处只有
 * 普通内核栈。让系统调用先返回、在 lx_syscall 出口投递才对——这也正是
 * "信号只在系统调用边界生效"的 Linux 语义。 */
int lx_proc_wait_tick(void)
{
    LxProc *c = cur_();

    lx_proc_alarm_check();             /* alarm/itimer/timerfd 到点投信号 */
    if (lx_tty_enabled() != 0u) {
        (void)lx_tty_pump();           /* 串口：^C/^D/排队的命令行 */
    }
    if (c == (LxProc *)0) {
        return 0;
    }
    return ((c->sig_pending & ~c->sig_mask) != 0ull) ? 1 : 0;
}

/* 自旋等待收尾：有信号时按 Linux 语义返回 EINTR（带 SA_RESTART 的继续等）。
 * return 1 表示调用方应该立刻返回 EINTR。 */
int lx_proc_wait_intr(void)
{
    LxProc *c = cur_();
    u32 s;

    if (c == (LxProc *)0) {
        return 0;
    }
    for (s = 1u; s < (u32)LX_NSIG; s++) {
        u64 h;

        if ((c->sig_pending & ~c->sig_mask & (1ull << (s - 1u))) == 0ull) {
            continue;
        }
        h = c->sa[s].handler;
        if (h == LX_SIG_IGN) {
            c->sig_pending &= ~(1ull << (s - 1u));   /* 忽略：清掉，不打断 */
            continue;
        }
        /* 默认动作就是"忽略"的几个（SIGCHLD/SIGWINCH/SIGURG/SIGCONT）也不该
         * 打断系统调用 —— Linux 上它们连 pending 都不会留。和 sig.c 的
         * default_() 保持一致。 */
        if (h == LX_SIG_DFL &&
            (s == (u32)LX_SIGCHLD || s == (u32)LX_SIGWINCH ||
             s == (u32)LX_SIGURG || s == (u32)LX_SIGCONT)) {
            c->sig_pending &= ~(1ull << (s - 1u));
            continue;
        }
        if (h != LX_SIG_DFL && (c->sa[s].flags & LX_SA_RESTART) != 0ull) {
            return 0;                  /* 自动重启：继续等（信号仍会投递） */
        }
        c->intr = 0u;
        c->intr_sig = s;
        return 1;
    }
    return 0;
}

/* 当前进程还有没有未屏蔽的待投递信号？
 *
 * 和 lx_proc_wait_intr 的区别：后者回答"这个信号该不该让系统调用返回
 * EINTR"（带 SA_RESTART 的 handler 会说"不用"）。这里只回答"有没有东西要
 * 投递"——自旋等待到了预算上限时用得上：哪怕 handler 带 SA_RESTART，
 * 也必须先回用户态让它跑一次，不能在内核里把 EAGAIN 交给调用方。 */
int lx_proc_sig_pending(void)
{
    LxProc *c = cur_();

    if (c == (LxProc *)0) {
        return 0;
    }
    return ((c->sig_pending & ~c->sig_mask) != 0ull) ? 1 : 0;
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
    u64 key = (c->as << 32) | (ct & 0xffffffffull);
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
    /* pthread_join 在 clear_child_tid 地址上等；只叫醒这个 futex 的等待者，
     * 不能像旧实现那样把所有 BLOCK 都改成 READY（会误醒无关的 futex）。 */
    for (i = 0u; i < g_proc_cap; i++) {
        LxProc *q = g_proc[i].p;

        if (q != (LxProc *)0 && q->state == LX_P_BLOCK &&
            ct != 0ull && q->wait_kind == LX_WAIT_FUTEX &&
            q->futex_key == key) {
            q->futex_key = 0ull;
            q->state = LX_P_READY;
        }
    }
    /* 组长还持有这个 AS 的引用，所以这里不会把页表拆掉 */
    port_as_unref(c->as);
    c->as = 0ull;
    next = pick_next_(g_cur);
    if (next < 0) {
        port_puts("\n[lx] 所有进程都已退出，内核停在这里\n");
        port_qemu_exit();
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
        port_qemu_exit();
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
    {
        /* M51：资源上限与 nice 随 fork 继承（Linux 语义）。 */
        u32 ri;
        for (ri = 0u; ri < LX_NLIMITS; ri++) {
            ch->rlim[ri] = par->rlim[ri];
            ch->rlim_max[ri] = par->rlim_max[ri];
        }
        ch->nice = par->nice;
    }
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
            lx_fs_ref_ent(&ch->fd->ent[i]);    /* VFS 句柄同理：最后一个 close 才关 */
        }
    }
    setup_trampoline_(ch);
    /* 不主动让子进程先跑：busybox wget 的 SSL helper 是先 fork 后由父进程
     * 写 HTTP 请求的。子进程抢先跑完握手就可能因网络先可读而退出。 */
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

/* waitid(2)（nr 247）：和 wait4 共用同一套子进程状态机，但事件选择用
 * WEXITED|WSTOPPED|WCONTINUED 位、目标用 idtype(P_PID/P_PGID/P_ALL)+id，
 * 结果填 128B siginfo（si_signo=SIGCHLD, si_code=CLD_*, si_pid/si_status）。
 * WNOWAIT 只观察不收尸（僵尸留给后续 wait4）。 */
u64 lx_proc_waitid(u64 idtype, u64 id, u64 uinfo, u64 options, u64 urusage)
{
    LxProc *par = cur_();
    u64 want = options & ((u64)LX_WEXITED | (u64)LX_WUNTRACED |
                          (u64)LX_WCONTINUED);
    i64 sel = -1;
    u64 extra = options & ~((u64)LX_WEXITED | (u64)LX_WUNTRACED |
                            (u64)LX_WCONTINUED | (u64)LX_WNOHANG |
                            (u64)LX_WNOWAIT);
    int nowait = (options & (u64)LX_WNOWAIT) != 0u;

    if (par == (LxProc *)0) {
        return n_((u64)LX_ECHILD);
    }
    if (extra != 0ull || want == 0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (uinfo != 0ull &&
        port_as_user_range_ok(par->as, uinfo, 128ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    /* idtype → wait4 风格的 pid 选择器 */
    if (idtype == 1ull) {                  /* P_PID */
        sel = (i64)(i32)id;
        if (sel <= 0) { return n_((u64)LX_EINVAL); }
    } else if (idtype == 2ull) {           /* P_PGID */
        i64 pg = (i64)(i32)id;
        if (pg <= 0) { return n_((u64)LX_EINVAL); }
        sel = -pg;
    } else if (idtype == 0ull) {           /* P_ALL：id 被忽略 */
        sel = -1;
    } else {
        return n_((u64)LX_EINVAL);
    }
    for (;;) {
        u32 i;
        int has_child = 0;

        for (i = 0u; i < g_proc_cap; i++) {
            LxProc *c = g_proc[i].p;
            int code = 0;
            int status = 0;
            int report = 0;

            if (c == (LxProc *)0 || c->state == LX_P_FREE ||
                !waited_(par, c, sel)) {
                continue;
            }
            has_child = 1;
            if (c->state == LX_P_ZOMBIE && (want & (u64)LX_WEXITED) != 0ull) {
                report = 1;
                code = (c->killed != 0u) ?
                       (((c->code & 0x7f) == 6) ? 3 : 2) : 1;  /* CLD_DUMPED/KILLED/EXITED */
                status = (c->killed != 0u) ? (int)(c->code & 0x7full)
                                           : (int)(c->code & 0xffull);
            } else if (c->state == LX_P_STOP && (want & (u64)LX_WUNTRACED) != 0ull) {
                report = 1;
                code = 5;                  /* CLD_STOPPED */
                status = (int)c->stopsig;
            } else if (c->state != LX_P_STOP && c->cont_seen != 0ull &&
                       (want & (u64)LX_WCONTINUED) != 0ull) {
                report = 1;
                code = 6;                  /* CLD_CONTINUED */
                status = (int)((c->code & 0xffull) ? (c->code & 0xffull) : 17u); /* SIGCONT */
            }
            if (report == 0) {
                c->cont_seen = 0ull;
                continue;
            }
            if (uinfo != 0ull) {
                u8 *s = (u8 *)uinfo;
                u32 k;

                for (k = 0u; k < 128u; k++) { s[k] = 0u; }
                *(u32 *)(s + 0u) = (u32)17;    /* si_signo = SIGCHLD */
                *(i32 *)(s + 8u) = code;       /* si_code @8 */
                *(u32 *)(s + 16u) = c->pid;    /* si_pid @16 */
                *(i32 *)(s + 24u) = status;    /* si_status @24 */
                /* si_utime/stime @32/40：填 0，下层不关心 */
            }
            fill_rusage_(urusage);
            if (!nowait) {
                if (c->state == LX_P_ZOMBIE) {
                    c->state = LX_P_FREE;
                    port_as_unref(c->as);
                    c->as = 0ull;
                } else if (c->state == LX_P_STOP) {
                    c->resumed = 0u;
                } else {
                    c->cont_seen = 0ull;
                }
            }
            return 0ull;                   /* waitid 成功返 0，结果在 siginfo */
        }
        /* WNOHANG：没有可报告事件一律返回 0（即使没有子进程），Linux 语义。
         * 必须先于 ECHILD 判断，否则 waitid(WNOHANG) 会误报 ECHILD。 */
        if ((options & (u64)LX_WNOHANG) != 0ull) {
            return 0ull;
        }
        if (has_child == 0) {
            return n_((u64)LX_ECHILD);
        }
        par->wait_kind = LX_WAIT_CHILD;
        lx_proc_block();
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
    {
        u32 ei;
        for (ei = 0u; ei + 1u < (u32)sizeof(cur_()->exe) && path[ei] != '\0'; ei++) {
            cur_()->exe[ei] = path[ei];
        }
        cur_()->exe[ei] = '\0';
    }
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

        if (q != (LxProc *)0 && q->state == LX_P_BLOCK &&
            q->wait_kind != LX_WAIT_FUTEX) {
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
    LxPipe *pp;

    /* socket fd 和管道端点一样按 fd 计数：dup2/fork/CLOEXEC/退出路径都走
     * 这里，最后一份引用才让 SAL 真正关闭 sd。 */
    if (fe->kind == LX_FD_SOCK) {
        lx_sock_drop(fe->node);
        return;
    }
    if (fe->kind == LX_FD_UNIX) {
        lx_unix_drop(fe->node);
        return;
    }
    pp = pipe_at_(fe->node);
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

/* fd 表销毁（进程退出 / 线程组释放）时逐槽调用。pipe_drop_ 只管管道类；
 * VFS 文件（/data FAT、ramfs）在这里把内核句柄还回去 —— 以前只挂
 * pipe_drop_，子进程退出时那一个 `cmd > /data/x` 的 fd 没人收，FAT_MAX_FILES
 * 8 个槽几十次 fork 就漏光，之后 open 全是 ENOMEM。close/dup2 覆盖也走
 * lx_fs_release_vfs，三条路径共用一份回收逻辑。 */
static void fd_free_hook_(LxFile *fe)
{
    if (fe == (LxFile *)0) {
        return;
    }
    if (fe->kind == LX_FD_VFS) {
        lx_fs_release_vfs(fe);
        return;
    }
    if (fe->kind == LX_FD_DEV && fe->dev != 0u) {
        lx_dev_release(lx_dev_of(fe->node));   /* G2：设备打开实例引用 -1 */
        return;
    }
    pipe_drop_(fe);
}

/* 增加一个端点引用（dup2 / fork 复制 fd 表时用）。 */
static void pipe_ref_(const LxFile *fe)
{
    LxPipe *pp;

    if (fe->kind == LX_FD_DEV) {
        lx_dev_ref(lx_dev_of(fe->node));      /* G2：设备句柄 dup/fork 共享 */
        return;
    }
    if (fe->kind == LX_FD_SOCK) {
        lx_sock_ref(fe->node);
        return;
    }
    if (fe->kind == LX_FD_UNIX) {
        lx_unix_ref(fe->node);
        return;
    }
    pp = pipe_at_(fe->node);
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

/* M19：getgroups(2)。单用户模型里进程只有主组 0，没有 supplementary groups。
 * size=0 时返回组数（1），供 busybox id 先探测再分配缓冲。 */
u64 lx_proc_getgroups(u64 size, u64 ugroups)
{
    if (size == 0ull) {
        return 1ull;
    }
    if (size < 1ull) {
        return n_((u64)LX_EINVAL);
    }
    if (lx_user_ok(ugroups, 4ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    *(u32 *)ugroups = 0u;
    return 1ull;
}

/* M19：sysinfo(2)。busybox top/free 读 uptime/loads/mem/totalram 等；
 * x86-64 struct sysinfo 是 112 字节（uptime 0 / loads 8 / totalram 32 /
 * freeram 40 / sharedram 48 / bufferram 56 / totalswap 64 / freeswap 72 /
 * procs 80 / totalhigh 88 / freehigh 96 / 余下保留）。给一组与 meminfo 自洽的常量。 */
u64 lx_proc_sysinfo(u64 ubuf)
{
    u64 i;
    u8 si[112];

    if (lx_user_ok(ubuf, 112ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    for (i = 0ull; i < 112ull; i++) {
        si[i] = 0u;
    }
    /* 逐字段写小端 u64：uptime=0 / loads[3]=8,16,24（全 0 已清零）/
     * totalram=32 / freeram=40 / sharedram=48 / bufferram=56 /
     * totalswap=64 / freeswap=72；procs(u16)=80。 */
    {
        u64 uptime = port_cycles() / 1000000000ull;
        u64 totalram = port_phys_total_kb() * 1024ull;
        u64 freeram = port_phys_free_kb() * 1024ull;
        u64 field[3];
        u64 f;

        field[0] = uptime; field[1] = totalram; field[2] = freeram;
        for (f = 0ull; f < 3ull; f++) {
            u64 at[3] = {0ull, 32ull, 40ull};
            u32 b;

            for (b = 0u; b < 8u; b++) {
                si[at[f] + b] = (u8)((field[f] >> (8u * b)) & 0xffu);
            }
        }
    }
    si[80] = 1u; si[81] = 0u;                   /* procs (u16) */
    /* mem_unit (u32 @104)：sysinfo 的所有内存字段都以它为倍率；必须为 1，
     * 否则 libc 算出来是 0 或错误值。 */
    si[104] = 1u; si[105] = 0u; si[106] = 0u; si[107] = 0u;
    for (i = 0ull; i < 112ull; i++) {
        *(u8 *)(ubuf + i) = si[i];
    }
    return 0ull;
}

/* M19：sendfile(2)。busybox cat 从 procfs 输出到 stdout 走它。
 * off=NULL 时用 infd 当前位置并推进（和 read 一样）；off!=NULL 用给定偏移、
 * 不推进 infd、回写新偏移。用 4KB 内核中转缓冲，read/write 复用各层分派。 */
static u64 write_kbuf_(u64 fd, const u8 *src, u64 len);  /* 定义在 write 附近 */

u64 lx_proc_sendfile(u64 outfd, u64 infd, u64 uoff, u64 count)
{
    u8 *tmp;
    u64 off = 0ull;
    int have_off = (uoff != 0ull);
    u64 done = 0ull;

    if (have_off) {
        if (lx_user_ok(uoff, 8ull, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        off = *(const u64 *)uoff;
    }
    tmp = (u8 *)port_kcalloc(1ull, 4096ull);
    if (tmp == (u8 *)0) {
        return n_((u64)LX_ENOMEM);
    }
    while (done < count) {
        u64 chunk = (count - done < 4096ull) ? count - done : 4096ull;
        u64 nr;
        u64 nw;

        if (have_off) {
            nr = lx_fs_pread_k(infd, (u64)tmp, chunk, off);
        } else {
            LxFile *f = lx_fdtab_get(lx_cur_proc() != (LxProc *)0
                                     ? lx_cur_proc()->fd : (LxFdTab *)0, infd);
            u64 pos = (f != (LxFile *)0) ? f->pos : 0ull;

            nr = lx_fs_pread_k(infd, (u64)tmp, chunk, pos);  /* 内核缓冲直读 */
            if ((i64)nr > 0 && f != (LxFile *)0) {
                f->pos += nr;                               /* 推进 in_fd 游标 */
            }
        }
        if ((i64)nr <= 0) {
            break;
        }
        nw = write_kbuf_(outfd, tmp, nr);
        if ((i64)nw <= 0) {
            if (done == 0ull) { done = n_((u64)LX_EIO); }
            break;
        }
        done += nw;
        off += nw;
        if (nw < nr) {
            break;
        }
    }
    port_kfree((void *)tmp);
    if (have_off && lx_user_ok(uoff, 8ull, C6OS_PROT_W) != 0) {
        *(u64 *)uoff = off;
    }
    return done;
}

static void ep_release_(u64 id);   /* M20：定义在 epoll 块，close 里释放注册表 */
static u16 poll_events_(LxProc *p, i32 fd);  /* M20：epoll_wait 复用，定义在下方 */
/* ET 用：取 fd 对应管道的写代数（新数据到达会 +1）；非管道返回 0。 */
static u64 fd_gen_(LxProc *p, i32 fd)
{
    const LxFile *f = lx_fdtab_get(p->fd, (u64)fd);
    const LxPipe *pp;

    if (f == (const LxFile *)0 || f->kind != LX_FD_PIPE_R) { return 0ull; }
    pp = pipe_at_(f->node);
    return (pp != (const LxPipe *)0) ? pp->gen : 0ull;
}
static void tfd_release_(u64 id);  /* M21：定义在 timerfd 块，close 里释放对象 */
static void sfd_release_(u64 id);  /* M22：定义在 signalfd 块，close 里释放对象 */

/* AF_UNIX connect 唤醒阻塞在 accept() 的服务端（wake_all_ 是 proc.c 私有）。 */
void lx_unix_wake(void)
{
    wake_all_();
}

/* G2：evdev 注入事件后唤醒被 read()/poll() 阻塞的进程。 */
void lx_proc_wake_all(void)
{
    wake_all_();
}

/* SCM_RIGHTS / dup：按 kind 增加 fd 携带对象的引用计数。 */
void lx_unix_fd_acquire(u32 node, u32 kind)
{
    LxFile tmp;

    tmp.node = (u64)node;
    tmp.kind = kind;
    if (kind == LX_FD_UNIX) {
        lx_unix_ref((u64)node);
        return;
    }
    if (kind == LX_FD_PIPE_R || kind == LX_FD_PIPE_W) {
        pipe_ref_(&tmp);
    }
}

/* SCM_RIGHTS：把对端发来的 LxFile 值副本安装到当前进程的新 fd 槽。 */
u64 lx_unix_fd_install(const LxFile *src)
{
    LxProc *p = cur_();
    u64 fd;

    if (p == (LxProc *)0 || src == (LxFile *)0) {
        return (u64)0 - LX_EBADF;
    }
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) {
        return (u64)0 - LX_EMFILE;
    }
    p->fd->ent[fd] = *src;
    lx_unix_fd_acquire((u32)src->node, src->kind);
    return fd;
}

u64 lx_proc_close(u64 fd)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);

    if (fe == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (fe->kind == LX_FD_SOCK) {
        (void)lx_sock_close(fe->node);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    if (fe->kind == LX_FD_UNIX) {
        (void)lx_unix_close(fd);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    if (fe->kind == LX_FD_PTY) {
        lx_pty_close(fe->node, fe->pos == 0ull ? 1u : 0u);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    if (fe->kind == LX_FD_EPOLL) {
        ep_release_(fe->pos);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    if (fe->kind == LX_FD_TIMERFD) {
        tfd_release_(fe->pos);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    if (fe->kind == LX_FD_SIGNALFD) {
        sfd_release_(fe->pos);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    /* VFS 文件（/data FAT、/tmp ramfs等）必须走 lx_fs_close 释放内核侧
     * 文件句柄（FAT_MAX_FILES 池）。以前落进下面的通用 pipe_drop_ 分支，
     * 句柄永不归还，几个 open/close 之后就 "Out of memory"。 */
    if (fe->kind == LX_FD_DEV && fe->dev != 0u) {
        lx_dev_release(lx_dev_of(fe->node));
        fe->node = LX_FD_NONE;
        fe->dev = 0u;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
    }
    if (fe->kind == LX_FD_VFS) {
        (void)lx_fs_close(fd);
        fe->node = LX_FD_NONE;
        fe->kind = LX_FD_FILE;
        wake_all_();
        return 0ull;
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
    if (nf->node != LX_FD_NONE && nf->kind == LX_FD_VFS) {
        lx_fs_release_vfs(nf);                 /* 覆盖前把 FAT 句柄还回去 */
    } else if (nf->node != LX_FD_NONE && nf->kind == LX_FD_DEV &&
               nf->dev != 0u) {
        lx_dev_release(lx_dev_of(nf->node));   /* 覆盖前放掉旧设备引用 */
    } else if (nf->node != LX_FD_NONE && nf->kind != LX_FD_FILE) {
        pipe_drop_(nf);
    }
    *nf = *of;
    pipe_ref_(nf);
    lx_fs_ref_ent(nf);                         /* 后端句柄再多一份引用 */
    /* Linux：dup2 的目标 fd 必须清除 FD_CLOEXEC（只有 dup3 可以再设）。
     * ash 用 pipe2(O_CLOEXEC) 建管道再 dup2 到 0/1；不清这一点的话，
     * execve 时会把 dup 出的 stdin/stdout 关掉，外部命令管道全失效。 */
    nf->flags &= ~(u32)LX_FD_CLOEXEC;
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

/* M52/G1：POSIX 记录锁。只做进程级、整段/区间冲突检测；同一进程（含线程）
 * 之间不冲突（Linux 的 fcntl 锁归进程所有）。F_SETLKW 在冲突时先返回
 * EAGAIN——Chrome 的 profile 锁实际走 F_SETLK + 重试。 */
typedef struct {
    u32 used;
    u32 node;
    u32 tgid;
    u32 type;
    i64 start;
    i64 len;
} LxLock;

static LxLock g_locks[128];

static int lock_range_(const LxLock *l, i64 start, i64 len)
{
    i64 a0 = l->start;
    i64 a1 = (l->len == 0) ? (i64)0x7fffffffffffffffLL : l->start + l->len;
    i64 b1 = (len == 0) ? (i64)0x7fffffffffffffffLL : start + len;

    return (a0 < b1 && start < a1);
}

static void lock_set_(u32 node, u32 tgid, u32 type, i64 start, i64 len)
{
    u32 i;

    for (i = 0u; i < 128u; i++) {
        if (g_locks[i].used != 0u && g_locks[i].node == node &&
            g_locks[i].tgid == tgid && lock_range_(&g_locks[i], start, len)) {
            g_locks[i].used = 0u;
        }
    }
    if (type == 2u) {                         /* F_UNLCK */
        return;
    }
    for (i = 0u; i < 128u; i++) {
        if (g_locks[i].used == 0u) {
            g_locks[i].used = 1u;
            g_locks[i].node = node;
            g_locks[i].tgid = tgid;
            g_locks[i].type = type;
            g_locks[i].start = start;
            g_locks[i].len = len;
            return;
        }
    }
}

static u64 fcntl_lock_(LxProc *p, LxFile *fe, u64 cmd, u64 arg)
{
    u8 fl[32];
    u32 i;
    u32 node = (u32)fe->node;
    i64 start;
    i64 len;
    u32 type;

    if (lx_user_ok(arg, 32ull, (cmd == 5ull) ? C6OS_PROT_W : C6OS_PROT_R) == 0) {
        return n_((u64)LX_EFAULT);
    }
    if (cmd != 5ull && lx_user_ok(arg, 32ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    for (i = 0u; i < 32u; i++) {
        fl[i] = *(const u8 *)(arg + i);
    }
    type = (u32)(fl[0] | ((u32)fl[1] << 8));
    start = *(const i64 *)(fl + 8);
    len = *(const i64 *)(fl + 16);
    if (cmd == 5ull) {                        /* F_GETLK：只报告别人的锁 */
        for (i = 0u; i < 128u; i++) {
            if (g_locks[i].used != 0u && g_locks[i].node == node &&
                g_locks[i].tgid != p->tgid &&
                (type == 2u || g_locks[i].type == type) &&
                lock_range_(&g_locks[i], start, len)) {
                *(u16 *)(arg + 0ull) = (u16)g_locks[i].type;
                *(u16 *)(arg + 2ull) = 0u;
                *(i64 *)(arg + 8ull) = g_locks[i].start;
                *(i64 *)(arg + 16ull) = g_locks[i].len;
                *(u32 *)(arg + 24ull) = g_locks[i].tgid;
                return 0ull;
            }
        }
        *(u16 *)(arg + 0ull) = 2u;            /* F_UNLCK */
        *(u16 *)(arg + 2ull) = 0u;
        *(i64 *)(arg + 8ull) = start;
        *(i64 *)(arg + 16ull) = len;
        return 0ull;
    }
    if (type > 2u) {
        return n_((u64)LX_EINVAL);
    }
    if (type != 2u) {
        for (i = 0u; i < 128u; i++) {
            if (g_locks[i].used != 0u && g_locks[i].node == node &&
                g_locks[i].tgid != p->tgid &&
                lock_range_(&g_locks[i], start, len)) {
                return n_((u64)LX_EACCES);
            }
        }
    }
    lock_set_(node, p->tgid, type, start, len);
    return 0ull;
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
    case 1033ull:                               /* F_ADD_SEALS */
    case 1034ull:                               /* F_GET_SEALS */
        if (lx_fdtab_get(p->fd, fd) == (LxFile *)0) {
            return n_((u64)LX_EBADF);
        }
        return lx_fs_seals(fd, (u32)cmd, (u32)arg);
    case 5ull:                                  /* F_GETLK */
    case 6ull:                                  /* F_SETLK */
    case 7ull:                                  /* F_SETLKW */
        fe = lx_fdtab_get(p->fd, fd);
        if (fe == (LxFile *)0) {
            return n_((u64)LX_EBADF);
        }
        return fcntl_lock_(p, fe, cmd, arg);
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
            pp->gen++;                     /* 新数据到达 = 一个新边沿（epoll ET） */
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

/* M16-1b：eventfd2 —— 一个 64 位计数器。read 返回计数并清零（阻塞语义在
 * 协作调度下简化为"无计数就返回 0"，与 select 的 POLLIN 判据一致）；write
 * 把用户给的 64 位值加到计数上。对象表在内核堆上按需倍增，不占 .bss。
 * LxFile.pos 存对象 id，node 固定为 LX_FD_EVENTFD 的哨兵。 */
#define LX_EFD_SENTINEL 0xfffffff5ull

typedef struct {
    u64 counter;
    u32 used;
    u32 flags;
} LxEfd;
static LxEfd *g_efd;
static u32 g_efd_cap;

static int efd_grow_(void)
{
    u32 ncap = (g_efd_cap == 0u) ? 8u : g_efd_cap * 2u;
    LxEfd *ne;

    if (ncap > 1024u) {
        ncap = 1024u;
    }
    if (g_efd_cap >= 1024u) {
        return -1;
    }
    ne = (LxEfd *)port_kcalloc((u64)ncap, (u64)sizeof(LxEfd));
    if (ne == (LxEfd *)0) {
        return -1;
    }
    if (g_efd != (LxEfd *)0) {
        u32 i;
        for (i = 0u; i < g_efd_cap; i++) {
            ne[i] = g_efd[i];
        }
        port_kfree((void *)g_efd);
    }
    g_efd = ne;
    g_efd_cap = ncap;
    return 0;
}

u64 lx_proc_eventfd2(u64 initval, u64 flags)
{
    LxProc *p = cur_();
    u64 fd;
    u32 i;

    if (p == (LxProc *)0) {
        return n_((u64)LX_EINVAL);
    }
    /* EFD_CLOEXEC(0x80000)/EFD_NONBLOCK(0x800) 之外的位非法 */
    if ((flags & ~(u64)(0x80000ull | 0x800ull)) != 0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (initval == 0xffffffffffffffffull) {
        return n_((u64)LX_EINVAL);
    }
    /* 先找一个空位；表不存在或全满就扩容（扩完再找一次）。 */
    for (i = 0u; i < g_efd_cap; i++) {
        if (g_efd[i].used == 0u) {
            break;
        }
    }
    if (i >= g_efd_cap) {
        if (efd_grow_() != 0) {
            return n_((u64)LX_ENOMEM);
        }
        for (i = 0u; i < g_efd_cap; i++) {
            if (g_efd[i].used == 0u) {
                break;
            }
        }
    }
    if (i >= g_efd_cap) {
        return n_((u64)LX_ENOMEM);
    }
    g_efd[i].used = 1u;
    g_efd[i].flags = (u32)flags;
    g_efd[i].counter = initval;
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) {
        g_efd[i].used = 0u;
        return n_((u64)LX_EMFILE);
    }
    {
        LxFile *ent = &p->fd->ent[fd];

        ent->node = LX_EFD_SENTINEL;
        ent->pos = (u64)i;                 /* pos 复用为对象 id */
        ent->is_dir = 0u;
        ent->kind = LX_FD_EVENTFD;
        ent->flags = 0u;
        ent->oflags = 0u;
        if ((flags & 0x80000ull) != 0ull) {
            ent->flags = LX_FD_CLOEXEC;
        }
    }
    return fd;
}

u64 lx_proc_eventfd_read(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    LxEfd *e;

    if (fe == (LxFile *)0 || fe->kind != LX_FD_EVENTFD || len < 8ull) {
        return n_((u64)LX_EINVAL);
    }
    if (port_as_user_range_ok(p->as, ubuf, 8ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    e = &g_efd[fe->pos];
    *(u64 *)ubuf = e->counter;
    e->counter = 0ull;
    return 8ull;
}

u64 lx_proc_eventfd_write(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    LxEfd *e;
    u64 v;

    if (fe == (LxFile *)0 || fe->kind != LX_FD_EVENTFD || len < 8ull) {
        return n_((u64)LX_EINVAL);
    }
    if (port_as_user_range_ok(p->as, ubuf, 8ull, C6OS_PROT_R) == 0) {
        return n_((u64)LX_EFAULT);
    }
    v = *(const u64 *)ubuf;
    if (v == 0xffffffffffffffffull) {
        return n_((u64)LX_EINVAL);
    }
    e = &g_efd[fe->pos];
    e->counter += v;
    wake_all_();
    return 8ull;
}

/* ---- M20：epoll（复用 poll_events_ 的就绪判定 + 等待队列）----
 * 单全局对象表（类似 eventfd），fd 的 node = 对象 id。注册表按 ADD 倍增。
 * 不实现 EPOLLONESHOT/EPOLLET（默认水平触发），对 Alpine/busybox 的
 * poll 替代路径足够；ET 语义可在后续扩展。 */
#define LX_EPOLL_SENTINEL 0xfffffff6ull

/* EPOLLET(1<<31) / EPOLLONESHOT(1<<30)：边沿触发与一次性通知。
 * prev_ready 记录上次上报时的就绪位，ET 只在"新出现"的位上再报；
 * fired 表示 ONESHOT 已报过一次，须 MOD/ADD 重新武装。 */
#define LX_EPOLLET      0x80000000u
#define LX_EPOLLONESHOT 0x40000000u

typedef struct {
    i32 fd;
    u32 events;                        /* 用户注册的 EPOLL* 位（含 ET/ONESHOT） */
    u32 prev_ready;                    /* ET：上次上报的就绪位 */
    u32 fired;                         /* ONESHOT：已上报过 */
    u64 last_gen;                      /* ET：上次上报时管道的写代数 */
    u64 data;                          /* 原样回传 */
} LxEpItem;

typedef struct {
    LxEpItem *items;
    u32 n;
    u32 cap;
    u32 used;
} LxEpoll;

static LxEpoll *g_ep;
static u32 g_ep_cap;

static LxEpoll *ep_at_(u64 id)
{
    if (g_ep == (LxEpoll *)0 || id >= g_ep_cap || g_ep[id].used == 0u) {
        return (LxEpoll *)0;
    }
    return &g_ep[id];
}

/* close(fd) 时释放注册表；定义在 epoll 块，由前置声明供 lx_proc_close 调用。 */
static void ep_release_(u64 id)
{
    LxEpoll *ep = ep_at_(id);

    if (ep == (LxEpoll *)0) {
        return;
    }
    if (ep->items != (LxEpItem *)0) {
        port_kfree((void *)ep->items);
        ep->items = (LxEpItem *)0;
    }
    ep->n = 0u;
    ep->cap = 0u;
    ep->used = 0u;
}

static int ep_grow_(void)
{
    u32 ncap = (g_ep_cap == 0u) ? 8u : g_ep_cap * 2u;
    LxEpoll *ne;

    if (ncap > 1024u) {
        return -1;
    }
    ne = (LxEpoll *)port_kcalloc((u64)ncap, (u64)sizeof(LxEpoll));
    if (ne == (LxEpoll *)0) {
        return -1;
    }
    if (g_ep != (LxEpoll *)0) {
        u32 i;

        for (i = 0u; i < g_ep_cap; i++) {
            ne[i] = g_ep[i];           /* 对象按值搬，注册表指针一起搬 */
        }
        port_kfree((void *)g_ep);
    }
    g_ep = ne;
    g_ep_cap = ncap;
    return 0;
}

u64 lx_epoll_create1(u64 flags)
{
    LxProc *p = cur_();
    u64 fd;
    u32 i;

    if (p == (LxProc *)0 || (flags & ~(u64)0x80000ull) != 0ull) {
        return n_((u64)LX_EINVAL);     /* 仅 EPOLL_CLOEXEC 合法 */
    }
    for (i = 0u; i < g_ep_cap; i++) {
        if (g_ep[i].used == 0u) { break; }
    }
    if (i >= g_ep_cap) {
        if (ep_grow_() != 0) { return n_((u64)LX_ENOMEM); }
        for (i = 0u; i < g_ep_cap; i++) {
            if (g_ep[i].used == 0u) { break; }
        }
    }
    if (i >= g_ep_cap) { return n_((u64)LX_ENOMEM); }
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) {
        return n_((u64)LX_EMFILE);
    }
    g_ep[i].used = 1u;
    g_ep[i].items = (LxEpItem *)0;
    g_ep[i].n = 0u;
    g_ep[i].cap = 0u;
    {
        LxFile *ent = &p->fd->ent[fd];

        ent->node = LX_EPOLL_SENTINEL;
        ent->pos = (u64)i;
        ent->is_dir = 0u;
        ent->kind = LX_FD_EPOLL;
        ent->flags = (flags & 0x80000ull) ? LX_FD_CLOEXEC : 0u;
        ent->oflags = 0u;
    }
    return fd;
}

u64 lx_epoll_ctl(u64 efd, u64 op, u64 fd, u64 uev)
{
    LxProc *p = cur_();
    LxEpoll *ep;
    LxFile *fe;

    /* efd 是 fd，node 才是对象 id */
    fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, efd);
    if (fe == (LxFile *)0 || fe->kind != LX_FD_EPOLL) {
        return n_((u64)LX_EBADF);
    }
    ep = ep_at_(fe->pos);
    if (ep == (LxEpoll *)0) {
        return n_((u64)LX_EBADF);
    }
    if (lx_fdtab_get(p->fd, fd) == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (op == 1ull) {                  /* EPOLL_CTL_ADD */
        u32 k;

        for (k = 0u; k < ep->n; k++) {
            if (ep->items[k].fd == (i32)fd) {
                return n_((u64)LX_EEXIST);
            }
        }
        if (ep->n == ep->cap) {
            u32 ncap = (ep->cap == 0u) ? 8u : ep->cap * 2u;
            LxEpItem *ni = (LxEpItem *)port_kcalloc((u64)ncap,
                                                    (u64)sizeof(LxEpItem));

            if (ni == (LxEpItem *)0) { return n_((u64)LX_ENOMEM); }
            for (k = 0u; k < ep->n; k++) { ni[k] = ep->items[k]; }
            if (ep->items != (LxEpItem *)0) { port_kfree((void *)ep->items); }
            ep->items = ni;
            ep->cap = ncap;
        }
        if (lx_user_ok(uev, 12ull, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        ep->items[ep->n].fd = (i32)fd;
        ep->items[ep->n].events = *(const u32 *)uev;
        ep->items[ep->n].data = *(const u64 *)(uev + 4ull);
        ep->items[ep->n].prev_ready = 0u;
        ep->items[ep->n].fired = 0u;
        ep->items[ep->n].last_gen = 0u;
        ep->n++;
        return 0ull;
    }
    if (op == 2ull || op == 3ull) {    /* DEL / MOD */
        u32 k;

        for (k = 0u; k < ep->n; k++) {
            if (ep->items[k].fd != (i32)fd) { continue; }
            if (op == 2ull) {
                ep->items[k] = ep->items[ep->n - 1u];
                ep->n--;
            } else {
                if (lx_user_ok(uev, 12ull, C6OS_PROT_R) == 0) {
                    return n_((u64)LX_EFAULT);
                }
                ep->items[k].events = *(const u32 *)uev;
                ep->items[k].data = *(const u64 *)(uev + 4ull);
                /* MOD 重新武装 ONESHOT，并重置 ET 基线 */
                ep->items[k].prev_ready = 0u;
                ep->items[k].fired = 0u;
                ep->items[k].last_gen = 0u;
            }
            return 0ull;
        }
        return n_((u64)LX_ENOENT);
    }
    return n_((u64)LX_EINVAL);
}

u64 lx_epoll_wait(u64 efd, u64 uevents, u64 maxevents, i64 timeout)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, efd);
    LxEpoll *ep;
    u64 deadline = 0ull;
    int have_deadline = (timeout > 0);

    if (fe == (LxFile *)0 || fe->kind != LX_FD_EPOLL) {
        return n_((u64)LX_EBADF);
    }
    ep = ep_at_(fe->pos);
    if (ep == (LxEpoll *)0 || maxevents == 0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (lx_user_ok(uevents, 12ull * maxevents, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    if (have_deadline) {
        deadline = port_cycles() + (u64)timeout * 1000000ull;
    }
    for (;;) {
        u64 out = 0ull;
        u32 k;
        u32 tty = 0u;
        u32 has_sock = 0u;

        for (k = 0u; k < ep->n && out < maxevents; k++) {
            LxEpItem *it = &ep->items[k];
            u16 rev = poll_events_(p, it->fd);
            u32 got = (u32)rev & (it->events | 0x18u);  /* ERR/HUP 总报 */

            if (lx_fdtab_get(p->fd, (u64)it->fd) != (LxFile *)0 &&
                lx_fdtab_get(p->fd, (u64)it->fd)->kind == LX_FD_SOCK) {
                has_sock = 1u;
            }

            if (rev != 0u && lx_fdtab_get(p->fd, (u64)it->fd) != (LxFile *)0 &&
                lx_fdtab_get(p->fd, (u64)it->fd)->node == LX_FD_CONSOLE &&
                lx_tty_enabled() != 0u) {
                tty = 1u;
            }
            /* ONESHOT：已报过且未 MOD 重武装的项直接跳过 */
            if ((it->events & LX_EPOLLONESHOT) != 0u && it->fired != 0u) {
                continue;
            }
            /* ET：只在"新出现"的就绪位上报；与上次完全一样则不报。
             * 管道的写代数变了（读空后又写）也算新边沿 —— 单靠 prev_ready
             * 采样不到两次 wait 之间的"空→有"瞬态。 */
            if ((it->events & LX_EPOLLET) != 0u) {
                u64 gen = fd_gen_(p, it->fd);
                u32 cur = (u32)rev & (it->events | 0x18u);

                if (gen != it->last_gen) {
                    got = cur;                 /* 新数据到达：当前就绪位都算新边沿 */
                } else {
                    got &= ~it->prev_ready;    /* 否则只报新出现的位 */
                }
                it->prev_ready = cur;
                it->last_gen = gen;
            }
            if (got != 0u) {
                u8 *dst = (u8 *)(uevents + out * 12ull);

                *(u32 *)dst = got;
                *(u64 *)(dst + 4ull) = it->data;
                out++;
                if ((it->events & LX_EPOLLONESHOT) != 0u) {
                    it->fired = 1u;
                }
            }
        }
        if (out > 0ull || timeout == 0) {
            return out;
        }
        if (have_deadline && port_cycles() >= deadline) {
            return 0ull;
        }
        (void)lx_proc_wait_tick();     /* 串口 + 到点的闹钟/定时器（M60） */
        /* 有信号待投递就先回用户态让它跑一次：handler 带 SA_RESTART 时
         * lx_proc_wait_intr 会说"继续等"，但信号只在系统调用出口投递，
         * 死等下去 handler 永远轮不到（见 lx_proc_sig_pending 的说明）。 */
        if (lx_proc_wait_intr() != 0 || lx_proc_sig_pending() != 0) {
            return n_((u64)LX_EINTR);  /* 被信号打断：先回用户态投递信号 */
        }
        if (tty == 0u) {
            /* 和 poll/select 同理：socket 只有本进程能泵网卡，不能 block。 */
            if (has_sock) {
                continue;
            }
            if (!others_live_(g_cur)) { return 0ull; }
            lx_proc_set_wait(LX_WAIT_PIPE);
            lx_proc_block();
            if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
                return n_((u64)LX_EINTR);
            }
        }
    }
}

/* ---- M21：timerfd（复用 port_cycles 假时钟，与 alarm 同口径）----
 * 全局对象表（类似 eventfd/epoll）。counter 为到期次数；it_value 是下次
 * 到期的绝对 cycles（0=未 armed）；it_interval 非 0 则周期重 armed。
 * 到期在 lx_timerfd_check() 里推进，该函数挂在 alarm_check 的两个调用点。 */
#define LX_TFD_SENTINEL 0xfffffff7ull

typedef struct {
    u64 counter;
    u64 value;                  /* 下次到期绝对 cycles，0=未 armed */
    u64 interval;               /* 周期 cycles，0=单次 */
    u32 used;
    u32 flags;
} LxTimerFd;

static LxTimerFd *g_tfd;
static u32 g_tfd_cap;

static LxTimerFd *tfd_at_(u64 id)
{
    if (g_tfd == (LxTimerFd *)0 || id >= g_tfd_cap || g_tfd[id].used == 0u) {
        return (LxTimerFd *)0;
    }
    return &g_tfd[id];
}

/* close(fd) 时释放对象；定义在 timerfd 块，由前置声明供 lx_proc_close 调用。 */
static void tfd_release_(u64 id)
{
    LxTimerFd *t = tfd_at_(id);

    if (t != (LxTimerFd *)0) {
        t->used = 0u;
    }
}

static int tfd_grow_(void)
{
    u32 ncap = (g_tfd_cap == 0u) ? 8u : g_tfd_cap * 2u;
    LxTimerFd *ne;

    if (ncap > 1024u) {
        return -1;
    }
    ne = (LxTimerFd *)port_kcalloc((u64)ncap, (u64)sizeof(LxTimerFd));
    if (ne == (LxTimerFd *)0) {
        return -1;
    }
    if (g_tfd != (LxTimerFd *)0) {
        u32 i;

        for (i = 0u; i < g_tfd_cap; i++) { ne[i] = g_tfd[i]; }
        port_kfree((void *)g_tfd);
    }
    g_tfd = ne;
    g_tfd_cap = ncap;
    return 0;
}

/* timespec(16B) → cycles；ts=0 表示 0。TV_SEC@0/TV_NSEC@8，1s=1e9 cycles。 */
static u64 ts_cycles_(u64 uts)
{
    i64 sec;
    i64 nsec;

    if (lx_user_ok(uts, 16ull, C6OS_PROT_R) == 0) {
        return ~0ull;
    }
    sec = *(const i64 *)uts;
    nsec = *(const i64 *)(uts + 8ull);
    if (sec < 0 || nsec < 0) {
        return ~0ull;
    }
    return (u64)sec * 1000000000ull + (u64)nsec;
}

/* 反向：cycles → timespec，写回用户缓冲（16B）。 */
static void cycles_ts_(u64 uts, u64 cyc)
{
    u64 sec = cyc / 1000000000ull;
    u64 nsec = cyc % 1000000000ull;

    *(i64 *)uts = (i64)sec;
    *(i64 *)(uts + 8ull) = (i64)nsec;
}

u64 lx_timerfd_create(u64 clockid, u64 flags)
{
    LxProc *p = cur_();
    u64 fd;
    u32 i;

    /* 只支持 CLOCK_MONOTONIC(1)/CLOCK_REALTIME(0)，标志同 eventfd */
    if ((clockid != 0ull && clockid != 1ull) ||
        (flags & ~(u64)(0x80000ull | 0x800ull)) != 0ull) {
        return n_((u64)LX_EINVAL);
    }
    for (i = 0u; i < g_tfd_cap; i++) {
        if (g_tfd[i].used == 0u) { break; }
    }
    if (i >= g_tfd_cap) {
        if (tfd_grow_() != 0) { return n_((u64)LX_ENOMEM); }
        for (i = 0u; i < g_tfd_cap; i++) {
            if (g_tfd[i].used == 0u) { break; }
        }
    }
    if (i >= g_tfd_cap) { return n_((u64)LX_ENOMEM); }
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) { return n_((u64)LX_EMFILE); }
    g_tfd[i].used = 1u;
    g_tfd[i].counter = 0u;
    g_tfd[i].value = 0u;
    g_tfd[i].interval = 0u;
    g_tfd[i].flags = (u32)flags;
    {
        LxFile *ent = &p->fd->ent[fd];

        ent->node = LX_TFD_SENTINEL;
        ent->pos = (u64)i;
        ent->is_dir = 0u;
        ent->kind = LX_FD_TIMERFD;
        ent->flags = (flags & 0x80000ull) ? LX_FD_CLOEXEC : 0u;
        ent->oflags = (flags & 0x800ull) ? LX_O_NONBLOCK : 0u;
    }
    return fd;
}

u64 lx_timerfd_settime(u64 fd, u64 flags, u64 unew, u64 uold)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    LxTimerFd *t;
    u64 value, interval;

    if (fe == (LxFile *)0 || fe->kind != LX_FD_TIMERFD) {
        return n_((u64)LX_EBADF);
    }
    t = tfd_at_(fe->pos);
    if (t == (LxTimerFd *)0 || (flags & ~1ull) != 0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (lx_user_ok(unew, 32ull, C6OS_PROT_R) == 0) {
        return n_((u64)LX_EFAULT);
    }
    value = ts_cycles_(unew + 16ull);       /* it_value */
    interval = ts_cycles_(unew);            /* it_interval */
    if (value == ~0ull || interval == ~0ull) {
        return n_((u64)LX_EINVAL);
    }
    if (uold != 0ull) {
        u64 remain = 0ull;

        if (t->value != 0u) {
            u64 now = port_cycles();
            remain = (now < t->value) ? (t->value - now) : 0ull;
        }
        if (lx_user_ok(uold, 32ull, C6OS_PROT_W) == 0) {
            return n_((u64)LX_EFAULT);
        }
        cycles_ts_(uold + 16ull, remain);   /* it_value@16 = 剩余 */
        cycles_ts_(uold, 0ull);             /* it_interval@0 旧值不追，报 0 */
    }
    t->counter = 0u;
    t->interval = interval;
    if (value == 0ull) {
        t->value = 0u;
    } else {
        /* TFD_TIMER_ABSTIME(1) 时 it_value 是绝对 cycles；否则相对现在 */
        t->value = (flags & 1ull) ? value : port_cycles() + value;
    }
    return 0ull;
}

u64 lx_timerfd_gettime(u64 fd, u64 ucur)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    LxTimerFd *t;
    u64 remain = 0ull;

    if (fe == (LxFile *)0 || fe->kind != LX_FD_TIMERFD) {
        return n_((u64)LX_EBADF);
    }
    t = tfd_at_(fe->pos);
    if (t == (LxTimerFd *)0 || lx_user_ok(ucur, 32ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    if (t->value != 0u) {
        u64 now = port_cycles();
        remain = (now < t->value) ? (t->value - now) : 0ull;
    }
    cycles_ts_(ucur + 16ull, remain);   /* it_value@16 */
    cycles_ts_(ucur, t->interval);      /* it_interval@0 */
    return 0ull;
}

/* 到点推进：counter++；周期则把 value 往后推 interval，否则停表。 */
void lx_timerfd_check(void)
{
    u64 now = port_cycles();
    u32 i;

    if (g_tfd == (LxTimerFd *)0) {
        return;
    }
    for (i = 0u; i < g_tfd_cap; i++) {
        LxTimerFd *t = &g_tfd[i];

        if (t->used == 0u || t->value == 0u || now < t->value) {
            continue;
        }
        if (t->interval == 0u) {
            t->value = 0u;
        } else {
            do {
                t->value += t->interval;
            } while (t->value <= now);
        }
        t->counter++;
        wake_all_();
    }
}

u64 lx_timerfd_read(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    LxTimerFd *t;
    u64 v;

    if (fe == (LxFile *)0 || fe->kind != LX_FD_TIMERFD) {
        return n_((u64)LX_EBADF);
    }
    t = tfd_at_(fe->pos);
    if (t == (LxTimerFd *)0 || lx_user_ok(ubuf, 8ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    lx_timerfd_check();
    if (t->counter == 0u) {
        if ((fe->oflags & LX_O_NONBLOCK) != 0u) {
            return n_((u64)LX_EAGAIN);
        }
        /* 阻塞等到到期：复用等待队列，到点后由 lx_timerfd_check 的 wake_all 唤醒 */
        for (;;) {
            lx_proc_set_wait(LX_WAIT_PIPE);
            lx_proc_block();
            lx_timerfd_check();
            if (t->counter != 0u) { break; }
        }
    }
    v = t->counter;
    t->counter = 0u;
    *(u64 *)ubuf = v;
    return 8ull;
}

/* ---- M22：signalfd（把信号转成可 read 的 128B siginfo）----
 * 全局对象表（同 epoll/timerfd）。每个对象带 sigmask；lx_sig_send_one
 * 投递前先问 lx_signalfd_queue：若信号被某 signalfd 监控则入它的队列，
 * 不再走 handler/默认动作。队列是内核堆上的定长记录（sig/code/pid）。 */
#define LX_SFD_SENTINEL 0xfffffff8ull
#define SFD_QMAX 64u

typedef struct {
    u32 sig;
    u32 code;
    u32 pid;
} LxSfdRec;

typedef struct {
    u64 mask;                   /* 1ull << (sig-1) */
    LxSfdRec q[SFD_QMAX];
    u32 head;
    u32 tail;
    u32 count;
    u32 used;
} LxSignalFd;

static LxSignalFd *g_sfd;
static u32 g_sfd_cap;

static LxSignalFd *sfd_at_(u64 id)
{
    if (g_sfd == (LxSignalFd *)0 || id >= g_sfd_cap || g_sfd[id].used == 0u) {
        return (LxSignalFd *)0;
    }
    return &g_sfd[id];
}

/* close(fd) 时释放槽位；由前置声明供 lx_proc_close 调用。 */
static void sfd_release_(u64 id)
{
    LxSignalFd *s = sfd_at_(id);

    if (s != (LxSignalFd *)0) {
        s->used = 0u;
    }
}

static int sfd_grow_(void)
{
    u32 ncap = (g_sfd_cap == 0u) ? 4u : g_sfd_cap * 2u;
    LxSignalFd *ne;

    if (ncap > 64u) {
        return -1;
    }
    ne = (LxSignalFd *)port_kcalloc((u64)ncap, (u64)sizeof(LxSignalFd));
    if (ne == (LxSignalFd *)0) {
        return -1;
    }
    if (g_sfd != (LxSignalFd *)0) {
        u32 i;

        for (i = 0u; i < g_sfd_cap; i++) { ne[i] = g_sfd[i]; }
        port_kfree((void *)g_sfd);
    }
    g_sfd = ne;
    g_sfd_cap = ncap;
    return 0;
}

/* sig.c 调用：任意一个 signalfd 监控该信号就入队并吞掉。返回 1=已消费。 */
int lx_signalfd_queue(u64 sig, u64 code, u64 pid)
{
    u32 i;

    if (sig == 0ull || sig > 64ull || g_sfd == (LxSignalFd *)0) {
        return 0;
    }
    for (i = 0u; i < g_sfd_cap; i++) {
        LxSignalFd *s = &g_sfd[i];

        if (s->used == 0u || (s->mask & (1ull << (sig - 1ull))) == 0ull) {
            continue;
        }
        if (s->count < SFD_QMAX) {
            LxSfdRec *r = &s->q[s->tail];

            r->sig = (u32)sig;
            r->code = (u32)code;
            r->pid = (u32)pid;
            s->tail = (s->tail + 1u) % SFD_QMAX;
            s->count++;
        }
        wake_all_();
        return 1;                           /* 该信号从此不再走 handler */
    }
    return 0;
}

u64 lx_signalfd4(u64 fd, u64 umask, u64 masklen, u64 flags)
{
    LxProc *p = cur_();
    LxFile *fe;
    LxSignalFd *s;

    if (p == (LxProc *)0 || (flags & ~((u64)0x80000ull | 0x800ull)) != 0ull) {
        return n_((u64)LX_EINVAL);
    }
    /* fd=-1 新建：兼容符号扩展(0xffffffffffffffff)和 32 位零扩展
     * (0x00000000ffffffff)两种——glibc 的 signalfd(-1,...) 在此 ABI 下
     * 观察到零扩展形式（与 AT_FDCWD 同构）。 */
    {
        u32 fd32 = (u32)fd;
        int is_new = ((i64)fd < 0) ||
                     (fd32 == 0xffffffffu && (fd >> 32) == 0ull);

        if (!is_new) {                     /* 修改已有 signalfd 的 mask */
            fe = lx_fdtab_get(p->fd, fd);
            if (fe == (LxFile *)0 || fe->kind != LX_FD_SIGNALFD) {
                return n_((u64)LX_EBADF);
            }
            s = sfd_at_(fe->pos);
            if (s == (LxSignalFd *)0) { return n_((u64)LX_EBADF); }
        } else {                           /* 新建 */
            u32 i;

        for (i = 0u; i < g_sfd_cap; i++) {
            if (g_sfd[i].used == 0u) { break; }
        }
        if (i >= g_sfd_cap) {
            if (sfd_grow_() != 0) { return n_((u64)LX_ENOMEM); }
            for (i = 0u; i < g_sfd_cap; i++) {
                if (g_sfd[i].used == 0u) { break; }
            }
        }
        if (i >= g_sfd_cap) { return n_((u64)LX_ENOMEM); }
        fd = lx_fdtab_alloc(p->fd);
        if (fd == LX_FD_NONE) { return n_((u64)LX_EMFILE); }
        s = &g_sfd[i];
        s->used = 1u;
        s->head = 0u;
        s->tail = 0u;
        s->count = 0u;
        s->mask = 0ull;
        fe = &p->fd->ent[fd];              /* 直接写槽（alloc 后 node=NONE） */
        fe->node = LX_SFD_SENTINEL;
        fe->pos = (u64)i;
        fe->is_dir = 0u;
        fe->kind = LX_FD_SIGNALFD;
        fe->flags = (flags & 0x80000ull) ? LX_FD_CLOEXEC : 0u;
        fe->oflags = (flags & 0x800ull) ? LX_O_NONBLOCK : 0u;
        }
    }
    if (masklen != 0ull) {
        if (lx_user_ok(umask, masklen, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        s->mask = *(const u64 *)umask;       /* 8 字节 sigset */
    }
    return fd;
}

/* 生成 128B signalfd_siginfo（只填前 16 字节，其余清零）。 */
static void sfi_(u8 *b, u32 sig, u32 code, u32 pid)
{
    u32 i;

    for (i = 0u; i < 128u; i++) { b[i] = 0u; }
    *(u32 *)(b + 0u) = sig;
    *(u32 *)(b + 4u) = 0u;                  /* ssi_errno */
    *(i32 *)(b + 8u) = (i32)code;
    *(u32 *)(b + 12u) = pid;
}

u64 lx_signalfd_read(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    LxSignalFd *s;
    LxSfdRec r;

    if (fe == (LxFile *)0 || fe->kind != LX_FD_SIGNALFD) {
        return n_((u64)LX_EBADF);
    }
    s = sfd_at_(fe->pos);
    if (s == (LxSignalFd *)0 || len < 128ull ||
        lx_user_ok(ubuf, 128ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    while (s->count == 0u) {
        if ((fe->oflags & LX_O_NONBLOCK) != 0u) {
            return n_((u64)LX_EAGAIN);
        }
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
    }
    r = s->q[s->head];
    s->head = (s->head + 1u) % SFD_QMAX;
    s->count--;
    sfi_((u8 *)ubuf, r.sig, r.code, r.pid);
    return 128ull;
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
    /* 不能用裸 port_as_user_range_ok：musl 的大缓冲（cat 用 64KB）里未触碰的
     * 页只有 VMA 承诺、尚无 PTE，必须先补页再查，否则误报 EFAULT。 */
    if (lx_user_ok(ubuf, len, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    if (fe->kind == LX_FD_PIPE_R) {
        return pipe_read_(p, fe, ubuf, len);
    }
    if (fe->kind == LX_FD_EVENTFD) {
        return lx_proc_eventfd_read(fd, ubuf, len);
    }
    if (fe->kind == LX_FD_TIMERFD) {
        return lx_timerfd_read(fd, ubuf, len);
    }
    if (fe->kind == LX_FD_SIGNALFD) {
        return lx_signalfd_read(fd, ubuf, len);
    }
    if (fe->kind == LX_FD_SOCK) {
        return lx_sock_recv(fd, ubuf, len, 0ull);
    }
    if (fe->kind == LX_FD_UNIX) {
        return lx_unix_recvfrom(fd, ubuf, len, 0ull, 0ull);
    }
    if (fe->kind == LX_FD_PTY) {
        return lx_pty_read(fe->node, ubuf, len, fe->pos == 0ull ? 1u : 0u);
    }
    /* 按 node 判“是不是控制台”，不能看 fd 号：`wc -l < /tmp/f` 里 0 号 fd 已是
     * VFS 文件，以前照样落进 tty 分支，于是永远等键盘 —— `< file` 这条重定向
     * 路径整体失效，读 stdin 的 applet（mkpasswd 等）直接挂住。只有 0 号 fd
     * 真的是控制台时才阻塞读键盘（M12 语义），其余交给文件系统。 */
    if (fe->node == LX_FD_CONSOLE) {
        if (fd == 0ull && lx_tty_enabled() != 0u) {
            return lx_tty_read(ubuf, len, lx_proc_yield);
        }
        return 0ull;                       /* 1/2 号或没开行规程：EOF */
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
    if (f->kind == LX_FD_EVENTFD) {
        return (g_efd != (LxEfd *)0 && f->pos < g_efd_cap &&
                g_efd[f->pos].counter != 0ull) ? (u16)LX_POLLIN : (u16)0;
    }
    if (f->kind == LX_FD_TIMERFD) {
        lx_timerfd_check();
        return (tfd_at_(f->pos) != (LxTimerFd *)0 &&
                tfd_at_(f->pos)->counter != 0u) ? (u16)LX_POLLIN : (u16)0;
    }
    if (f->kind == LX_FD_SIGNALFD) {
        LxSignalFd *s = sfd_at_(f->pos);
        return (s != (LxSignalFd *)0 && s->count != 0u) ? (u16)LX_POLLIN : (u16)0;
    }
    if (f->kind == LX_FD_SOCK) {
        u16 rev = lx_sock_poll_one(f->node);

        return (u16)(rev & ((u16)LX_POLLIN | (u16)LX_POLLOUT |
                            (u16)LX_POLLERR | (u16)LX_POLLHUP));
    }
    if (f->kind == LX_FD_UNIX) {
        u16 rev = lx_unix_poll(f->node);

        return (u16)(rev & ((u16)LX_POLLIN | (u16)LX_POLLOUT |
                            (u16)LX_POLLERR | (u16)LX_POLLHUP));
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
    /* G2：设备（evdev 有事件才报 POLLIN；fb0 恒可写）。放在通用
     * "普通文件 / console：不阻塞" 之前，否则空 event0 会误报就绪。 */
    if (f->kind == LX_FD_DEV && f->dev != 0u) {
        LxOsFile *o = lx_dev_of(f->node);

        return (o != (LxOsFile *)0) ? lx_dev_poll(o, (u16)0xffffu)
                                  : (u16)LX_POLLNVAL;
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
        u32 has_sock = 0u;             /* 这轮轮询里有没有 socket fd（M24） */

        if (C6OS_NET) {
            lx_net_pump();             /* 轮询式网卡：收 ARP 应答/DNS 响应 */
        }
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
                if (qe != (const LxFile *)0 && qe->kind == LX_FD_SOCK) {
                    has_sock = 1u;
                }
            }
            /* Linux poll 只报告调用方请求的事件（ERR/HUP/NVAL 永远报）。
             * 以前直接把 poll_events_ 的全部位写进 revents：TCP socket 的
             * POLLOUT 一直置位，ssl_client 只等 POLLIN 却立刻返回，随后
             * read() 拿到 0 字节就当网络 EOF 退出，wget 报 "Connection refused"。 */
            {
                u16 want = *(const u16 *)(e + 4ull);

                rev = (u16)(poll_events_(p, fd) &
                            (want | (u16)LX_POLLERR | (u16)LX_POLLHUP |
                             (u16)LX_POLLNVAL));
            }
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
        /* 轮询式串口 + 到点的闹钟/定时器：每一轮都是一次机会（M60 合并成
         * 一个节拍，自旋等待期间 SIGALRM 也能响、^C 也能进来）。 */
        (void)lx_proc_wait_tick();
        /* 有信号待投递就先回用户态让它跑一次：handler 带 SA_RESTART 时
         * lx_proc_wait_intr 会说"继续等"，但信号只在系统调用出口投递，
         * 死等下去 handler 永远轮不到（见 lx_proc_sig_pending 的说明）。 */
        if (lx_proc_wait_intr() != 0 || lx_proc_sig_pending() != 0) {
            return n_((u64)LX_EINTR);  /* 被信号打断：先回用户态投递信号 */
        }
        /* 终端：输入来自串口，没有哪个进程会来唤醒我们，就地继续泵。
         * 这里**不能**用 lx_proc_block()：它把进程标成 BLOCK 再 resched，
         * 没人可切时状态就悬在 BLOCK 上，而且再也等不到唤醒。
         * 纯管道才值得真阻塞——但也得先确认还有别人能往管道里写。 */
        if (tty == 0u) {
            int has_tfd = 0;
            u64 q;

            /* 单进程：若有 timerfd 且带 deadline，自旋到点（没有别的进程
             * 能 wake 我们，timerfd 到期由 poll_events_ 每轮驱动）。否则
             * 立刻返回，避免协作调度里死等（L2–L6 依赖 stdin 立即 EOF）。 */
            for (q = 0ull; q < nfds; q++) {
                i32 qfd = *(i32 *)(ufds + q * 8ull);
                const LxFile *qe = lx_fdtab_get(p->fd, (u64)qfd);

                if (qe != (LxFile *)0 && qe->kind == LX_FD_TIMERFD) {
                    has_tfd = 1;
                }
            }
            /* 有 socket 就自旋：网卡是轮询式的，**只有本进程能泵它**。
             * 这里以前会掉进下面的 lx_proc_block()，而 block 之后没有任何
             * 人再来调 lx_net_pump()——DNS 应答永远进不了 UDP 队列，
             * nslookup 就死等（busybox 的 nslookup 会 fork 一个子进程发查询，
             * 所以 others_live_ 为真，反而更容易掉进这个坑）。
             * 无论单进程还是多进程，socket 一律 continue 回循环顶部泵网卡。 */
            if (has_sock) {
                continue;
            }
            /* 单进程：没有任何别的进程能来唤醒我们，所以不能 block。
             * 有 timerfd 且带 deadline 就自旋到点（poll_events_ 每轮驱动），
             * 都没有才立刻返回（L2–L6 依赖 stdin 立即 EOF）。 */
            if (!others_live_(g_cur)) {
                if (has_tfd && have_deadline) {
                    continue;
                }
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

/* M16-1a：select/pselect6。fd_set 是"每个 long 一位图"的用户缓冲
 * （x86-64 上沿用了历史 ABI：FD_SETSIZE=1024，即 16 个 64 位 long）。
 * 复用 poll_events_ 的就绪判定 + 和 poll 一样的阻塞/超时骨架，避免两套
 * 事件逻辑分叉。timeout=NULL 表示无限等；struct timeval={sec,usec}。 */
static int fdset_test_(u64 set, i32 fd)
{
    if (set == 0ull || fd < 0 || fd >= 1024) {
        return 0;
    }
    if (lx_user_ok(set + (u64)(fd >> 6u) * 8ull, 8ull, C6OS_PROT_R) == 0) {
        return 0;
    }
    return ((*(const u64 *)(set + (u64)(fd >> 6u) * 8ull) >> (fd & 63)) & 1ull) != 0ull;
}

i64 lx_proc_select(i32 nfds, u64 rset, u64 wset, u64 eset, i64 timeout)
{
    LxProc *p = cur_();
    u64 deadline = 0ull;
    int have_deadline = 0;
    i32 i;

    if (p == (LxProc *)0 || nfds < 0 || nfds > 1024) {
        return n_((u64)LX_EINVAL);
    }
    if (timeout > 0) {                     /* 已经是毫秒（pselect6 转换后传进来） */
        deadline = port_cycles() + (u64)timeout * 1000000ull;
        have_deadline = 1;
    } else if (timeout < 0) {              /* NULL = 无限等 */
        have_deadline = 0;
    }
    /* 先把除结果集之外的三个集清零的语义交给调用方：select 会就地改写入集。
     * 我们在每轮开头把 wset/eset 的结果位单独收集，最后一次性写回，避免
     * 边判断边写导致"上一轮的就绪位残留"。 */
    for (;;) {
        i64 cnt = 0;
        u64 ready_r[16] = {0};
        u64 ready_w[16] = {0};
        u64 ready_e[16] = {0};
        u32 tty = 0u;
        u32 has_sock = 0u;

        if (C6OS_NET) {
            lx_net_pump();             /* M24：收 ARP 应答/DNS 响应 */
        }
        for (i = 0; i < nfds; i++) {
            u16 rev = poll_events_(p, i);
            const LxFile *f = lx_fdtab_get(p->fd, (u64)i);

            if (f != (LxFile *)0 && f->node == LX_FD_CONSOLE && lx_tty_enabled() != 0u) {
                tty = 1u;
            }
            if (f != (LxFile *)0 && f->kind == LX_FD_SOCK) {
                has_sock = 1u;
            }
            if (fdset_test_(rset, i) && (rev & (LX_POLLIN | LX_POLLHUP)) != 0u) {
                ready_r[i >> 6u] |= 1ull << (i & 63);
                cnt++;
            }
            if (fdset_test_(wset, i) && (rev & (LX_POLLOUT | LX_POLLERR)) != 0u) {
                ready_w[i >> 6u] |= 1ull << (i & 63);
                cnt++;
            }
            if (fdset_test_(eset, i) && (rev & (LX_POLLERR | LX_POLLHUP)) != 0u) {
                ready_e[i >> 6u] |= 1ull << (i & 63);
                cnt++;
            }
        }
        if (cnt > 0 || timeout == 0 || (have_deadline && port_cycles() >= deadline)) {
            u64 k;

            for (k = 0ull; k < 16ull; k++) {
                if (rset != 0ull && lx_user_ok(rset + k * 8ull, 8ull, C6OS_PROT_W) != 0) {
                    *(u64 *)(rset + k * 8ull) = ready_r[k];
                }
                if (wset != 0ull && lx_user_ok(wset + k * 8ull, 8ull, C6OS_PROT_W) != 0) {
                    *(u64 *)(wset + k * 8ull) = ready_w[k];
                }
                if (eset != 0ull && lx_user_ok(eset + k * 8ull, 8ull, C6OS_PROT_W) != 0) {
                    *(u64 *)(eset + k * 8ull) = ready_e[k];
                }
            }
            return cnt;
        }
        (void)lx_proc_wait_tick();     /* 串口 + 到点的闹钟/定时器（M60） */
        /* 有信号待投递就先回用户态让它跑一次：handler 带 SA_RESTART 时
         * lx_proc_wait_intr 会说"继续等"，但信号只在系统调用出口投递，
         * 死等下去 handler 永远轮不到（见 lx_proc_sig_pending 的说明）。 */
        if (lx_proc_wait_intr() != 0 || lx_proc_sig_pending() != 0) {
            u64 k;

            /* 被信号打断：不写就绪位，只把结果集清零（调用方会复用它们）。 */
            for (k = 0ull; k < 16ull; k++) {
                if (rset != 0ull && lx_user_ok(rset + k * 8ull, 8ull, C6OS_PROT_W) != 0) {
                    *(u64 *)(rset + k * 8ull) = 0ull;
                }
                if (wset != 0ull && lx_user_ok(wset + k * 8ull, 8ull, C6OS_PROT_W) != 0) {
                    *(u64 *)(wset + k * 8ull) = 0ull;
                }
                if (eset != 0ull && lx_user_ok(eset + k * 8ull, 8ull, C6OS_PROT_W) != 0) {
                    *(u64 *)(eset + k * 8ull) = 0ull;
                }
            }
            return n_((u64)LX_EINTR);
        }
        if (tty == 0u) {
            /* 和 poll 同理：socket 只有本进程能泵，绝不能 block。 */
            if (has_sock) {
                continue;
            }
            if (!others_live_(g_cur)) {
                return 0;
            }
            lx_proc_set_wait(LX_WAIT_PIPE);
            lx_proc_block();
            if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
                return n_((u64)LX_EINTR);
            }
        }
    }
}

/* sendfile 用：从**内核**缓冲写到一个 fd，跳过用户范围检查（普通 write
 * 的 ubuf 在用户空间，要走 port_as_user_range_ok；sendfile 的中转页在
 * 内核堆，不能过那道检查）。目前只需支持 pipe/console/文件三种目标。 */
static u64 write_kbuf_(u64 fd, const u8 *src, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);

    if (fe == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }
    if (fe->kind == LX_FD_PIPE_W) {
        return pipe_write_(p, fe, (u64)src, len);
    }
    if (fe->kind == LX_FD_PTY) {
        return lx_pty_write_k(fe->node, (u64)src, len, fe->pos == 0ull ? 1u : 0u);
    }
    if (fe->node == LX_FD_CONSOLE) {
        return lx_console_put_k((u64)src, len);
    }
    return lx_fs_write_k(fd, src, len);
}

u64 lx_proc_write(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();
    LxFile *fe;

    fe = lx_fdtab_get(p != (LxProc *)0 ? p->fd : (LxFdTab *)0, fd);
    if (fe == (LxFile *)0) {
        return n_((u64)LX_EBADF);
    }    if (fe->kind == LX_FD_PIPE_W) {
        if (port_as_user_range_ok(p->as, ubuf, len, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        return pipe_write_(p, fe, ubuf, len);
    }
    if (fe->kind == LX_FD_EVENTFD) {
        return lx_proc_eventfd_write(fd, ubuf, len);
    }
    if (fe->kind == LX_FD_SOCK) {
        return lx_sock_send(fd, ubuf, len, 0ull);
    }
    if (fe->kind == LX_FD_UNIX) {
        return lx_unix_sendto(fd, ubuf, len, 0ull, 0ull);
    }
    if (fe->kind == LX_FD_PTY) {
        if (port_as_user_range_ok(p->as, ubuf, len, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        return lx_pty_write(fe->node, ubuf, len, fe->pos == 0ull ? 1u : 0u);
    }
    /* 按 node/kind 判目标，而不是 fd 号：dup 出来的高位 fd 也可能是终端，
     * 反过来 fd 1/2 被 `> file` 重定向后是 VFS 文件。以前这里对非控制台
     * 的 fd 直接调 sys_write，而 sys_write 只看 fd 号（1/2 就写串口），于是
     * `cmd > /data/o` 的输出全打到控制台、目标文件永远是空的。 */
    if (fe->node == LX_FD_CONSOLE) {
        return lx_console_put(ubuf, len);
    }
    return lx_fs_write(fd, ubuf, len);          /* VFS 文件 / /dev/null 等 */
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
            port_qemu_exit();
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
/* futex 键：私有映射按"地址空间 + 用户地址"区分，不同进程的同名地址不能
 * 互相唤醒；同一进程的线程共享 AS，pthread mutex/cond 仍然命中。
 * 共享映射（MAP_SHARED）本该按物理页做键，当前先退化为 AS+VA —— 等
 * G1 的 /dev/shm 用例需要跨进程唤醒时再接 port_as_translate。 */
static u64 futex_key_(LxProc *p, u64 uaddr)
{
    return (p->as << 32) | (uaddr & 0xffffffffull);
}

u64 lx_futex(u64 uaddr, u32 op, u32 val, u64 utime)
{
    u32 cmd = op & 0x7fu;
    LxProc *p = cur_();
    u64 key;

    (void)utime;
    if (p == (LxProc *)0) {
        return 0ull;
    }
    /* FUTEX_WAIT_BITSET(9) / FUTEX_WAKE_BITSET(10) 在 glibc 的 condvar
     * 实现里会出现；bitset 先按全集处理，val3 不参与匹配。 */
    if (cmd == 9u) {
        cmd = 0u;
    } else if (cmd == 10u) {
        cmd = 1u;
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
        key = futex_key_(p, uaddr);
        p->futex_key = key;
        lx_proc_set_wait(LX_WAIT_FUTEX);
        lx_proc_block();
        p->futex_key = 0ull;
        return 0ull;
    }
    if (cmd == 1u) {                       /* FUTEX_WAKE */
        u64 n = 0ull;
        u32 i;

        key = futex_key_(p, uaddr);
        for (i = 0u; i < g_proc_cap && n < (u64)val; i++) {
            LxProc *q = g_proc[i].p;

            if (q != (LxProc *)0 && q->state == LX_P_BLOCK &&
                q->futex_key == key) {
                q->futex_key = 0ull;
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
