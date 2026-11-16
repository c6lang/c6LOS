#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

/* Linux x86-64 系统调用实现（personality 的语义层）。
 *
 * 约定：返回负 errno；未实现的统一 -ENOSYS 并打一条日志（可观测性优先，
 * 绝不让用户进程把内核卡死）。见 docs/linux-abi.md §8。 */

static u64 g_as;
static u64 g_brk;
static u64 g_brk_init;
static u64 g_mmap_top = C6OS_LINUX_MMAP_BASE;
static u32 g_seed;

static u64 neg(u64 e)
{
    return (u64)0 - e;
}

static void put_hex(u64 v)
{
    int i;

    for (i = 60; i >= 0; i -= 4) {
        port_putc("0123456789abcdef"[(v >> (u8)i) & 0xfull]);
    }
}

static void put_dec(u64 v)
{
    char buf[24];
    int i = 0;

    if (v == 0ull) {
        port_putc('0');
        return;
    }
    while (v != 0ull && i < 24) {
        buf[i] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
        i++;
    }
    while (i > 0) {
        i--;
        port_putc(buf[i]);
    }
}

void lx_proc_init(u64 as, u64 brk_start)
{
    g_as = as;
    g_brk = brk_start;
    g_brk_init = brk_start;
    g_seed = 0x9e3779b9u;
    lx_proc_attach(as, brk_start);         /* L5：登记成 0 号进程 */
}

/* ---- 进程切换要用到的状态存取（linux/proc.c 调用） ---- */
u64  lx_brk_get(void) { return g_brk; }
void lx_brk_set(u64 v) { g_brk = v; }
u64  lx_mmap_top_get(void) { return g_mmap_top; }
void lx_mmap_top_set(u64 v) { g_mmap_top = v; }
void lx_set_as(u64 as) { g_as = as; port_as_activate(as); }

/* 从用户态读一个 NUL 结尾的字符串；返回长度，<0 是 -errno。 */
i64 lx_user_cstr(u64 upath, char *dst, u64 max)
{
    u64 i;

    if (max == 0ull) {
        return (i64)0 - (i64)LX_EINVAL;
    }
    for (i = 0ull; i + 1ull < max; i++) {
        if (lx_user_ok(upath + i, 1ull, C6OS_PROT_R) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        dst[i] = *(const char *)(upath + i);
        if (dst[i] == 0) {
            return (i64)i;
        }
    }
    dst[max - 1ull] = 0;
    return (i64)(max - 1ull);
}

static u64 sys_write(u64 fd, u64 buf, u64 len);

/* 管道写之外的写路径（控制台）最终落到 sys_write。 */
u64 lx_console_write(u64 fd, u64 ubuf, u64 len)
{
    return sys_write(fd, ubuf, len);
}

u64 lx_proc_brk(void)
{
    return g_brk;
}

int lx_user_ok(u64 va, u64 len, u32 prot)
{
    if (len == 0ull) {
        return 1;
    }
    if (va + len < va) {
        return 0;
    }
    return port_as_user_range_ok(g_as, va, len, prot);
}

u64 lx_copy_from_user(void *dst, u64 src, u64 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s;
    u64 i;

    if (lx_user_ok(src, len, C6OS_PROT_R) == 0) {
        return neg(LX_EFAULT);
    }
    s = (const u8 *)src;
    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

static int user_streq(u64 uva, const char *lit)
{
    u64 i = 0;

    for (;;) {
        u8 c;

        if (lx_user_ok(uva + i, 1ull, C6OS_PROT_R) == 0) {
            return 0;
        }
        c = *(const u8 *)(uva + i);
        if (c != (u8)lit[i]) {
            return 0;
        }
        if (c == 0u) {
            return 1;
        }
        i++;
    }
}

static int brk_extend(u64 want)
{
    u64 from = (g_brk + 4095ull) & ~4095ull;
    u64 to = (want + 4095ull) & ~4095ull;

    if (to > from) {
        if (port_as_map(g_as, from, 0ull, to - from, C6OS_PROT_R | C6OS_PROT_W) != 0) {
            return -1;
        }
    }
    g_brk = want;
    return 0;
}

static u64 sys_write(u64 fd, u64 buf, u64 len)
{
    u64 i;

    if (fd != 1ull && fd != 2ull) {
        return neg(LX_EBADF);
    }
    if (lx_user_ok(buf, len, C6OS_PROT_R) == 0) {
        return neg(LX_EFAULT);
    }
    for (i = 0; i < len; i++) {
        port_putc((int)(*(const u8 *)(buf + i)));
    }
    return len;
}

/* writev：ld.so 打印错误信息走的就是它（没有它连"为什么失败"都看不见）。 */
static u64 sys_writev(u64 fd, u64 iov, u64 cnt)
{
    u64 i;
    u64 total = 0ull;

    if (fd != 1ull && fd != 2ull) {
        return neg(LX_EBADF);
    }
    if (lx_user_ok(iov, cnt * 16ull, C6OS_PROT_R) == 0) {
        return neg(LX_EFAULT);
    }
    for (i = 0; i < cnt; i++) {
        u64 base = *(const u64 *)(iov + i * 16ull);
        u64 len = *(const u64 *)(iov + i * 16ull + 8ull);
        u64 r = sys_write(fd, base, len);

        if ((i64)r < 0) {
            return r;
        }
        total += r;
    }
    return total;
}

static u64 sys_getrandom(u64 buf, u64 len, u64 flags)
{
    u64 i;

    (void)flags;
    if (lx_user_ok(buf, len, C6OS_PROT_W) == 0) {
        return neg(LX_EFAULT);
    }
    for (i = 0; i < len; i++) {
        g_seed = g_seed * 1103515245u + 12345u;
        *(u8 *)(buf + i) = (u8)((g_seed >> 16) & 0xffu);
    }
    return len;
}

static u64 sys_brk(u64 want)
{
    if (want == 0ull) {
        return g_brk;
    }
    if (want < g_brk_init) {
        return g_brk;                      /* 不允许降到映像内 */
    }
    if (brk_extend(want) != 0) {
        return g_brk;
    }
    return g_brk;
}

static u64 sys_mmap2(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off)
{
    u64 at = addr;
    u64 n = (len + 4095ull) & ~4095ull;
    /* 文件映射要让内核把文件内容写进这些页，所以先一律给 W；
     * 真正的只读/RELRO/W^X 收窄等 mprotect 落地后再说（见 docs §6）。 */
    u64 perm = C6OS_PROT_R | C6OS_PROT_W;

    if (n == 0ull) {
        return neg(LX_EINVAL);
    }
    if ((prot & 2ull) != 0ull) {
        perm |= C6OS_PROT_W;
    }
    if ((prot & 4ull) != 0ull) {
        perm |= C6OS_PROT_X;              /* ld.so 映射 libc 代码段时必须有 X */
    }
    if ((flags & LX_MAP_FIXED) != 0ull && at != 0ull) {
        at &= ~4095ull;                   /* ld.so 先探地址，再 MAP_FIXED 回来 */
    } else if (at == 0ull) {
        if (g_mmap_top < n) {
            return neg(LX_ENOMEM);
        }
        g_mmap_top -= n;
        at = g_mmap_top;
    } else {
        at &= ~4095ull;
    }
    if ((flags & LX_MAP_ANONYMOUS) != 0ull) {
        if (port_as_map(g_as, at, 0ull, n, perm) != 0) {
            return neg(LX_ENOMEM);
        }
    } else {
        /* 文件映射（MAP_PRIVATE）：逐页"分配页框 → 从 initramfs 拷内容 → 挂页表"。
         * 次序很重要：先挂页表再写用户地址，在 TLB 里还是旧页框的情况下会把
         * 数据写丢（ld.so 的 .dynamic 就是这么被写成全零的）。 */
        u64 fsz = lx_fs_fd_size(fd);
        u64 p = 0ull;

        while (p < n) {
            u64 pa = port_phys_alloc();
            u8 *kpg;
            u64 k;

            if (pa == 0ull) {
                return neg(LX_ENOMEM);
            }
            kpg = (u8 *)port_phys_map(pa);
            k = 0ull;
            while (k < 4096ull) {
                kpg[k] = 0;
                k++;
            }
            if (p + off < fsz) {
                u64 cnt = fsz - (p + off);

                if (cnt > 4096ull) {
                    cnt = 4096ull;
                }
                (void)lx_fs_pread_k(fd, (u64)kpg, cnt, off + p);
            }
            if (port_as_map(g_as, at + p, pa, 4096ull, perm) != 0) {
                return neg(LX_ENOMEM);
            }
            p += 4096ull;
        }
    }
    return at;
}

static u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off)
{
    return sys_mmap2(addr, len, prot, flags, fd, off);
}

static void lx_syscall_do(LxRegs *r)
{
    u64 nr = r->rax;
    u64 a0 = r->rdi;
    u64 a1 = r->rsi;
    u64 a2 = r->rdx;
    u64 a3 = r->r10;
    u64 a4 = r->r8;
    u64 a5 = r->r9;

    switch (nr) {
    case LX_SYS_writev:
        r->rax = sys_writev(a0, a1, a2);
        return;

    case LX_SYS_write:
        r->rax = lx_proc_write(a0, a1, a2);
        return;
    case LX_SYS_read:
        r->rax = lx_proc_read(a0, a1, a2);
        return;

    case LX_SYS_open:
    case LX_SYS_openat:
        r->rax = lx_fs_openat(a0, a1, a2, a3);
        return;

    case LX_SYS_close:
        r->rax = lx_proc_close(a0);
        return;

    case LX_SYS_lseek:
        r->rax = lx_fs_lseek(a0, a1, a2);
        return;

    case LX_SYS_fstat:
        r->rax = lx_fs_fstat(a0, a1);
        return;

    case LX_SYS_newfstatat:
        r->rax = lx_fs_statat(a0, a1, a2, a3);
        return;

    case LX_SYS_getdents64:
        r->rax = lx_fs_getdents64(a0, a1, a2);
        return;

    case LX_SYS_access:
        r->rax = lx_fs_access(a0, a1);
        return;

    case LX_SYS_getcwd:
        if (a1 < 2ull) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        if (lx_user_ok(a0, 2ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        *(u8 *)a0 = '/';
        *(u8 *)(a0 + 1ull) = 0;
        r->rax = 1ull;
        return;

    case LX_SYS_exit:
    case LX_SYS_exit_group:
        port_puts("\n[lx] exit(");
        put_dec(a0);
        port_puts(") —— 用户进程正常结束\n");
        lx_proc_exit(a0);                  /* 不返回 */
        return;

    case LX_SYS_brk:
        r->rax = sys_brk(a0);
        return;

    case LX_SYS_mmap:
        r->rax = sys_mmap(a0, a1, a2, a3, a4, a5);
        return;

    case LX_SYS_pread64:
        r->rax = lx_fs_pread(a0, a1, a2, a3);
        return;

    case LX_SYS_munmap:
        (void)port_as_unmap(g_as, a0, (a1 + 4095ull) & ~4095ull);
        r->rax = 0ull;
        return;

    case LX_SYS_mprotect:
        /* L2 先接受：glibc 用它做 RELRO。真正的权限收窄在 L5 加（需要改 PTE）。 */
        r->rax = 0ull;
        return;

    case LX_SYS_arch_prctl:
        if (a0 == LX_ARCH_SET_FS) {
            port_user_set_fsbase(a1);
            r->rax = 0ull;
            return;
        }
        if (a0 == LX_ARCH_GET_FS) {
            if (lx_user_ok(a1, 8ull, C6OS_PROT_W) == 0) {
                r->rax = neg(LX_EFAULT);
                return;
            }
            *(u64 *)a1 = port_user_get_fsbase();
            r->rax = 0ull;
            return;
        }
        r->rax = neg(LX_EINVAL);
        return;

    case LX_SYS_set_tid_address:
        r->rax = 1ull;
        return;

    case LX_SYS_set_robust_list:
        r->rax = 0ull;
        return;

    case LX_SYS_rseq:
        /* 我们没有可重启序列（不做抢占），**不能**假成功：登记成功意味着内核
         * 会维护那块 rseq 区，而我们不会。返回 ENOSYS 让 glibc 走回退路径，
         * 这正是老内核的行为。 */
        r->rax = neg(LX_ENOSYS);
        return;

    case LX_SYS_prlimit64:
        if (a3 != 0ull) {
            if (lx_user_ok(a3, 16ull, C6OS_PROT_W) == 0) {
                r->rax = neg(LX_EFAULT);
                return;
            }
            *(u64 *)(a3) = 8ull * 1024ull * 1024ull;
            *(u64 *)(a3 + 8ull) = 8ull * 1024ull * 1024ull;
        }
        r->rax = 0ull;
        return;

    case LX_SYS_readlinkat:
        if (user_streq(a1, "/proc/self/exe") != 0) {
            static const char p[] = "/hello";
            u64 i;

            if (a3 < 6ull) {
                r->rax = neg(LX_EINVAL);
                return;
            }
            if (lx_user_ok(a2, 6ull, C6OS_PROT_W) == 0) {
                r->rax = neg(LX_EFAULT);
                return;
            }
            for (i = 0; i < 6ull; i++) {
                *(u8 *)(a2 + i) = (u8)p[i];
            }
            r->rax = 6ull;
            return;
        }
        r->rax = neg(LX_ENOENT);
        return;

    case LX_SYS_getrandom:
        r->rax = sys_getrandom(a0, a1, a2);
        return;

    case LX_SYS_fork:
    case LX_SYS_vfork:
        r->rax = lx_proc_fork(r);
        return;

    case LX_SYS_clone:
        if (a1 != 0ull) {
            r->rax = neg(LX_ENOSYS);       /* 非 0 栈 = 线程，L7 再做 */
            return;
        }
        r->rax = lx_proc_fork(r);
        if (r->rax != 0ull && (i64)r->rax > 0) {
            if (a2 != 0ull && lx_user_ok(a2, 4ull, C6OS_PROT_W) != 0) {
                *(u32 *)a2 = 0u;
            }
            if (a3 != 0ull && lx_user_ok(a3, 4ull, C6OS_PROT_W) != 0) {
                *(u32 *)a3 = 0u;
            }
        }
        return;

    case LX_SYS_execve:
        r->rax = lx_proc_execve(a0, a1, a2, r);
        return;

    case LX_SYS_wait4:
        r->rax = lx_proc_wait4((i64)a0, a1, a2, a3);
        return;

    case LX_SYS_pipe:
    case LX_SYS_pipe2:
        r->rax = lx_proc_pipe2(a0, a1);
        return;

    case LX_SYS_dup:
        r->rax = lx_proc_dup(a0);
        return;

    case LX_SYS_dup2:
        r->rax = lx_proc_dup2(a0, a1);
        return;

    case LX_SYS_getppid:
        if (lx_cur_proc() != (LxProc *)0) {
            r->rax = (u64)lx_cur_proc()->ppid;
        } else {
            r->rax = 0ull;
        }
        return;

    case LX_SYS_rt_sigaction:
    case LX_SYS_rt_sigprocmask:
    case LX_SYS_sigaltstack:
    case LX_SYS_setpgid:
    case LX_SYS_setsid:
    case LX_SYS_umask:
    case LX_SYS_chdir:
    case LX_SYS_fcntl:
        /* L5 先把进程模型跑通；真正的信号投递是 L6，chdir/fcntl 是 L6+ */
        r->rax = 0ull;
        return;

    case LX_SYS_getpgrp:
        r->rax = 1ull;
        return;

    case LX_SYS_getuid:
    case LX_SYS_geteuid:
    case LX_SYS_getgid:
    case LX_SYS_getegid:
    case LX_SYS_setuid:
    case LX_SYS_setgid:
        r->rax = 0ull;                     /* 单用户 root */
        return;

    case LX_SYS_ioctl:
        /* 没有真终端：一律 ENOTTY，ls 会退回单列输出 */
        r->rax = neg(LX_ENOTTY);
        return;

    case LX_SYS_time:
        if (a0 != 0ull && lx_user_ok(a0, 8ull, C6OS_PROT_W) != 0) {
            *(u64 *)a0 = 1700000000ull;
        }
        r->rax = 1700000000ull;            /* 没有 RTC：给个固定时刻 */
        return;

    case LX_SYS_getpid:
    case LX_SYS_gettid:
        r->rax = 1ull;
        return;

    case LX_SYS_uname:
        if (lx_user_ok(a0, 390ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        {
            u8 *u = (u8 *)a0;
            const char *vals[6];
            u64 f;
            u64 i;

            vals[0] = "Linux";
            vals[1] = "c6los";
            vals[2] = "6.1.0-c6los";
            vals[3] = "#1";
            vals[4] = "x86_64";
            vals[5] = "(none)";
            for (f = 0; f < 6ull; f++) {
                for (i = 0; i < 65ull; i++) {
                    u[f * 65ull + i] = 0;
                }
                for (i = 0; vals[f][i] != '\0' && i < 64ull; i++) {
                    u[f * 65ull + i] = (u8)vals[f][i];
                }
            }
        }
        r->rax = 0ull;
        return;

    case LX_SYS_futex:
        r->rax = 0ull;                     /* L2：单线程，唤醒即成功 */
        return;

    default:
        port_puts("[lx] ENOSYS nr=");
        put_dec(nr);
        port_puts("\n");
        r->rax = neg(LX_ENOSYS);
        return;
    }
}

/* 系统调用出口：所有处理完（含阻塞返回）之后统一让出一次 CPU。 */
void lx_syscall(LxRegs *r)
{
    lx_syscall_do(r);
    lx_sched_tick();
}
