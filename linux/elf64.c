#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"

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
#define STACK_BUF   4096ull
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

static int build_stack(u64 as, const char *path, const char *const *argv,
                       u32 argc, u64 run_entry, u64 at_entry,
                       u64 at_base, u64 phdr, u64 phnum, u64 phentsize,
                       u64 *out_rsp)
{
    u64 top = C6OS_LINUX_STACK_TOP;
    u64 base = top - STACK_PAGES * PAGE;
    u64 frames[STACK_PAGES];
    u8 buf[STACK_BUF];
    u64 va_base = top - STACK_BUF;
    u64 w = STACK_BUF;
    u64 va_argv0;
    u64 va_plat;
    u64 va_execfn;
    u64 va_rand;
    u64 va_arg1;
    u64 va_argv[16];
    u64 nargv;
    u64 fixed;
    u64 i;
    u8 *pg;

    for (i = 0; i < STACK_PAGES; i++) {
        u64 pa = port_phys_alloc();
        u64 k;

        if (pa == 0ull) {
            return -1;
        }
        frames[i] = pa;
        pg = (u8 *)port_phys_map(pa);
        for (k = 0; k < PAGE; k++) {
            pg[k] = 0;
        }
        if (port_as_map(as, base + i * PAGE, pa, PAGE, C6OS_PROT_R | C6OS_PROT_W) != 0) {
            return -2;
        }
    }

    if (argc == 0u || argv == (const char *const *)0) {
        argc = 1u;
    }
    if (argc > 16u) {
        argc = 16u;
    }
    nargv = (u64)argc;
    for (i = 0; i < nargv; i++) {
        const char *a = argv[i];

        if (a == (const char *)0) {
            a = path;
        }
        va_argv[i] = push_str(buf, &w, va_base, a);
    }
    va_argv0 = va_argv[0];
    va_arg1 = (nargv > 1ull) ? va_argv[1] : 0ull;
    va_plat = push_str(buf, &w, va_base, "x86_64");
    va_execfn = push_str(buf, &w, va_base, path);
    w -= 16ull;
    va_rand = va_base + w;
    for (i = 0; i < 16ull; i++) {
        buf[w + i] = (u8)(0xa5u + (u8)i * 7u);
    }

    /* 固定部分：envp 结尾(8) + argv 结尾(8) + argv[0](8) + argc(8) + auxv。
     * 若 argc 落不到 16 字节边界，只在**字符串与 auxv 之间**垫 8 字节：
     * 垫在 envp 与 auxv 之间会被 glibc 当成 AT_NULL，auxv 整个读空。 */
    fixed = 8ull * (3ull + nargv);      /* argc + argv[] + argv NULL + envp NULL */
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
    push8(buf, &w, 0ull);                      /* argv 结尾 */
    i = nargv;
    while (i > 1ull) {
        i--;
        push8(buf, &w, va_argv[i]);            /* argv[n-1] .. argv[1] */
    }
    push8(buf, &w, va_argv0);                  /* argv[0] */
    push8(buf, &w, nargv);                     /* argc */

    pg = (u8 *)port_phys_map(frames[STACK_PAGES - 1ull]);
    for (i = 0; i + w < STACK_BUF; i++) {
        pg[w + i] = buf[w + i];
    }

    *out_rsp = va_base + w;
    return 0;
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

    if (build_stack(as, path, argv, argc, (ibase != 0ull) ? ie : e, e, ibase,
                    ph, pn, pe, o_rsp) != 0) {
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
