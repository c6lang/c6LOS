/* linux/proc.c —— L5 进程模型。
 *
 * 协作调度：ring3 里 IF=0，没有时钟中断，所以进程只在系统调用边界上切换。
 * fork/exec/wait/pipe 这条路径够用：父进程 wait4 或读空管道时主动让出，
 * 被唤醒后从让出点继续。抢占式调度要等 IRQ_USER，届时只换触发源。
 */

#include <c6os/linux_abi.h>
#include <c6os/linux_proc.h>
#include <c6os/port.h>

#define LX_MAX_PIPE   8u
#define LX_PIPE_SZ    4096ull
#define LX_ARG_MAX    16u
#define LX_ARG_BYTES  1024ull

typedef struct LxPipe {
    u32 used;
    u32 readers;
    u32 writers;
    u64 head;
    u64 tail;
    u8  buf[LX_PIPE_SZ];
} LxPipe;

static LxProc  g_proc[LX_MAX_PROC];
static LxPipe  g_pipe[LX_MAX_PIPE];
static u8      g_kstack[LX_MAX_PROC][LX_KSTACK_SIZE];
static int     g_cur = -1;
static u64     g_pid_next = 1ull;
static int     g_resched;

/* 汇编侧（port/x86_64/user_entry.S） */
extern void lx_ctx_switch(u64 *save_sp, u64 new_sp);
extern void x86_resume_user(void);
extern u64  x86_resume_frame;

/* 外部依赖 */
i64 lx_user_cstr(u64 upath, char *dst, u64 max);              /* linux/sys.c */
i64 lx_fs_lookup(const char *path);                           /* linux/fs.c */


static LxProc *cur_(void)
{
    if (g_cur < 0) {
        return (LxProc *)0;
    }
    return &g_proc[g_cur];
}

LxProc *lx_cur_proc(void)
{
    return cur_();
}

static void pipe_drop_(LxFile *fe);

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

void lx_proc_attach(u64 as, u64 brk)
{
    LxProc *p;

    if (g_cur < 0) {
        g_cur = 0;
        p = &g_proc[0];
        mem_zero_((u8 *)p, (u64)sizeof(*p));
        p->pid = g_pid_next;
        g_pid_next++;
        p->ppid = 0u;
    }
    p = &g_proc[g_cur];
    lx_fs_set_fdtab(p->fd);                /* fd 表从此跟着进程走 */
    p->state = LX_P_RUN;
    p->as = as;
    p->brk = brk;
    p->mmap_top = C6OS_LINUX_MMAP_BASE;
    p->kstack_top = (u64)(&g_kstack[g_cur][0]) + LX_KSTACK_SIZE;
    port_user_set_kstack(p->kstack_top);
}

static int slot_alloc_(void)
{
    int i;

    for (i = 0; i < (int)LX_MAX_PROC; i++) {
        if (g_proc[i].state == LX_P_FREE) {
            return i;
        }
    }
    return -1;
}

static int pick_next_(int from)
{
    int i;

    for (i = 1; i <= (int)LX_MAX_PROC; i++) {
        int k = (from + i) % (int)LX_MAX_PROC;

        if (g_proc[k].state == LX_P_READY) {
            return k;
        }
    }
    return -1;
}

/* 切到 next：换页表 / 内核栈 / fd 表 / brk 与 mmap 状态。
 * 回来时（本进程被再次调度）从 lx_ctx_switch 之后继续。 */
static void switch_to_(int next)
{
    LxProc *p = &g_proc[next];
    LxProc *c = cur_();
    u64 prev_sp;

    g_cur = next;
    p->state = LX_P_RUN;
    lx_set_as(p->as);
    lx_brk_set(p->brk);
    lx_mmap_top_set(p->mmap_top);
    lx_fs_set_fdtab(p->fd);
    port_user_set_kstack(p->kstack_top);
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

/* 让出 CPU 一次；没有别的可运行进程时立刻回来。 */
static void resched_(void)
{
    LxProc *c = cur_();
    int next;

    if (c == (LxProc *)0) {
        return;
    }
    if (c->state == LX_P_RUN) {
        c->state = LX_P_READY;
    }
    next = pick_next_(g_cur);
    if (next < 0) {
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

void lx_proc_block(void)
{
    LxProc *c = cur_();

    if (c == (LxProc *)0) {
        return;
    }
    c->state = LX_P_BLOCK;
    resched_();
    /* 被唤醒后从这里继续：调用点必须重新检查条件 */
}

static void wake_(u32 pid)
{
    int i;

    for (i = 0; i < (int)LX_MAX_PROC; i++) {
        if (g_proc[i].pid == pid && g_proc[i].state == LX_P_BLOCK) {
            g_proc[i].state = LX_P_READY;
        }
    }
}

void lx_proc_exit(u64 code)
{
    LxProc *c = cur_();
    int next;

    if (c == (LxProc *)0) {
        port_puts("[lx] exit 但当前没有进程\n");
        port_panic_halt();
    }
    {
        u64 i;

        for (i = 0ull; i < (u64)C6OS_LINUX_MAX_FD; i++) {
            if (c->fd[i].node != LX_FD_NONE && c->fd[i].kind != LX_FD_FILE) {
                pipe_drop_(&c->fd[i]);
                c->fd[i].node = LX_FD_NONE;
            }
        }
    }
    c->code = (i64)code;
    c->state = LX_P_ZOMBIE;
    wake_(c->ppid);
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
    ch = &g_proc[slot];
    mem_zero_((u8 *)ch, (u64)sizeof(*ch));
    ch->pid = (u32)g_pid_next;
    g_pid_next++;
    ch->ppid = par->pid;
    ch->as = as;
    ch->brk = par->brk;
    ch->mmap_top = par->mmap_top;
    ch->kstack_top = (u64)(&g_kstack[slot][0]) + LX_KSTACK_SIZE;
    ch->f.r = *r;                         /* 继承父进程的用户寄存器 */
    ch->f.r.rax = 0ull;                   /* fork 在子进程里返回 0 */
    ch->f.user_rsp = pf->user_rsp;
    ch->state = LX_P_READY;
    {
        u64 i;

        for (i = 0ull; i < (u64)C6OS_LINUX_MAX_FD; i++) {
            ch->fd[i] = par->fd[i];
            if (par->fd[i].node == LX_FD_NONE ||
                par->fd[i].node >= (u64)LX_MAX_PIPE) {
                continue;
            }
            if (par->fd[i].kind == LX_FD_PIPE_R) {
                g_pipe[par->fd[i].node].readers++;
            } else if (par->fd[i].kind == LX_FD_PIPE_W) {
                g_pipe[par->fd[i].node].writers++;
            }
        }
    }
    setup_trampoline_(ch);
    lx_proc_yield();                      /* 给子进程一次机会 */
    return (u64)ch->pid;
}

/* ---- wait4 --------------------------------------------------------------- */

u64 lx_proc_wait4(i64 pid, u64 ustatus, u64 options, u64 urusage)
{
    LxProc *par = cur_();

    (void)options;
    (void)urusage;
    if (par == (LxProc *)0) {
        return (u64)0 - LX_ECHILD;
    }
    for (;;) {
        int i;
        int has_child = 0;

        for (i = 0; i < (int)LX_MAX_PROC; i++) {
            LxProc *c = &g_proc[i];

            if (c->state == LX_P_FREE || c->ppid != par->pid) {
                continue;
            }
            if (pid > 0 && (i64)c->pid != pid) {
                continue;
            }
            has_child = 1;
            if (c->state == LX_P_ZOMBIE) {
                u32 cpid = c->pid;
                u64 status = ((u64)c->code & 0xffull) << 8;

                if (ustatus != 0ull) {
                    if (port_as_user_range_ok(par->as, ustatus, 4ull,
                                              C6OS_PROT_W) == 0) {
                        return (u64)0 - LX_EFAULT;
                    }
                    *(u32 *)ustatus = (u32)status;
                }
                c->state = LX_P_FREE;
                return (u64)cpid;
            }
        }
        if (has_child == 0) {
            return (u64)0 - LX_ECHILD;
        }
        lx_proc_block();
    }
}

/* ---- execve -------------------------------------------------------------- */

/* 从用户态读 argv：指针数组 + 字符串一起搬进内核缓冲。 */
static u32 read_argv_(u64 uargv, char **argv, char *buf, u64 bufbytes)
{
    u64 i;
    u64 off = 0ull;

    if (uargv == 0ull) {
        return 0u;
    }
    for (i = 0; i < (u64)LX_ARG_MAX; i++) {
        u64 up;
        i64 n;

        if (port_as_user_range_ok(cur_()->as, uargv + i * 8ull, 8ull,
                                  C6OS_PROT_R) == 0) {
            break;
        }
        up = *(u64 *)(uargv + i * 8ull);
        if (up == 0ull) {
            break;
        }
        if (off >= bufbytes) {
            break;
        }
        n = lx_user_cstr(up, buf + off, bufbytes - off);
        if ((i64)n < 0) {
            break;
        }
        argv[i] = buf + off;
        off += (u64)n + 1ull;
    }
    return (u32)i;
}

u64 lx_proc_execve(u64 upath, u64 uargv, u64 uenvp, LxRegs *r)
{
    char path[128];
    char buf[LX_ARG_BYTES];
    char *argv[LX_ARG_MAX + 1u];
    u64 as;
    u64 entry = 0ull;
    u64 rsp = 0ull;
    u64 brk = 0ull;
    i64 n;
    i64 idx;
    u32 argc;

    (void)uenvp;
    n = lx_user_cstr(upath, path, (u64)sizeof(path));
    if (n <= 0) {
        return (u64)0 - LX_EFAULT;
    }
    idx = lx_fs_lookup(path);
    if (idx < 0) {
        return (u64)0 - LX_ENOENT;
    }
    argc = read_argv_(uargv, argv, buf, (u64)sizeof(buf));
    if (argc == 0u) {
        argv[0] = path;
        argc = 1u;
    }
    argv[argc] = (char *)0;
    as = port_as_create();
    if (as == 0ull) {
        return (u64)0 - LX_ENOMEM;
    }
    if (lx_exec_build(as, lx_fs_node_data_pa((u32)idx),
                      lx_fs_node_size((u32)idx), path,
                      (const char *const *)argv, argc, &entry, &rsp,
                      &brk) != 0) {
        port_as_destroy(as);
        return (u64)0 - LX_ENOEXEC;
    }
    /* 换掉整个地址空间；fd 表按 Linux 语义保留（CLOEXEC 还没实现）。 */
    cur_()->as = as;
    cur_()->brk = brk;                    /* 新映像的堆起点，不能留旧程序的 */
    cur_()->mmap_top = C6OS_LINUX_MMAP_BASE;
    lx_set_as(as);
    lx_brk_set(brk);
    lx_mmap_top_set(cur_()->mmap_top);
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
}

/* ---- fd 与管道 ----------------------------------------------------------- */

static u64 n_(u64 v)
{
    return (u64)0 - v;
}

static void wake_all_(void)
{
    int i;

    for (i = 0; i < (int)LX_MAX_PROC; i++) {
        if (g_proc[i].state == LX_P_BLOCK) {
            g_proc[i].state = LX_P_READY;
        }
    }
}

static LxPipe *pipe_new_(int *o_idx)
{
    int i;

    for (i = 0; i < (int)LX_MAX_PIPE; i++) {
        if (g_pipe[i].used == 0u) {
            LxPipe *pp = &g_pipe[i];

            pp->used = 1u;
            pp->readers = 0u;
            pp->writers = 0u;
            pp->head = 0ull;
            pp->tail = 0ull;
            *o_idx = i;
            return pp;
        }
    }
    return (LxPipe *)0;
}

static void pipe_drop_(LxFile *fe)
{
    if (fe->node >= (u64)LX_MAX_PIPE) {
        return;
    }
    if (fe->kind == LX_FD_PIPE_R && g_pipe[fe->node].readers > 0u) {
        g_pipe[fe->node].readers--;
    } else if (fe->kind == LX_FD_PIPE_W && g_pipe[fe->node].writers > 0u) {
        g_pipe[fe->node].writers--;
    }
    if (g_pipe[fe->node].readers == 0u && g_pipe[fe->node].writers == 0u) {
        g_pipe[fe->node].used = 0u;
    }
}

u64 lx_proc_pipe2(u64 ufds, u64 flags)
{
    LxProc *p = cur_();
    LxPipe *pp;
    int pi = -1;
    int rfd = -1;
    int wfd = -1;
    int i;
    u32 pair[2];

    (void)flags;
    if (p == (LxProc *)0) {
        return n_((u64)LX_EINVAL);
    }
    if (port_as_user_range_ok(p->as, ufds, 8ull, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    for (i = 3; i < (int)C6OS_LINUX_MAX_FD; i++) {
        if (p->fd[i].node == LX_FD_NONE) {
            if (rfd < 0) {
                rfd = i;
            } else {
                wfd = i;
                break;
            }
        }
    }
    if (wfd < 0) {
        return n_((u64)LX_EMFILE);
    }
    pp = pipe_new_(&pi);
    if (pp == (LxPipe *)0) {
        return n_((u64)LX_EMFILE);
    }
    pp->readers = 1u;
    pp->writers = 1u;
    p->fd[rfd].node = (u64)pi;
    p->fd[rfd].pos = 0ull;
    p->fd[rfd].is_dir = 0u;
    p->fd[rfd].kind = LX_FD_PIPE_R;
    p->fd[wfd].node = (u64)pi;
    p->fd[wfd].pos = 0ull;
    p->fd[wfd].is_dir = 0u;
    p->fd[wfd].kind = LX_FD_PIPE_W;
    pair[0] = (u32)rfd;
    pair[1] = (u32)wfd;
    *(u32 *)ufds = pair[0];
    *(u32 *)(ufds + 4ull) = pair[1];
    return 0ull;
}

u64 lx_proc_close(u64 fd)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0 || fd >= (u64)C6OS_LINUX_MAX_FD) {
        return n_((u64)LX_EBADF);
    }
    if (p->fd[fd].node == LX_FD_NONE) {
        return n_((u64)LX_EBADF);
    }
    if (p->fd[fd].kind != LX_FD_FILE) {
        pipe_drop_(&p->fd[fd]);
        p->fd[fd].node = LX_FD_NONE;
        wake_all_();
        return 0ull;
    }
    p->fd[fd].node = LX_FD_NONE;           /* 只读 initramfs：文件没有可回收的状态 */
    return 0ull;
}

u64 lx_proc_dup(u64 oldfd)
{
    LxProc *p = cur_();
    u64 i;

    if (p == (LxProc *)0 || oldfd >= (u64)C6OS_LINUX_MAX_FD) {
        return n_((u64)LX_EBADF);
    }
    if (p->fd[oldfd].node == LX_FD_NONE) {
        return n_((u64)LX_EBADF);
    }
    for (i = 3ull; i < (u64)C6OS_LINUX_MAX_FD; i++) {
        if (p->fd[i].node == LX_FD_NONE) {
            return lx_proc_dup2(oldfd, i);
        }
    }
    return n_((u64)LX_EMFILE);
}

u64 lx_proc_dup2(u64 oldfd, u64 newfd)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0 || oldfd >= (u64)C6OS_LINUX_MAX_FD ||
        newfd >= (u64)C6OS_LINUX_MAX_FD) {
        return n_((u64)LX_EBADF);
    }
    if (p->fd[oldfd].node == LX_FD_NONE) {
        return n_((u64)LX_EBADF);
    }
    if (oldfd == newfd) {
        return newfd;
    }
    if (p->fd[newfd].node != LX_FD_NONE && p->fd[newfd].kind != LX_FD_FILE) {
        pipe_drop_(&p->fd[newfd]);
    }
    p->fd[newfd] = p->fd[oldfd];
    if (p->fd[newfd].kind == LX_FD_PIPE_R &&
        p->fd[newfd].node < (u64)LX_MAX_PIPE) {
        g_pipe[p->fd[newfd].node].readers++;
    } else if (p->fd[newfd].kind == LX_FD_PIPE_W &&
               p->fd[newfd].node < (u64)LX_MAX_PIPE) {
        g_pipe[p->fd[newfd].node].writers++;
    }
    return newfd;
}

/* ---- 读写：控制台 / 文件 / 管道 ------------------------------------------ */

static u64 pipe_read_(LxProc *p, LxFile *fe, u64 ubuf, u64 len)
{
    LxPipe *pp;

    if (fe->node >= (u64)LX_MAX_PIPE || g_pipe[fe->node].used == 0u) {
        return n_((u64)LX_EBADF);
    }
    pp = &g_pipe[fe->node];
    for (;;) {
        u64 done = 0ull;

        while (pp->head != pp->tail && done < len) {
            u64 i = pp->head % LX_PIPE_SZ;

            *(u8 *)(ubuf + done) = pp->buf[i];
            pp->head++;
            done++;
        }
        if (done > 0ull) {
            wake_all_();
            return done;
        }
        if (pp->writers == 0u) {
            return 0ull;                   /* 写端全关：EOF */
        }
        lx_proc_block();
    }
}

static u64 pipe_write_(LxProc *p, LxFile *fe, u64 ubuf, u64 len)
{
    LxPipe *pp;

    (void)p;
    if (fe->node >= (u64)LX_MAX_PIPE || g_pipe[fe->node].used == 0u) {
        return n_((u64)LX_EBADF);
    }
    pp = &g_pipe[fe->node];
    for (;;) {
        u64 done = 0ull;

        while ((pp->tail - pp->head) < LX_PIPE_SZ && done < len) {
            pp->buf[pp->tail % LX_PIPE_SZ] = *(const u8 *)(ubuf + done);
            pp->tail++;
            done++;
        }
        if (done > 0ull) {
            wake_all_();
            return done;
        }
        if (pp->readers == 0u) {
            return n_((u64)LX_EPIPE);
        }
        lx_proc_block();
    }
}

u64 lx_proc_read(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0 || fd >= (u64)C6OS_LINUX_MAX_FD) {
        return n_((u64)LX_EBADF);
    }
    if (len == 0ull) {
        return 0ull;
    }
    if (port_as_user_range_ok(p->as, ubuf, len, C6OS_PROT_W) == 0) {
        return n_((u64)LX_EFAULT);
    }
    if (p->fd[fd].kind == LX_FD_PIPE_R) {
        return pipe_read_(p, &p->fd[fd], ubuf, len);
    }
    if (fd <= 2ull) {
        return 0ull;                       /* stdin 还没接键盘：EOF */
    }
    return lx_fs_read(fd, ubuf, len);
}

u64 lx_proc_write(u64 fd, u64 ubuf, u64 len)
{
    LxProc *p = cur_();

    if (p == (LxProc *)0 || fd >= (u64)C6OS_LINUX_MAX_FD) {
        return n_((u64)LX_EBADF);
    }
    if (p->fd[fd].kind == LX_FD_PIPE_W) {
        if (port_as_user_range_ok(p->as, ubuf, len, C6OS_PROT_R) == 0) {
            return n_((u64)LX_EFAULT);
        }
        return pipe_write_(p, &p->fd[fd], ubuf, len);
    }
    return lx_console_write(fd, ubuf, len);
}
