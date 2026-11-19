#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_vma.h"

/* ELF64 加载器。
 *  L2：ET_EXEC（静态非 PIE）。
 *  L4：ET_DYN（PIE / ld.so）+ PT_INTERP + AT_BASE/AT_ENTRY。
 *
 * 动态链接不由内核做：内核只把 ld.so 摆到 AT_BASE、把 RIP 指向它的入口，
 * 其余（libc.so 的 mmap、relocation、符号解析）全是 ld.so 自己的事——
 * 这正是 linuxulator/WSL1 这类 personality 的做法，也是 Tier 3 的第一条原则。 */

#define PAGE      4096ull
#define PT_LOAD   1u
#define PT_INTERP 3u
#define ET_EXEC   2u
#define ET_DYN    3u
#define PF_X      1u
#define PF_W      2u
#define PF_R      4u

typedef struct {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} Elf64Hdr;

typedef struct {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} Elf64Ph;

/* ---- auxv 常量 ---- */
#define AT_NULL        0ull
#define AT_PHDR        3ull
#define AT_PHENT       4ull
#define AT_PHNUM       5ull
#define AT_PAGESZ      6ull
#define AT_BASE        7ull
#define AT_FLAGS       8ull
#define AT_ENTRY       9ull
#define AT_UID        11ull
#define AT_EUID       12ull
#define AT_GID        13ull
#define AT_EGID       14ull
#define AT_PLATFORM   15ull
#define AT_SECURE     23ull
#define AT_RANDOM     25ull
#define AT_EXECFN     31ull
#define AT_SYSINFO_EHDR 33ull

#define NAUX 16ull         /* 下面实际压入的 auxv 对数（不含结尾的 AT_NULL） */

#define STACK_PAGES 32ull
/* 栈顶用于 argv/envp/auxv 的窗口。以前是 4KB，等于"命令行和环境变量加起来
 * 不能超过 4KB"——一条长一点的 `sh -c` 或 JAVA_TOOL_OPTIONS 就装不下。
 * 现在给 64KB，缓冲本身走内核堆（内核栈只有 8KB，放不下）。 */
#define STACK_BUF   65536ull
#define INTERP_MAX  128ull

static u64 g_exec_entry;
static u64 g_exec_rsp;
static const char *g_arg1;      /* 可选的 argv[1]（busybox ls / 这类命令用） */

static u64 slen(const char *s)
{
    u64 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static u64 push_str(u8 *b, u64 *w, u64 va_base, const char *s)
{
    u64 n = slen(s) + 1ull;
    u64 pad = (n + 7ull) & ~7ull;
    u64 i;

    if (*w < pad) {                 /* 参数区放不下：置 0 让调用者判失败 */
        *w = 0ull;
        return 0ull;
    }
    *w -= pad;
    for (i = 0; i < pad; i++) {
        b[*w + i] = 0;
    }
    for (i = 0; i + 1ull < n; i++) {
        b[*w + i] = (u8)s[i];
    }
    return va_base + *w;
}

static void push8(u8 *b, u64 *w, u64 v)
{
    u64 i;

    if (*w < 8ull) {
        *w = 0ull;
        return;
    }
    *w -= 8ull;
    for (i = 0; i < 8ull; i++) {
        b[*w + i] = (u8)((v >> (8ull * i)) & 0xffull);
    }
}

/* auxv 的一对 (type, value)：栈向低地址长，所以先压 value 再压 type，
 * 内存里才是升序的 [type][value]（Linux 侧就是这么读的）。 */
#define PUSH_AUX(t, v) do { push8(buf, &w, (v)); push8(buf, &w, (t)); } while (0)

/* 映像需要多少字节（= max(p_offset + p_filesz)）。内核据此决定从磁盘读多少。 */
u64 lx_elf_image_size(u64 img_pa, u64 max_bytes)
{
    const u8 *img = (const u8 *)port_phys_map(img_pa);
    const Elf64Hdr *h = (const Elf64Hdr *)img;
    u64 need = sizeof(Elf64Hdr);
    u16 i;

    if (h->ident[0] != 0x7fu || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F') {
        return 0ull;
    }
    if (h->phentsize != sizeof(Elf64Ph)) {
        return 0ull;
    }
    if (h->phoff + (u64)h->phnum * sizeof(Elf64Ph) > max_bytes) {
        return 0ull;
    }
    if (h->phoff + (u64)h->phnum * sizeof(Elf64Ph) > need) {
        need = h->phoff + (u64)h->phnum * sizeof(Elf64Ph);
    }
    for (i = 0; i < h->phnum; i++) {
        const Elf64Ph *ph = (const Elf64Ph *)(img + h->phoff + (u64)i * sizeof(Elf64Ph));

        if (ph->offset + ph->filesz > need) {
            need = ph->offset + ph->filesz;
        }
    }
    if (need > max_bytes) {
        return 0ull;
    }
    return need;
}

/* base 只对 ET_DYN 生效；ET_EXEC 的 vaddr 就是绝对地址。 */
int lx_elf_load(u64 as, u64 img_pa, u64 img_size, u64 base,
                u64 *o_entry, u64 *o_phdr, u64 *o_phnum, u64 *o_phentsize,
                u64 *o_brk_end)
{
    const u8 *img = (const u8 *)port_phys_map(img_pa);
    const Elf64Hdr *h = (const Elf64Hdr *)img;
    u64 brk = 0ull;
    u64 phdr = 0ull;
    u16 i;

    if (img_size < sizeof(Elf64Hdr)) {
        return -1;
    }
    if (h->ident[0] != 0x7fu || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F') {
        return -2;
    }
    if (h->ident[4] != 2u || h->ident[5] != 1u) {
        return -3;
    }
    if (h->type == ET_EXEC) {
        base = 0ull;
    } else if (h->type != ET_DYN) {
        return -4;
    }
    if ((u64)h->phentsize != sizeof(Elf64Ph)) {
        return -5;
    }
    if (h->phoff + (u64)h->phnum * sizeof(Elf64Ph) > img_size) {
        return -6;
    }

    for (i = 0; i < h->phnum; i++) {
        const Elf64Ph *ph = (const Elf64Ph *)(img + h->phoff + (u64)i * sizeof(Elf64Ph));
        u64 pv;
        u64 va0;
        u64 va1;
        u64 va;

        if (ph->type != PT_LOAD || ph->memsz == 0ull) {
            continue;
        }
        if (ph->offset + ph->filesz > img_size) {
            return -7;
        }
        pv = ph->vaddr + base;
        if (ph->offset <= h->phoff && h->phoff < ph->offset + ph->filesz) {
            phdr = pv + (h->phoff - ph->offset);
        }

        va0 = pv & ~(PAGE - 1ull);
        va1 = (pv + ph->memsz + PAGE - 1ull) & ~(PAGE - 1ull);
        for (va = va0; va < va1; va += PAGE) {
            u64 pa = port_phys_alloc();
            u64 s;
            u64 e;
            u32 prot = 0u;
            u8 *page;

            if (pa == 0ull) {
                return -8;
            }
            page = (u8 *)port_phys_map(pa);
            for (s = 0; s < PAGE; s++) {
                page[s] = 0;
            }
            s = (va > pv) ? va : pv;
            e = (va + PAGE < pv + ph->filesz) ? (va + PAGE) : (pv + ph->filesz);
            if (e > s) {
                const u8 *src = img + ph->offset + (s - pv);
                u8 *dst = page + (s - va);
                u64 n = e - s;
                u64 k;

                for (k = 0; k < n; k++) {
                    dst[k] = src[k];
                }
            }
            if ((ph->flags & PF_R) != 0u) {
                prot |= C6OS_PROT_R;
            }
            if ((ph->flags & PF_W) != 0u) {
                prot |= C6OS_PROT_W;
            }
            if ((ph->flags & PF_X) != 0u) {
                prot |= C6OS_PROT_X;
            }
            if (port_as_map(as, va, pa, PAGE, prot) != 0) {
                return -9;
            }
            /* 记账：mprotect/munmap 要靠它判断"这段是什么"。相邻页属性相同
             * 会被 lx_vma_add 合并成一条，不会一页一个节点。 */
            if (lx_vma_add(as, va, va + PAGE, prot, 0u, 0ull) != 0) {
                return -11;
            }
        }
        if (va1 > brk) {
            brk = va1;
        }
    }

    if (phdr == 0ull || brk == 0ull) {
        return -10;
    }
    *o_entry = h->entry + base;
    *o_phdr = phdr;
    *o_phnum = (u64)h->phnum;
    *o_phentsize = (u64)h->phentsize;
    *o_brk_end = brk;
    return 0;
}

/* 建初始栈：把 argv/envp 字符串、指针数组、auxv 按 Linux 的布局摆到栈顶窗口里。
 *
 * M14-1 的三处改动：
 *   1. 窗口从 4KB 拉到 64KB，缓冲从内核栈搬进内核堆（内核栈只有 8KB）；
 *   2. envp 真的压栈（以前是 `(void)uenvp`，等于所有子进程都没有环境变量）；
 *   3. 指针数组本身也从堆里来，argc/envc 只受 LX_ARG_MAX 限制，不再是 16 项。 */
static int build_stack(u64 as, const char *path, const char *const *argv,
                       u32 argc, const char *const *envp, u32 envc,
                       u64 run_entry, u64 at_entry,
                       u64 at_base, u64 phdr, u64 phnum, u64 phentsize,
                       u64 *out_rsp)
{
    u64 top = C6OS_LINUX_STACK_TOP;
    u64 base = top - STACK_PAGES * PAGE;
    u64 frames[STACK_PAGES];
    u8 *buf;
    u64 *va_argv;
    u64 *va_envp;
    u64 va_base = top - STACK_BUF;
    u64 w = STACK_BUF;
    u64 va_argv0;
    u64 va_plat;
    u64 va_execfn;
    u64 va_rand;
    u64 nargv;
    u64 nenv;
    u64 fixed;
    u64 i;
    int ret = -1;
    u8 *pg;

    buf = (u8 *)port_kmalloc(STACK_BUF);
    va_argv = (u64 *)port_kcalloc((u64)LX_ARG_MAX + 1ull, 8ull);
    va_envp = (u64 *)port_kcalloc((u64)LX_ARG_MAX + 1ull, 8ull);
    if (buf == (u8 *)0 || va_argv == (u64 *)0 || va_envp == (u64 *)0) {
        ret = -5;
        goto out;
    }

    for (i = 0; i < STACK_PAGES; i++) {
        u64 pa = port_phys_alloc();
        u64 k;

        if (pa == 0ull) {
            ret = -1;
            goto out;
        }
        frames[i] = pa;
        pg = (u8 *)port_phys_map(pa);
        for (k = 0; k < PAGE; k++) {
            pg[k] = 0;
        }
        if (port_as_map(as, base + i * PAGE, pa, PAGE, C6OS_PROT_R | C6OS_PROT_W) != 0) {
            ret = -2;
            goto out;
        }
    }
    /* 栈 VMA：页现在是预映射的；LX_VM_STACK 是为了让缺页路径知道"落在栈
     * 下方的地址是合法增长"，不是野指针（M13-2b-3 起按需扩栈）。 */
    if (lx_vma_add(as, base, top, C6OS_PROT_R | C6OS_PROT_W,
                   LX_VM_STACK | LX_VM_ANON, 0ull) != 0) {
        ret = -3;
        goto out;
    }

    if (argc == 0u || argv == (const char *const *)0) {
        argc = 1u;
    }
    if (argc > LX_ARG_MAX) {
        argc = LX_ARG_MAX;
    }
    nargv = (u64)argc;
    for (i = 0; i < nargv; i++) {
        const char *a = argv[i];

        if (a == (const char *)0) {
            a = path;
        }
        va_argv[i] = push_str(buf, &w, va_base, a);
    }
    if (envp == (const char *const *)0) {
        envc = 0u;
    }
    if (envc > LX_ARG_MAX) {
        envc = LX_ARG_MAX;
    }
    nenv = (u64)envc;
    for (i = 0; i < nenv; i++) {
        const char *e = envp[i];

        va_envp[i] = push_str(buf, &w, va_base, (e != (const char *)0) ? e : "");
    }
    if (w == 0ull) {
        ret = -6;                    /* 字符串区放不下 */
        goto out;
    }
    va_argv0 = va_argv[0];
    va_plat = push_str(buf, &w, va_base, "x86_64");
    va_execfn = push_str(buf, &w, va_base, path);
    if (w < 16ull) {
        ret = -6;
        goto out;
    }
    w -= 16ull;
    va_rand = va_base + w;
    for (i = 0; i < 16ull; i++) {
        buf[w + i] = (u8)(0xa5u + (u8)i * 7u);
    }

    /* 固定部分：argc + argv[] + argv NULL + envp[] + envp NULL + auxv。
     * 若落不到 16 字节边界，只在**字符串与 auxv 之间**垫 8 字节：
     * 垫在 envp 与 auxv 之间会被 glibc 当成 AT_NULL，auxv 整个读空。 */
    fixed = 8ull * (3ull + nargv + nenv);   /* argc + argv[]+NULL + envp[]+NULL */
    if (w < fixed + 16ull * (NAUX + 1ull) + 8ull) {
        ret = -6;
        goto out;
    }
    if (((va_base + w - (fixed + 16ull * (NAUX + 1ull))) & 15ull) != 0ull) {
        push8(buf, &w, 0ull);
    }

    PUSH_AUX(AT_NULL, 0ull);
    PUSH_AUX(AT_SYSINFO_EHDR, 0ull);                 /* 无 vDSO */
    PUSH_AUX(AT_EXECFN, va_execfn);
    PUSH_AUX(AT_RANDOM, va_rand);
    PUSH_AUX(AT_PLATFORM, va_plat);
    PUSH_AUX(AT_SECURE, 0ull);
    PUSH_AUX(AT_EGID, 0ull);
    PUSH_AUX(AT_GID, 0ull);
    PUSH_AUX(AT_EUID, 0ull);
    PUSH_AUX(AT_UID, 0ull);
    PUSH_AUX(AT_ENTRY, at_entry);                    /* 主程序入口（不是 ld.so） */
    PUSH_AUX(AT_BASE, at_base);                      /* ld.so 装载基址；静态为 0 */
    PUSH_AUX(AT_FLAGS, 0ull);
    PUSH_AUX(AT_PAGESZ, PAGE);
    PUSH_AUX(AT_PHENT, phentsize);
    PUSH_AUX(AT_PHNUM, phnum);
    PUSH_AUX(AT_PHDR, phdr);                         /* 主程序的 phdr */

    push8(buf, &w, 0ull);                      /* envp 结尾 */
    i = nenv;
    while (i > 0ull) {
        i--;
        push8(buf, &w, va_envp[i]);            /* envp[n-1] .. envp[0] */
    }
    push8(buf, &w, 0ull);                      /* argv 结尾 */
    i = nargv;
    while (i > 1ull) {
        i--;
        push8(buf, &w, va_argv[i]);            /* argv[n-1] .. argv[1] */
    }
    push8(buf, &w, va_argv0);                  /* argv[0] */
    push8(buf, &w, nargv);                     /* argc */
    if (w == 0ull) {
        ret = -6;
        goto out;
    }

    /* 窗口跨多页：按虚拟地址逐页拷，不能只往最后一页写。 */
    for (i = w; i < STACK_BUF; i++) {
        u64 va = va_base + i;
        u64 pi = (va - base) / PAGE;
        u64 po = (va - base) % PAGE;

        pg = (u8 *)port_phys_map(frames[pi]);
        pg[po] = buf[i];
    }

    *out_rsp = va_base + w;
    ret = 0;
out:
    if (buf != (u8 *)0) {
        port_kfree((void *)buf);
    }
    if (va_argv != (u64 *)0) {
        port_kfree((void *)va_argv);
    }
    if (va_envp != (u64 *)0) {
        port_kfree((void *)va_envp);
    }
    return ret;
}

/* 读 PT_INTERP（解释器路径）到内核缓冲。返回长度，0 表示静态链接。 */
static u64 elf_interp(u64 img_pa, u64 img_size, char *out, u64 cap)
{
    const u8 *img = (const u8 *)port_phys_map(img_pa);
    const Elf64Hdr *h = (const Elf64Hdr *)img;
    u16 i;

    if (img_size < sizeof(Elf64Hdr)) {
        return 0ull;
    }
    if (h->phoff + (u64)h->phnum * sizeof(Elf64Ph) > img_size) {
        return 0ull;
    }
    for (i = 0; i < h->phnum; i++) {
        const Elf64Ph *ph = (const Elf64Ph *)(img + h->phoff + (u64)i * sizeof(Elf64Ph));
        u64 k;

        if (ph->type != PT_INTERP) {
            continue;
        }
        if (ph->filesz == 0ull || ph->filesz > cap ||
            ph->offset + ph->filesz > img_size) {
            return 0ull;
        }
        for (k = 0; k < ph->filesz; k++) {
            out[k] = (char)img[ph->offset + k];
        }
        out[ph->filesz - 1ull] = '\0';
        return ph->filesz;
    }
    return 0ull;
}

int lx_exec_build(u64 as, u64 img_pa, u64 img_size, const char *path,
                  const char *const *argv, u32 argc,
                  const char *const *envp, u32 envc,
                  u64 *o_entry, u64 *o_rsp, u64 *o_brk)
{
    u64 e = 0ull;
    u64 ph = 0ull;
    u64 pn = 0ull;
    u64 pe = 0ull;
    u64 brk = 0ull;
    u64 ie = 0ull;
    u64 iph = 0ull;
    u64 ipn = 0ull;
    u64 ipe = 0ull;
    u64 ibrk = 0ull;
    char ipath[INTERP_MAX];
    u64 ilen;
    u64 ibase = 0ull;

    if (lx_elf_load(as, img_pa, img_size, C6OS_LINUX_ELF_BASE,
                    &e, &ph, &pn, &pe, &brk) != 0) {
        return -1;
    }

    ilen = elf_interp(img_pa, img_size, ipath, INTERP_MAX);
    if (ilen != 0ull) {
        u64 isz = lx_fs_size_of(ipath);
        u64 ipa = lx_fs_pa_of(ipath);

        if (isz == 0ull || ipa == 0ull) {
            port_puts("elf: FAIL 解释器不在 initramfs 里\n");
            return -2;
        }
        ibase = C6OS_LINUX_INTERP_BASE;
        if (lx_elf_load(as, ipa, isz, ibase, &ie, &iph, &ipn, &ipe, &ibrk) != 0) {
            port_puts("elf: FAIL 解释器装载失败\n");
            return -3;
        }
    }

    if (build_stack(as, path, argv, argc, envp, envc, (ibase != 0ull) ? ie : e,
                    e, ibase, ph, pn, pe, o_rsp) != 0) {
        return -4;
    }
    *o_entry = (ibase != 0ull) ? ie : e;
    if (o_brk != (u64 *)0) {
        *o_brk = brk;
    }
    return 0;
}

/* 首次装载（L2..L4 以及 L5 的 0 号进程）：装载 + 登记进程 + 重置 fd 表。 */
int lx_exec_prepare(u64 as, u64 img_pa, u64 img_size, const char *path)
{
    const char *argv0[2];
    u64 entry = 0ull;
    u64 rsp = 0ull;
    u64 brk = 0ull;

    argv0[0] = path;
    argv0[1] = g_arg1;
    if (lx_exec_build(as, img_pa, img_size, path, argv0,
                      (g_arg1 != (const char *)0) ? 2u : 1u,
                      (const char *const *)0, 0u,
                      &entry, &rsp, &brk) != 0) {
        return -2;
    }
    lx_proc_init(as, brk);                 /* 登记 0 号进程，brk 来自新映像 */
    lx_fs_fd_reset();
    g_exec_entry = entry;
    g_exec_rsp = rsp;
    return 0;
}

/* M12：完整 argv 版本。让 init 能直接跑 `sh -c '...'` —— 交互式 shell 要等
 * 串口输入，在自动化验收里没法复现；带 -c 的固定脚本则完全确定。
 * 必须在 lx_exec_prepare 之前调（它内部会重置 fd 表、登记 0 号进程）。 */
int lx_exec_preparev(u64 as, u64 img_pa, u64 img_size, const char *path,
                     const char *const *argv, u32 argc)
{
    u64 entry = 0ull;
    u64 rsp = 0ull;
    u64 brk = 0ull;

    if (argc == 0u || argv == (const char *const *)0) {
        return -1;
    }
    if (lx_exec_build(as, img_pa, img_size, path, argv, argc,
                      (const char *const *)0, 0u,
                      &entry, &rsp, &brk) != 0) {
        return -2;
    }
    lx_proc_init(as, brk);
    lx_fs_fd_reset();
    g_exec_entry = entry;
    g_exec_rsp = rsp;
    return 0;
}

u64 lx_exec_entry(void)
{
    return g_exec_entry;
}
u64 lx_exec_rsp(void)
{
    return g_exec_rsp;
}

/* 给用户程序塞一个 argv[1]（例如 busybox 的 "ls"）。必须在 lx_exec_prepare 前调用。 */
void lx_exec_arg1(const char *a)
{
    g_arg1 = a;
}
