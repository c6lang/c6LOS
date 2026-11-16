#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"

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

static u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags)
{
    u64 at = addr;
    u64 n = (len + 4095ull) & ~4095ull;

    (void)flags;
    if (n == 0ull) {
        return neg(LX_EINVAL);
    }
    if (at == 0ull) {
        if (g_mmap_top < n) {
            return neg(LX_ENOMEM);
        }
        g_mmap_top -= n;
        at = g_mmap_top;
    } else {
        at &= ~4095ull;
    }
    if (port_as_map(g_as, at, 0ull, n,
                    C6OS_PROT_R | ((prot & 2ull) != 0ull ? C6OS_PROT_W : 0u)) != 0) {
        return neg(LX_ENOMEM);
    }
    return at;
}

void lx_syscall(LxRegs *r)
{
    u64 nr = r->rax;
    u64 a0 = r->rdi;
    u64 a1 = r->rsi;
    u64 a2 = r->rdx;
    u64 a3 = r->r10;

    switch (nr) {
    case LX_SYS_write:
        r->rax = sys_write(a0, a1, a2);
        return;
    case LX_SYS_read:
        r->rax = 0ull;                     /* L2 没有 stdin，读 EOF */
        return;

    case LX_SYS_exit:
    case LX_SYS_exit_group:
        port_puts("\n[lx] exit(");
        put_dec(a0);
        port_puts(") —— 用户进程正常结束\n");
        port_panic_halt();
        return;

    case LX_SYS_brk:
        r->rax = sys_brk(a0);
        return;

    case LX_SYS_mmap:
        r->rax = sys_mmap(a0, a1, a2, a3);
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
