#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"

/* ELF64 静态加载器（L2）。
 * 只处理 ET_EXEC（静态链接的非 PIE）。PIE/动态链接在 L3/L4。 */

#define PAGE      4096ull
#define PT_LOAD   1u
#define ET_EXEC   2u
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

int lx_elf_load(u64 as, u64 img_pa, u64 img_size,
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
    if (h->type != ET_EXEC) {
        return -4;                        /* 静态非 PIE；PIE 留给 L3 */
    }
    if ((u64)h->phentsize != sizeof(Elf64Ph)) {
        return -5;
    }
    if (h->phoff + (u64)h->phnum * sizeof(Elf64Ph) > img_size) {
        return -6;
    }

    for (i = 0; i < h->phnum; i++) {
        const Elf64Ph *ph = (const Elf64Ph *)(img + h->phoff + (u64)i * sizeof(Elf64Ph));
        u64 va0;
        u64 va1;
        u64 va;

        if (ph->type != PT_LOAD || ph->memsz == 0ull) {
            continue;
        }
        if (ph->offset + ph->filesz > img_size) {
            return -7;
        }
        if (ph->offset <= h->phoff && h->phoff < ph->offset + ph->filesz) {
            phdr = ph->vaddr + (h->phoff - ph->offset);
        }

        va0 = ph->vaddr & ~(PAGE - 1ull);
        va1 = (ph->vaddr + ph->memsz + PAGE - 1ull) & ~(PAGE - 1ull);
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
            s = (va > ph->vaddr) ? va : ph->vaddr;
            e = (va + PAGE < ph->vaddr + ph->filesz) ? (va + PAGE) : (ph->vaddr + ph->filesz);
            if (e > s) {
                const u8 *src = img + ph->offset + (s - ph->vaddr);
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
    *o_entry = h->entry;
    *o_phdr = phdr;
    *o_phnum = (u64)h->phnum;
    *o_phentsize = (u64)h->phentsize;
    *o_brk_end = brk;
    return 0;
}

int lx_build_stack(u64 as, u64 entry, u64 phdr, u64 phnum, u64 phentsize,
                   u64 *out_rsp, u64 *out_base)
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

    va_argv0 = push_str(buf, &w, va_base, "/hello");
    va_plat = push_str(buf, &w, va_base, "x86_64");
    va_execfn = push_str(buf, &w, va_base, "/hello");
    w -= 16ull;
    va_rand = va_base + w;
    for (i = 0; i < 16ull; i++) {
        buf[w + i] = (u8)(0xa5u + (u8)i * 7u);
    }

    /* 固定部分：envp 结尾(8) + argv 结尾(8) + argv[0](8) + argc(8) + auxv。
     * 若 argc 落不到 16 字节边界，只在**字符串与 auxv 之间**垫 8 字节：
     * 垫在 envp 与 auxv 之间会被 glibc 当成 AT_NULL，auxv 整个读空。 */
    if (((va_base + w - (32ull + 16ull * (NAUX + 1ull))) & 15ull) != 0ull) {
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
    PUSH_AUX(AT_ENTRY, entry);
    PUSH_AUX(AT_BASE, 0ull);
    PUSH_AUX(AT_FLAGS, 0ull);
    PUSH_AUX(AT_PAGESZ, PAGE);
    PUSH_AUX(AT_PHENT, phentsize);
    PUSH_AUX(AT_PHNUM, phnum);
    PUSH_AUX(AT_PHDR, phdr);

    push8(buf, &w, 0ull);                      /* envp 结尾 */
    push8(buf, &w, 0ull);                      /* argv 结尾 */
    push8(buf, &w, va_argv0);                  /* argv[0] */
    push8(buf, &w, 1ull);                      /* argc */

    pg = (u8 *)port_phys_map(frames[STACK_PAGES - 1ull]);
    for (i = 0; i + w < STACK_BUF; i++) {
        pg[w + i] = buf[w + i];
    }

    *out_rsp = va_base + w;
    *out_base = base;
    return 0;
}
