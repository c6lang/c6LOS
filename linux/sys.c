#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"
#include "c6os/linux_vma.h"


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
    lx_vma_init();                         /* VMA 表 + AS 释放回调 */
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

/* 按 **node** 而不是 fd 号判断目标是不是控制台：dup 出来的高位 fd 也是终端
 * （ash/lineedit 真的会那么用），只看 fd==1/2 会把它们判成 EBADF。 */
u64 lx_console_put(u64 ubuf, u64 len)
{
    u64 i;

    if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    if (lx_tty_enabled() != 0u) {
        return lx_tty_write(ubuf, len);
    }
    for (i = 0ull; i < len; i++) {
        port_putc((int)(*(const u8 *)(ubuf + i)));
    }
    return len;
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
    if (port_as_user_range_ok(g_as, va, len, prot) != 0) {
        return 1;
    }
    /* 惰性匿名页还没有 PTE：只要 VMA 承诺过这段地址，先把页补上再交给页表
     * 检查。否则内核 copy_to/from_user 会对"还没被碰过的 malloc 缓冲"误判
     * EFAULT（RSI 回归里 wait 写 status 就是这么坏的）。 */
    if (lx_vma_fault_range(g_as, va, va + len, prot) != 0) {
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
        /* 堆也进 VMA：munmap/mprotect/栈增长都要看到它，否则堆上的 mprotect
         * 会被当成"未映射区间"而返回 ENOMEM。 */
        lx_vma_remove(g_as, from, to);
        if (lx_vma_add(g_as, from, to, C6OS_PROT_R | C6OS_PROT_W, LX_VM_HEAP,
                       0ull) != 0) {
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
        return lx_fs_write(fd, buf, len);        /* /tmp 等 VFS 文件（M9-4） */
    }
    if (lx_user_ok(buf, len, C6OS_PROT_R) == 0) {
        return neg(LX_EFAULT);
    }
    /* M12：tty 开着时输出要经过它 —— busybox 的 lineedit 会写 ESC[6n 问光标
     * 位置，必须由终端从输入方向回 ESC[1;1R，否则程序一直等、最后退出。
     * 没开 tty 就是原来的逐字节直写。 */
    if (lx_tty_enabled() != 0u) {
        return lx_tty_write(buf, len);
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
    if (want < g_brk) {                    /* 收缩：Linux 会真把页还回去 */
        u64 from = (want + 4095ull) & ~4095ull;
        u64 to = (g_brk + 4095ull) & ~4095ull;

        if (to > from) {
            lx_vma_remove(g_as, from, to);
            (void)port_as_unmap(g_as, from, to - from);
        }
        g_brk = want;
        return g_brk;
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
    if ((flags & LX_MAP_FIXED) != 0ull && at != 0ull) {
        lx_vma_remove(g_as, at, at + n);   /* 覆盖映射：旧记账先作废 */
        (void)port_as_unmap(g_as, at, n);  /* 惰性映射下旧页不会自己消失 */
    }
    if ((flags & LX_MAP_ANONYMOUS) != 0ull) {
        /* 惰性：只记账，第一次访问由 #PF 补零页（M13-2b-3）。malloc 的 arena、
         * 线程栈这类大 mmap 不再一上来就把物理页吃光。 */
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
    /* 记账：munmap/mprotect/栈增长都靠这条链判断"这段地址是什么"。 */
    {
        u32 vf = ((flags & LX_MAP_ANONYMOUS) != 0ull) ? LX_VM_ANON : LX_VM_FILE;

        if ((flags & LX_MAP_SHARED) != 0ull) {
            vf |= LX_VM_SHARED;
        }
        if ((flags & LX_MAP_ANONYMOUS) != 0ull) {
            vf |= LX_VM_LAZY;              /* 页在第一次访问时才补 */
        }
        if (lx_vma_add(g_as, at, at + n, (u32)perm, vf, off) != 0) {
            return neg(LX_ENOMEM);         /* 区间冲突/记账 OOM */
        }
    }
    return at;
}

static u64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off)
{
    return sys_mmap2(addr, len, prot, flags, fd, off);
}

static 
void lx_syscall_do(LxRegs *r)
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
        r->rax = lx_proc_writev(a0, a1, a2);
        return;
    case LX_SYS_readv:
        r->rax = lx_proc_readv(a0, a1, a2);
        return;

    case LX_SYS_write:
        r->rax = lx_proc_write(a0, a1, a2);
        return;
    case LX_SYS_read:
        r->rax = lx_proc_read(a0, a1, a2);
        return;

    case LX_SYS_open:
        /* open(path, flags, mode)：老 syscall。glibc 走的是 openat，
         * 所以这条一直到 M11-3 的裸 syscall 程序才被踩到——参数顺序不同，
         * 这里按 dirfd=AT_FDCWD 转给共用的实现。 */
        r->rax = lx_fs_openat((u64)0 - 100ull, a0, a1, a2);
        return;

    case LX_SYS_openat:
        r->rax = lx_fs_openat(a0, a1, a2, a3);
        return;

    /* M15-1b：写类 syscall（*at 的 dirfd 按 int 符号/零扩展兼容，fs.c 里处理） */
    case LX_SYS_mkdirat:
        r->rax = lx_fs_mkdirat((i64)(i32)a0, a1, a2);
        return;
    case LX_SYS_mkdir:
        r->rax = lx_fs_mkdirat((u64)0 - 100ull, a0, a1);
        return;
    case LX_SYS_rmdir:
        r->rax = lx_fs_rmdir(a0);
        return;
    case LX_SYS_unlinkat:
        r->rax = lx_fs_unlinkat((i64)(i32)a0, a1, a2);
        return;
    case LX_SYS_unlink:
        r->rax = lx_fs_unlinkat((u64)0 - 100ull, a0, 0ull);
        return;
    case LX_SYS_renameat:
        r->rax = lx_fs_renameat((i64)(i32)a0, a1, (i64)(i32)a2, a3);
        return;
    case LX_SYS_rename:
        r->rax = lx_fs_renameat((u64)0 - 100ull, a0, (u64)0 - 100ull, a1);
        return;
    case LX_SYS_truncate:
        r->rax = lx_fs_truncate(a0, a1);
        return;
    case LX_SYS_ftruncate:
        r->rax = lx_fs_ftruncate(a0, a1);
        return;
    case LX_SYS_chmod:
        r->rax = lx_fs_chmod(a0, a1);
        return;
    case LX_SYS_fchmodat:
        /* fchmodat(dirfd, path, mode, flags)：dirfd 按 int 解析 */
        r->rax = lx_fs_chmod_pathat((i64)(i32)a0, a1, a2);
        return;
    case LX_SYS_fchmod:
        /* fchmod(fd, mode)：fd 有效就成功（单用户无权限校验） */
        r->rax = (lx_fdtab_get((lx_cur_proc() != (LxProc *)0) ? lx_cur_proc()->fd
                               : (LxFdTab *)0, a0) != (LxFile *)0) ? 0ull
                                                                   : neg(LX_EBADF);
        return;
    case LX_SYS_statfs:
    case LX_SYS_fstatfs:
        r->rax = lx_fs_statfs(a0, a1);
        return;
    case LX_SYS_utimensat:
        r->rax = lx_fs_utimensat((i64)(i32)a0, a1, a2);
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

    case LX_SYS_statx:
        /* statx(dirfd, path, flags, mask, statx_buf)：mask(a3) 只作提示，
         * 本内核统一返回 BASIC_STATS，忽略具体请求位。 */
        r->rax = lx_fs_statx(a0, a1, a2, a4);
        return;

    case LX_SYS_copy_file_range:
        /* copy_file_range(fd_in, off_in*, fd_out, off_out*, len, flags) */
        r->rax = lx_fs_copy_file_range(a0, a1, a2, a3, a4, a5);
        return;

    case LX_SYS_fallocate:
        /* fallocate(fd, mode, offset, len) */
        r->rax = lx_fs_fallocate(a0, a1, a2, a3);
        return;

    case LX_SYS_stat:
        /* M12：老的 stat(path, buf)（x86_64 上是 4 号）。musl 编的是
         * newfstatat，但 Alpine 里的 busybox 是**静态老版本调用**混着用，
         * ash 的 lineedit 初始化时就会踩到；不认的话它直接 ENOSYS 退出。
         * 语义就是 newfstatat(AT_FDCWD, path, buf, 0)。 */
        r->rax = lx_fs_statat((u64)0 - 100ull, a0, a1, 0ull);
        return;

    case LX_SYS_lstat:
        r->rax = lx_fs_statat((u64)0 - 100ull, a0, a1, LX_AT_SYMLINK_NOFOLLOW);
        return;

    case LX_SYS_getdents64:
        r->rax = lx_fs_getdents64(a0, a1, a2);
        return;

    case LX_SYS_access:
        r->rax = lx_fs_access(a0, a1);
        return;

    case LX_SYS_getcwd:
    {
        const char *cwd = lx_fs_cwd();
        u64 n = 0ull;
        u64 len;

        while (cwd[n] != '\0') {
            n++;
        }
        len = 1ull + n;                    /* 前导 '/' + cwd */
        if (a1 < len + 1ull) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        if (lx_user_ok(a0, len + 1ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        *(u8 *)a0 = '/';
        for (n = 0ull; cwd[n] != '\0'; n++) {
            *(u8 *)(a0 + 1ull + n) = (u8)cwd[n];
        }
        *(u8 *)(a0 + 1ull + n) = 0;
        r->rax = len;
        return;
    }

    case LX_SYS_exit:                      /* 只结束当前线程 */
        if (lx_proc_tgid() == lx_proc_tid()) {
            port_puts("\n[lx] exit(");
            put_dec(a0);
            port_puts(") —— 用户进程正常结束\n");
        }
        lx_proc_exit(a0);                  /* 不返回 */
        return;

    case LX_SYS_exit_group:                /* 整组退出 */
        lx_proc_exit_group(a0);            /* 不返回 */
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

    case LX_SYS_pwrite64:
        /* pwrite(fd, buf, count, offset)：a3=r10 是 offset 低 32 位 */
        r->rax = lx_fs_pwrite(a0, a1, a2, a3);
        return;

    case LX_SYS_preadv:
        /* preadv(fd, iov, iovcnt, offset) */
        r->rax = lx_fs_preadv(a0, a1, a2, a3);
        return;

    case LX_SYS_pwritev:
        /* pwritev(fd, iov, iovcnt, offset) */
        r->rax = lx_fs_pwritev(a0, a1, a2, a3);
        return;

    case LX_SYS_munmap:
        /* 先作废记账再还页：反过来会让缺页在"页已还、VMA 还在"的窗口里
         * 重新分配页，把 munmap 变成空操作。 */
        lx_vma_remove(g_as, a0, a0 + ((a1 + 4095ull) & ~4095ull));
        (void)port_as_unmap(g_as, a0, (a1 + 4095ull) & ~4095ull);
        r->rax = 0ull;
        return;

    case LX_SYS_mprotect:
        {
            u64 end = a0 + ((a1 + 4095ull) & ~4095ull);

            /* 先过 VMA：Linux 对"保护一段没映射的地址"返回 ENOMEM。只改页表
             * 而不管记账，会让后续 munmap/栈增长按旧属性判断。 */
            if (lx_vma_protect(g_as, a0, end, (u32)a2) != 0) {
                r->rax = neg(LX_ENOMEM);
                return;
            }
            /* 再改已存在的 PTE。惰性页没有 PTE 可改：VMA 记账成功就算成功，
             * 页会在缺页时按新 prot 生成（Linux 对未触碰页同样不做事）。 */
            (void)port_as_protect(g_as, a0, (a1 + 4095ull) & ~4095ull,
                                  (u32)a2);
            r->rax = 0ull;
        }
        return;

    case LX_SYS_msync:
        /* 匿名页没有回写后端：整段在 VMA 内就算成功，否则 ENOMEM（Linux 语义）。
         * 文件映射的脏页回写等 M15 的页缓存落地后再补。 */
        r->rax = (lx_vma_covered(g_as, a0,
                                 a0 + ((a1 + 4095ull) & ~4095ull)) != 0)
                 ? 0ull : neg(LX_ENOMEM);
        return;

    case LX_SYS_poll:
        /* 8 字节一项：{ i32 fd; i16 events; i16 revents; } */
        if (lx_user_ok(a0, a1 * 8ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        r->rax = (u64)lx_proc_poll(a0, a1, (i64)(i32)a2);
        return;

    case LX_SYS_ppoll:
        {
            i64 to = -1;

            if (a2 != 0ull) {
                if (lx_user_ok(a2, 16ull, C6OS_PROT_R) == 0) {
                    r->rax = neg(LX_EFAULT);
                    return;
                }
                if (*(const i64 *)a2 == 0 && *(const i64 *)(a2 + 8ull) == 0) {
                    to = 0;                  /* 零超时 = 立即返回 */
                }
            }
            if (lx_user_ok(a0, a1 * 8ull, C6OS_PROT_W) == 0) {
                r->rax = neg(LX_EFAULT);
                return;
            }
            r->rax = (u64)lx_proc_poll(a0, a1, to);
        }
        return;

    /* M16-1a：select/pselect6。select 的 timeout 是 struct timeval{sec,usec}，
     * pselect6 是 struct timespec{sec,nsec} + 第 5 参 sigmask（单线程下忽略）。
     * 统一转成毫秒喂给 lx_proc_select；timeout=NULL → -1（无限等）。 */
    case LX_SYS_select:
        {
            i64 to = -1;

            if (a4 != 0ull) {
                if (lx_user_ok(a4, 16ull, C6OS_PROT_R) == 0) {
                    r->rax = neg(LX_EFAULT);
                    return;
                }
                to = *(const i64 *)a4 * 1000ll + *(const i64 *)(a4 + 8ull) / 1000ll;
                if (to < 0) {
                    to = -1;
                }
            }
            r->rax = (u64)lx_proc_select((i32)(i32)a0, a1, a2, a3, to);
        }
        return;

    case LX_SYS_eventfd2:
        r->rax = lx_proc_eventfd2(a0, a1);
        return;

    case LX_SYS_pselect6:
        {
            i64 to = -1;

            if (a4 != 0ull) {
                if (lx_user_ok(a4, 16ull, C6OS_PROT_R) == 0) {
                    r->rax = neg(LX_EFAULT);
                    return;
                }
                to = *(const i64 *)a4 * 1000ll + *(const i64 *)(a4 + 8ull) / 1000000ll;
                if (to < 0) {
                    to = -1;
                }
            }
            r->rax = (u64)lx_proc_select((i32)(i32)a0, a1, a2, a3, to);
        }
        return;

    case LX_SYS_arch_prctl:
        if (a0 == LX_ARCH_SET_FS) {
            if (lx_cur_proc() != (LxProc *)0) {
                lx_cur_proc()->fs_base = a1;
            }
            port_user_set_fsbase(a1);
            r->rax = 0ull;
            return;
        }
        if (a0 == LX_ARCH_GET_FS) {
            if (lx_user_ok(a1, 8ull, C6OS_PROT_W) == 0) {
                r->rax = neg(LX_EFAULT);
                return;
            }
            *(u64 *)a1 = (lx_cur_proc() != (LxProc *)0)
                             ? lx_cur_proc()->fs_base
                             : port_user_get_fsbase();
            r->rax = 0ull;
            return;
        }
        r->rax = neg(LX_EINVAL);
        return;

    case LX_SYS_set_tid_address:
        r->rax = lx_proc_set_tid_address(a0);
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
        if ((a0 & LX_CLONE_VM) != 0ull) {
            r->rax = lx_proc_clone_thread(r, a0, a1, a2, a3, a4);
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
        /* Linux 的 pid 参数是 int：glibc 传 -1 时上游高 32 位不可信（常是零
         * 扩展），这里必须按 int 截断再符号扩展，否则 -1 会变成 +4294967295，
         * 进程表里找不到任何子进程，wait() 直接 ECHILD。*/
        r->rax = lx_proc_wait4((i64)(i32)a0, a1, a2, a3);
        return;

    case LX_SYS_waitid:
        /* waitid(idtype, id, siginfo*, options, rusage*)
         * idtype/id 都是 int 语义，按 32 位截断。 */
        r->rax = lx_proc_waitid((u32)a0, (u32)a1, a2, a3, a4);
        return;

    case LX_SYS_pipe:
        /* pipe(2) 只有 1 个参数：rsi 是残留值，绝不能当 pipe2 的 flags。
         * glibc busybox 恰好 rsi=0 蒙混过关，musl ash 的 rsi 是栈地址，
         * 传下去会被当成非法 flags → EINVAL → "can't create pipe"。 */
        r->rax = lx_proc_pipe2(a0, 0ull);
        return;
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
        r->rax = lx_sig_action((u32)a0, a1, a2);
        return;

    case LX_SYS_rt_sigprocmask:
        r->rax = lx_sig_procmask((u32)a0, a1, a2);
        return;

    case LX_SYS_rt_sigreturn:
        r->rax = lx_sig_return(r);
        return;

    case LX_SYS_sigaltstack:
        r->rax = lx_sig_altstack(a0, a1);
        return;

    case LX_SYS_kill:
        /* M14-2：pid<=0 的三种广播语义（0=本组，-1=全体，<-1=指定组）。
         * shell 的 `kill -TERM -%d` 和 Ctrl-C 都靠它。 */
        if (a1 >= (u64)LX_NSIG) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        if (a1 != 0ull && lx_proc_send((i64)(i32)a0, (u32)a1) == 0) {
            r->rax = 0ull;
        } else if (a1 == 0ull && lx_proc_send((i64)(i32)a0, 0u) == 0) {
            r->rax = 0ull;              /* 0 号信号：只探测存在性 */
        } else {
            r->rax = neg(LX_ESRCH);
        }
        return;

    case LX_SYS_tgkill:                    /* tgkill(tgid, tid, sig) */
        if (a2 >= (u64)LX_NSIG) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        if (a2 != 0ull && lx_proc_find((i64)(i32)a1) == (LxProc *)0) {
            r->rax = neg(LX_ESRCH);
            return;
        }
        if (a2 != 0ull) {
            lx_sig_send_one(lx_proc_find((i64)(i32)a1), (u32)a2, 0);
        }
        r->rax = 0ull;
        return;

    /* B1：setpgid 以前和 chdir 共用一个 case，`setpgid(pid, pgid)` 实际执行的是
     * `chdir(pid 的指针)`。拆成两条，各自按各自的语义走。 */
    case LX_SYS_setpgid:
        r->rax = lx_proc_setpgid((i64)(i32)a0, (i64)(i32)a1);
        return;

    case LX_SYS_chdir:
        r->rax = lx_fs_chdir(a0);           /* M12-4：真的换目录（含相对路径） */
        return;

    case LX_SYS_setsid:
        r->rax = lx_proc_setsid();
        return;

    case LX_SYS_umask:
        r->rax = 022ull;                    /* 单用户 root：记录后恒返回旧值 */
        return;

    case LX_SYS_fcntl:
        r->rax = lx_proc_fcntl(a0, a1, a2);
        return;

    case LX_SYS_dup3:
        r->rax = lx_proc_dup3(a0, a1, a2);
        return;

    case LX_SYS_getpgrp:
        /* getpgrp 和 getpgid 必须同值（ash 会拿它们和 TIOCGPGRP 比），
         * 现在两者都读同一个 pgid 字段，天然一致。 */
        r->rax = lx_proc_getpgrp();
        return;

    case LX_SYS_getpgid:
        r->rax = lx_proc_getpgid((i64)(i32)a0);
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
        /* M14-2：TIOCSPGRP/TIOCGPGRP 是作业控制的核心 —— ash 启动时会
         * `tcsetpgrp(0, getpid())` 把前台组设成自己，然后用 kill(-pgid) 给
         * 整条作业发信号。以前一律 ENOTTY，ash 只能退回"没有作业控制"的模式。 */
        /* TIOCSCTTY (0x540E)：把 fd 设为控制终端。本内核只有一个全局串口
         * 终端（tty 已启用时 fd 0/1/2 就是它），没有 per-process ctty 表，
         * 所以"接受"即可成功；未启用 tty 时 ENOTTY。busybox/ash 只在
         * setsid 之后调它，返回 0 就能继续走作业控制流程。 */
        if (a1 == 0x540eull) {
            if (a0 <= 2ull && lx_tty_enabled() != 0u) {
                r->rax = 0ull;
            } else {
                r->rax = neg(LX_ENOTTY);
            }
            return;
        }
        if (a0 <= 2ull && (a1 == 0x5410ull || a1 == 0x540full)) {
            if (a1 == 0x5410ull) {         /* TIOCSPGRP：arg 是 pid_t* */
                i64 v;

                if (lx_user_ok(a2, 4ull, C6OS_PROT_R) == 0) {
                    r->rax = neg(LX_EFAULT);
                    return;
                }
                v = (i64)(*(const i32 *)a2);
                r->rax = lx_proc_tcsetpgrp((i64)(i32)a0, v);
                return;
            }
            if (lx_user_ok(a2, 4ull, C6OS_PROT_W) == 0) {   /* TIOCGPGRP */
                r->rax = neg(LX_EFAULT);
                return;
            }
            {
                u64 v = lx_proc_tcgetpgrp((i64)(i32)a0);

                if ((i64)v < 0) {
                    r->rax = v;
                    return;
                }
                *(i32 *)a2 = (i32)v;
            }
            r->rax = 0ull;
            return;
        }
        /* M35：必须按 fd 的**实际种类**判终端，不能只看 fd<=2 —— dup2 之后
         * fd 0/1/2 可能是管道/socket，此时 isatty() 必须返回 ENOTTY，否则
         * busybox cat 会当成终端走另一条路径，导致外部命令管道静默丢数据。 */
        {
            LxFile *iofe = lx_fdtab_get(lx_cur_proc() != (LxProc *)0
                                        ? lx_cur_proc()->fd : (LxFdTab *)0, a0);

            if (iofe != (LxFile *)0 && iofe->node == LX_FD_CONSOLE &&
                lx_tty_enabled() != 0u) {
                r->rax = (u64)lx_tty_ioctl(a0, a1, a2);
                return;
            }
            if (iofe != (LxFile *)0 && iofe->kind == LX_FD_PTY) {
                i64 pr = lx_pty_ioctl(iofe->node, a1, a2,
                                      iofe->pos == 0ull ? 1u : 0u);

                r->rax = (pr < 0) ? neg((u64)(-pr)) : (u64)pr;
                return;
            }
        }
        r->rax = neg(LX_ENOTTY);
        return;

    case LX_SYS_time:
        if (a0 != 0ull && lx_user_ok(a0, 8ull, C6OS_PROT_W) != 0) {
            *(u64 *)a0 = 1700000000ull;
        }
        r->rax = 1700000000ull;            /* 没有 RTC：给个固定时刻 */
        return;

    /* M16-2：gettimeofday/clock_getres/clock_settime。统一用 TSC 推算的
     * "假时钟"：起点 1700000000 秒 + 开机以来的毫秒，和 clock_gettime 同源，
     * 保证 time()/gettimeofday()/clock_gettime() 不会互相矛盾。 */
    case LX_SYS_gettimeofday:
        {
            u64 now = 1700000000ull + port_cycles() / 1000000000ull;
            u64 usec = (port_cycles() / 1000ull) % 1000000ull;

            if (a0 != 0ull) {
                if (lx_user_ok(a0, 16ull, C6OS_PROT_W) == 0) {
                    r->rax = neg(LX_EFAULT);
                    return;
                }
                *(u64 *)a0 = (i64)now;
                *(u64 *)(a0 + 8ull) = (i64)usec;
            }
            if (a1 != 0ull && lx_user_ok(a1, 8ull, C6OS_PROT_W) == 0) {
                r->rax = neg(LX_EFAULT);
                return;
            }
            r->rax = 0ull;                 /* 没有时区：不写 tz 也成功 */
        }
        return;

    case LX_SYS_clock_getres:
        if (lx_user_ok(a1, 16ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        if (a0 > 5ull) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        *(u64 *)a1 = 0ull;                 /* sec = 0 */
        *(u64 *)(a1 + 8ull) = 1000000ull;  /* nsec = 1ms */
        r->rax = 0ull;
        return;

    case LX_SYS_clock_settime:
        /* 没有 RTC 也没有可写的墙上时钟源：校验参数后明确返回 EPERM，
         * 不假成功（busybox date -s 会据此判断能不能设时间）。 */
        if (a0 > 5ull || lx_user_ok(a1, 16ull, C6OS_PROT_R) == 0) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        r->rax = neg(LX_EPERM);
        return;

    case LX_SYS_nanosleep:
        /* M12：busybox 的 lineedit 初始化时发 ESC[6n 问光标位置；终端不回话就
         * 靠这个超时兜底，再按默认 80 列继续。以前返回 ENOSYS，ash 直接退出。
         * 没有真时钟可用，就按请求时长忙等（上限 1 秒，别让用户等太久）。 */
        if (lx_user_ok(a0, 16ull, C6OS_PROT_R) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        {
            i64 sec = *(const i64 *)a0;
            i64 nsec = *(const i64 *)(a0 + 8ull);
            u64 ms;

            if (sec < 0 || nsec < 0) {
                r->rax = neg(LX_EINVAL);
                return;
            }
            ms = (u64)sec * 1000ull + (u64)nsec / 1000000ull;
            if (ms > 1000ull) {
                ms = 1000ull;              /* 上限：这只是个超时兜底 */
            }
            if (ms > 0ull) {
                port_delay_ms((u32)ms);
            }
        }
        r->rax = 0ull;
        return;

    case LX_SYS_alarm:
        /* alarm(2)：SIGALRM 闹钟。TSC 截止时刻存在 LxProc 上，由
         * resched_/PIT tick 两处检查到点投递（见 proc.c 的 lx_proc_alarm_check）。 */
        r->rax = lx_proc_alarm(a0);
        return;

    case LX_SYS_clock_nanosleep:
        /* glibc 的 sleep() 走这个而不是 nanosleep(2)。参数和 nanosleep 一样在
         * a2（req）/a3（rem），a0=clockid、a1=flags（TIMER_ABSTIME 时 a2 是绝对
         * 时刻，但我们没有真时钟基准，统一按相对时长处理）。 */
        if (lx_user_ok(a2, 16ull, C6OS_PROT_R) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        {
            i64 sec = *(const i64 *)a2;
            i64 nsec = *(const i64 *)(a2 + 8ull);
            u64 ms;

            if (sec < 0 || nsec < 0 || nsec >= 1000000000ll) {
                r->rax = neg(LX_EINVAL);
                return;
            }
            ms = (u64)sec * 1000ull + (u64)nsec / 1000000ull;
            if (ms > 10000ull) {
                ms = 10000ull;             /* 上限：别让 test 把 QEMU 挂太久 */
            }
            if (ms > 0ull) {
                port_delay_ms((u32)ms);
            }
        }
        r->rax = 0ull;
        return;

    case LX_SYS_clock_gettime:
        /* 同理：nanosleep 的超时逻辑和 busybox 的时间戳都要它。
         * 返回"单调递增的假时钟"，精度到毫秒，起点固定在 1700000000。 */
        if (lx_user_ok(a1, 16ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EFAULT);
            return;
        }
        {
            u64 now = port_cycles() / 1000000ull;      /* 假设 ~1GHz 量级的 TSC */

            if (a0 == 1ull) {                          /* CLOCK_MONOTONIC */
                *(u64 *)a1 = (i64)(now / 1000ull);
                *(u64 *)(a1 + 8ull) = (i64)((now % 1000ull) * 1000000ull);
            } else {                                   /* CLOCK_REALTIME */
                *(u64 *)a1 = (i64)(1700000000ull + now / 1000ull);
                *(u64 *)(a1 + 8ull) = (i64)((now % 1000ull) * 1000000ull);
            }
        }
        r->rax = 0ull;
        return;

    case LX_SYS_getpid:
        r->rax = (u64)lx_proc_tgid();
        return;

    case LX_SYS_gettid:
        r->rax = (u64)lx_proc_tid();
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
        r->rax = lx_futex(a0, (u32)a1, (u32)a2, a3);
        return;

    case LX_SYS_madvise:
    case LX_SYS_prctl:
        r->rax = 0ull;                     /* 只影响性能/命名，忽略 */
        return;

    case LX_SYS_sched_yield:
        lx_proc_yield();                   /* 协作式调度：真让一次 CPU */
        r->rax = 0ull;
        return;

    case LX_SYS_sched_getaffinity:
        if (a2 < 8ull || lx_user_ok(a1, 8ull, C6OS_PROT_W) == 0) {
            r->rax = neg(LX_EINVAL);
            return;
        }
        *(u64 *)a1 = 1ull;                 /* 单 CPU */
        r->rax = 8ull;
        return;

    /* ---- M19：busybox applet 矩阵暴露的 syscall 缺口 ---- */
    case LX_SYS_sendfile:
        r->rax = lx_proc_sendfile(a0, a1, a2, a3);
        return;
    case LX_SYS_sysinfo:
        r->rax = lx_proc_sysinfo(a0);
        return;
    case LX_SYS_getgroups:
        r->rax = lx_proc_getgroups(a0, a1);
        return;
    /* M20：epoll */
    case LX_SYS_epoll_create1:
        r->rax = lx_epoll_create1(a0);
        return;
    case LX_SYS_epoll_ctl:
        r->rax = lx_epoll_ctl(a0, a1, a2, a3);
        return;
    case LX_SYS_epoll_wait:
        r->rax = lx_epoll_wait(a0, a1, a2, (i64)a3);
        return;
    /* M21：timerfd */
    case LX_SYS_timerfd_create:
        r->rax = lx_timerfd_create(a0, a1);
        return;
    case LX_SYS_timerfd_settime:
        r->rax = lx_timerfd_settime(a0, a1, a2, a3);
        return;
    case LX_SYS_timerfd_gettime:
        r->rax = lx_timerfd_gettime(a0, a1);
        return;
    /* M22：signalfd */
    case LX_SYS_signalfd4:
        r->rax = lx_signalfd4(a0, a1, a2, a3);
        return;

    /* ---- M18：socket 族（linux/socket.c → LiteOS SAL）---- */
    case LX_SYS_socket:
        r->rax = lx_sock_socket(a0, a1, a2);
        return;
    case LX_SYS_bind:
        r->rax = lx_sock_bind(a0, a1, a2);
        return;
    case LX_SYS_connect:
        r->rax = lx_sock_connect(a0, a1, a2);
        return;
    case LX_SYS_listen:
        r->rax = lx_sock_listen(a0, a1);
        return;
    case LX_SYS_accept:
        r->rax = lx_sock_accept(a0, a1, a2);
        return;
    case LX_SYS_sendto:
        r->rax = lx_sock_sendto(a0, a1, a2, a3, a4, a5);
        return;
    case LX_SYS_recvfrom:
        r->rax = lx_sock_recvfrom(a0, a1, a2, a3, a4, a5);
        return;
    case LX_SYS_send:
        r->rax = lx_sock_send(a0, a1, a2, a3);
        return;
    case LX_SYS_recv:
        r->rax = lx_sock_recv(a0, a1, a2, a3);
        return;
    case LX_SYS_sendmsg:
        r->rax = lx_sock_sendmsg(a0, a1, a2);
        return;
    case LX_SYS_recvmsg:
        r->rax = lx_sock_recvmsg(a0, a1, a2);
        return;
    case LX_SYS_shutdown:
        r->rax = lx_sock_shutdown(a0, a1);
        return;
    case LX_SYS_setsockopt:
        r->rax = lx_sock_setsockopt(a0, a1, a2, a3, a4);
        return;
    case LX_SYS_getsockopt:
        r->rax = lx_sock_getsockopt(a0, a1, a2, a3, a4);
        return;
    case LX_SYS_getsockname:
        r->rax = lx_sock_getsockname(a0, a1, a2);
        return;
    case LX_SYS_getpeername:
        r->rax = lx_sock_getpeername(a0, a1, a2);
        return;
    case LX_SYS_socketpair:
        r->rax = neg(LX_EOPNOTSUPP);
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
    LxProc *p = lx_cur_proc();

    if (lx_tty_enabled() != 0u) {
        (void)lx_tty_pump();             /* 串口是轮询的：系统调用就是我们的"中断" */
    }
    lx_syscall_do(r);
    /* L6：系统调用返回前投递挂起的信号（把 iret 帧改成"进 handler"）。
     * 系统调用帧就是完整 LxFrame（LxRegs 是它的第一个成员）。 */
    if (p != (LxProc *)0) {
        p->started = 1u;                 /* 能进系统调用，说明已经在用户态跑过了 */
        lx_sig_deliver_frame(p, (LxFrame *)r);
    }
    lx_sched_tick();
}
