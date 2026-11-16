#include "c6os/types.h"
#include "c6os/port.h"

/* x86-64 MMU：物理帧分配器 + 4 级页表 + 内核直接映射窗口。
 *
 * 地址空间布局（docs/linux-abi.md §3）：
 *   PML4[0]   : 低半区。0..4MB = 内核（identity, supervisor），
 *               4MB..1GB = 用户区（4KB 页，按需建表）
 *   PML4[256] : 直接映射窗口 0xFFFF8000_00000000 → 物理 0..1GB（2MB 页, NX）
 *
 * 为什么需要直接映射窗口：页表自身也住在物理页里，内核必须能读写它们；
 * 而用户区会占用 4MB 以上的虚拟地址，所以不能再靠 identity 访问物理内存。
 * 直接把物理内存挂到 PML4[256]（Linux 用同样的位置）是代价最小的做法。
 */

#define PAGE_SZ   4096ull
#define PTE_P     0x001ull
#define PTE_W     0x002ull
#define PTE_U     0x004ull
#define PTE_PS    0x080ull
#define PTE_PWT   0x008ull
#define PTE_PCD   0x010ull
#define PTE_NX    (1ull << 63)
#define PTE_ADDR  0x000ffffffffff000ull

#define DMAP_BASE 0xffff800000000000ull

#define IDX4(va) (((va) >> 39) & 0x1ffull)
#define IDX3(va) (((va) >> 30) & 0x1ffull)
#define IDX2(va) (((va) >> 21) & 0x1ffull)
#define IDX1(va) (((va) >> 12) & 0x1ffull)

/* boot16.S 收集的 E820 表：count@0x5000，条目@0x5010（24B/条）。 */
#define E820_COUNT (*(volatile u32 *)0x5000u)
#define E820_ENTRY(pa, i) (*(volatile u64 *)((pa) + (u64)(i) * 24ull))
#define E820_TABLE 0x5010u

/* ---- 物理帧分配器 ------------------------------------------------------ */

#define PMM_MAX_FRAMES 262144ull          /* 1GB / 4KB */
static u8  g_bmp[PMM_MAX_FRAMES / 8u];    /* 32KB，1 = 已占用 */
static u64 g_frames;                      /* 已纳入管理的帧数上界 */
static u64 g_free;
static u64 g_rover = 4ull;                /* 从 4MB 起分配 */

static int bmp_get(u64 f) { return (int)((g_bmp[f >> 3] >> (u8)(f & 7ull)) & 1u); }
static void bmp_set(u64 f) { g_bmp[f >> 3] = (u8)(g_bmp[f >> 3] | (u8)(1u << (u8)(f & 7ull))); }
static void bmp_clr(u64 f) { g_bmp[f >> 3] = (u8)(g_bmp[f >> 3] & (u8)~(u8)(1u << (u8)(f & 7ull))); }

#define KERNEL_RESERVED_END 0x400000ull   /* 内核镜像（含 .bss 与引导栈）区间上界 */

static void pmm_init(void)
{
    u32 n = E820_COUNT;
    u32 i;
    u64 k;

    if (n > 32u) {
        n = 32u;
    }
    for (k = 0; k < PMM_MAX_FRAMES / 8ull; k++) {
        g_bmp[k] = 0xffu;                 /* 先全保留，只放开 E820 的 usable */
    }
    g_free = 0;
    g_frames = 0;

    for (i = 0; i < n; i++) {
        u64 base = E820_ENTRY(E820_TABLE, i);
        u64 len  = E820_ENTRY(8u + E820_TABLE, i);
        u32 type = (u32)E820_ENTRY(16u + E820_TABLE, i);
        u64 s;
        u64 e;

        if (type != 1u) {
            continue;                     /* 只认 ACPI usable */
        }
        s = (base + PAGE_SZ - 1ull) & ~(PAGE_SZ - 1ull);
        e = base + len;
        if (e > 0x40000000ull) {
            e = 0x40000000ull;            /* 直接映射窗口只覆盖 1GB */
        }
        e &= ~(PAGE_SZ - 1ull);
        while (s + PAGE_SZ <= e) {
            u64 f = s >> 12;
            if (f >= 4ull && f < PMM_MAX_FRAMES && bmp_get(f)) {
                bmp_clr(f);
                g_free++;
            }
            s += PAGE_SZ;
        }
    }
    /* 内核自己就住在低端：boot16 装到 0x10000，链接在 0x20000，.bss 到 ~0x3d000。
     * 这段在 E820 里是 usable，必须整块扣掉 —— 否则分配器跑到 0x20000 之后
     * 会把内核代码/数据当作页表页清零（L2 一次映射上百页时必然踩到）。 */
    for (k = 4ull; k < (KERNEL_RESERVED_END >> 12); k++) {
        if (!bmp_get(k)) {
            bmp_set(k);
            g_free--;
        }
    }

    /* 管理上界取最高的可用帧（用于 free_kb 报表） */
    for (k = PMM_MAX_FRAMES; k > 0ull; k--) {
        if (!bmp_get(k - 1ull) || k == 4ull) {
            g_frames = k;
            break;
        }
    }
}

u64 port_phys_alloc(void)
{
    u64 f = g_rover;
    u64 scanned = 0;

    while (scanned < g_frames) {
        if (f >= g_frames) {
            f = 4ull;
        }
        if (!bmp_get(f)) {
            bmp_set(f);
            g_free--;
            g_rover = f + 1ull;
            return f << 12;
        }
        f++;
        scanned++;
    }
    return 0;
}

void port_phys_free(u64 pa)
{
    u64 f = pa >> 12;

    if (f >= 4ull && f < PMM_MAX_FRAMES && bmp_get(f)) {
        bmp_clr(f);
        g_free++;
    }
}

void port_phys_reserve(u64 pa, u64 bytes)
{
    u64 f = pa >> 12;
    u64 n = (bytes + PAGE_SZ - 1ull) >> 12;
    u64 i;

    for (i = 0; i < n; i++) {
        u64 idx = f + i;

        if (idx < 4ull || idx >= PMM_MAX_FRAMES) {
            continue;
        }
        if (!bmp_get(idx)) {
            bmp_set(idx);
            g_free--;
        }
    }
}

u64 port_phys_total_kb(void) { return (g_frames * PAGE_SZ) / 1024ull; }
u64 port_phys_free_kb(void)  { return (g_free * PAGE_SZ) / 1024ull; }

/* ---- 页表 -------------------------------------------------------------- */

static int g_dmap_ready;
static u64 g_dmap_pdpt;                   /* 所有地址空间共享的直接映射 PDPT */
static u64 g_kernel_as;

/* 页表内存的访问方式：激活内核地址空间前用 identity，之后用直接映射窗口。 */
static void *ptab(u64 pa)
{
    return g_dmap_ready ? (void *)(pa | DMAP_BASE) : (void *)(u64)pa;
}

void *port_phys_map(u64 pa)
{
    return (void *)(pa | DMAP_BASE);
}

static u64 alloc_frame_zero(void)
{
    u64 pa = port_phys_alloc();
    u64 *p;
    u32 i;

    if (pa == 0ull) {
        return 0ull;
    }
    p = (u64 *)ptab(pa);
    for (i = 0; i < 512u; i++) {
        p[i] = 0ull;
    }
    return pa;
}

static u64 *tbl(u64 entry)
{
    return (u64 *)ptab(entry & PTE_ADDR);
}

/* 建 4KB 用户页；中间层一律 U|W（叶子决定实际权限）。 */
static int map4k(u64 pml4_pa, u64 va, u64 pa, u64 leaf)
{
    u64 *p4 = tbl(pml4_pa);
    u64 *p3;
    u64 *p2;
    u64 *p1;
    u64 *slot;

    slot = &p4[IDX4(va)];
    if ((*slot & PTE_P) == 0ull) {
        u64 f = alloc_frame_zero();
        if (f == 0ull) {
            return -1;
        }
        *slot = f | PTE_P | PTE_W | PTE_U;
    }
    p3 = tbl(*slot);

    slot = &p3[IDX3(va)];
    if ((*slot & PTE_P) == 0ull) {
        u64 f = alloc_frame_zero();
        if (f == 0ull) {
            return -1;
        }
        *slot = f | PTE_P | PTE_W | PTE_U;
    }
    p2 = tbl(*slot);

    slot = &p2[IDX2(va)];
    if ((*slot & PTE_PS) != 0ull) {
        return -2;                        /* 已被 2MB 页占用（内核区） */
    }
    if ((*slot & PTE_P) == 0ull) {
        u64 f = alloc_frame_zero();
        if (f == 0ull) {
            return -1;
        }
        *slot = f | PTE_P | PTE_W | PTE_U;
    }
    p1 = tbl(*slot);
    p1[IDX1(va)] = (pa & PTE_ADDR) | PTE_P | leaf;
    return 0;
}

static void invlpg(u64 va)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
}

static void enable_nxe(void)
{
    u32 lo;
    u32 hi;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080u));
    lo |= 0x800u;                         /* EFER.NXE */
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(0xc0000080u));
}

void port_as_activate(u64 as)
{
    __asm__ __volatile__("movq %0, %%cr3" : : "r"(as) : "memory");
}

/* 内核低端：0..4MB identity、supervisor；再挂上共享的直接映射窗口。 */
static u64 build_as(void)
{
    u64 pml4 = alloc_frame_zero();
    u64 pdpt = alloc_frame_zero();
    u64 pd = alloc_frame_zero();
    u64 *p4;
    u64 *p3;
    u64 *p2;
    u32 i;

    if (pml4 == 0ull || pdpt == 0ull || pd == 0ull) {
        return 0ull;
    }
    p4 = tbl(pml4);
    p4[0] = pdpt | PTE_P | PTE_W | PTE_U;
    p4[256] = g_dmap_pdpt | PTE_P | PTE_W;
    p3 = tbl(pdpt);
    p3[0] = pd | PTE_P | PTE_W | PTE_U;
    p2 = tbl(pd);
    for (i = 0; i < 2u; i++) {
        p2[i] = ((u64)i * 0x200000ull) | PTE_P | PTE_W | PTE_PS;   /* supervisor */
    }
    return pml4;
}

/* 直接映射窗口：物理 0..4GB 出现在 0xFFFF8000_00000000，NX。
 * 0..1GB 是普通内存（cache 打开）；1..4GB 是 PCI/MMIO 空洞，用 2MB 页
 * 按 UC（PCD|PWT）映射——e1000 的 BAR 落在 0xFExxxxxx，就是靠这一段访问的。 */
static u64 build_dmap(void)
{
    u64 pdpt = alloc_frame_zero();
    u64 pd = alloc_frame_zero();
    u64 *p3;
    u64 *p2;
    u32 i;
    u32 gb;

    if (pdpt == 0ull || pd == 0ull) {
        return 0ull;
    }
    p3 = tbl(pdpt);
    p3[0] = pd | PTE_P | PTE_W;
    p2 = tbl(pd);
    for (i = 0; i < 512u; i++) {
        p2[i] = ((u64)i * 0x200000ull) | PTE_P | PTE_W | PTE_PS | PTE_NX;
    }
    for (gb = 1u; gb < 4u; gb++) {
        u64 pdg = alloc_frame_zero();
        u64 base;
        u64 *p2g;

        if (pdg == 0ull) {
            break;
        }
        base = (u64)gb * 0x40000000ull;
        p3[gb] = pdg | PTE_P | PTE_W;
        p2g = tbl(pdg);
        for (i = 0; i < 512u; i++) {
            p2g[i] = (base + ((u64)i * 0x200000ull)) |
                     PTE_P | PTE_W | PTE_PS | PTE_NX | PTE_PCD | PTE_PWT;
        }
    }
    g_dmap_pdpt = pdpt;
    return pdpt;
}

static void put_hex(u32 v)
{
    const char *d = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        port_putc((int)d[(v >> (u32)i) & 0xfu]);
    }
}

void port_mmu_init(void)
{
    enable_nxe();
    pmm_init();
    port_puts("mmu: e820=");
    put_hex(E820_COUNT);
    port_puts(" free_kb=");
    put_hex((u32)((g_free * PAGE_SZ) / 1024ull));
    port_puts("\n");
    if (build_dmap() == 0ull) {
        port_puts("mmu: FAIL dmap\n");
        return;
    }
    g_kernel_as = build_as();             /* 此时还在 stage1 的 identity 映射上 */
    if (g_kernel_as == 0ull) {
        port_puts("mmu: FAIL kernel as\n");
        return;
    }
    port_puts("mmu: cr3=");
    put_hex((u32)g_kernel_as);
    port_puts("\n");
    port_as_activate(g_kernel_as);
    g_dmap_ready = 1;                     /* 之后所有页表内存走直接映射窗口 */
}

u64 port_as_create(void)
{
    u64 as = build_as();
    return as;
}

void port_as_destroy(u64 as)
{
    (void)as;                             /* L1 不回收；L5 接帧回收 */
}

int port_as_map(u64 as, u64 vaddr, u64 paddr, u64 bytes, u32 prot)
{
    u64 off = 0;

    if (as == 0ull || bytes == 0ull) {
        return -1;
    }
    if ((vaddr & (PAGE_SZ - 1ull)) != 0ull || (bytes & (PAGE_SZ - 1ull)) != 0ull) {
        return -1;
    }
    if (paddr != 0ull && (paddr & (PAGE_SZ - 1ull)) != 0ull) {
        return -1;
    }
    while (off < bytes) {
        u64 pa = (paddr != 0ull) ? (paddr + off) : port_phys_alloc();
        u64 leaf = 0ull;

        if (pa == 0ull) {
            return -2;
        }
        if ((prot & C6OS_PROT_W) != 0u) {
            leaf |= PTE_W;
        }
        if ((prot & C6OS_PROT_X) == 0u) {
            leaf |= PTE_NX;
        }
        if (map4k(as, vaddr + off, pa, leaf | PTE_U) != 0) {
            return -3;
        }
        /* 同一 VA 重映射（MAP_FIXED 覆盖旧页）时，TLB 里可能还是旧页框；
         * 不刷的话紧接着写这个 VA 会写进已经没人看的旧帧。 */
        invlpg(vaddr + off);
        off += PAGE_SZ;
    }
    return 0;
}

int port_as_unmap(u64 as, u64 vaddr, u64 bytes)
{
    u64 *p4;
    u64 *p3;
    u64 *p2;
    u64 *p1;
    u64 off = 0;

    if (as == 0ull) {
        return -1;
    }
    while (off < bytes) {
        u64 va = vaddr + off;

        p4 = tbl(as);
        if ((p4[IDX4(va)] & PTE_P) == 0ull) {
            return -2;
        }
        p3 = tbl(p4[IDX4(va)]);
        if ((p3[IDX3(va)] & PTE_P) == 0ull) {
            return -2;
        }
        p2 = tbl(p3[IDX3(va)]);
        if ((p2[IDX2(va)] & (PTE_P | PTE_PS)) != PTE_P) {
            return -2;
        }
        p1 = tbl(p2[IDX2(va)]);
        p1[IDX1(va)] = 0ull;
        invlpg(va);
        off += PAGE_SZ;
    }
    return 0;
}

int port_as_user_range_ok(u64 as, u64 vaddr, u64 len, u32 prot)
{
    u64 va;

    if (as == 0ull) {
        return 0;
    }
    for (va = vaddr & ~(PAGE_SZ - 1ull); va < vaddr + len; va += PAGE_SZ) {
        u64 *p4 = tbl(as);
        u64 *p3;
        u64 *p2;
        u64 *p1;
        u64 e;

        if ((p4[IDX4(va)] & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            return 0;
        }
        p3 = tbl(p4[IDX4(va)]);
        if ((p3[IDX3(va)] & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            return 0;
        }
        p2 = tbl(p3[IDX3(va)]);
        e = p2[IDX2(va)];
        if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            return 0;
        }
        if ((e & PTE_PS) != 0ull) {
            if ((prot & C6OS_PROT_X) != 0u) {
                return 0;                 /* 2MB 内核页不给用户执行 */
            }
            continue;
        }
        p1 = tbl(e);
        e = p1[IDX1(va)];
        if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            return 0;
        }
        if ((prot & C6OS_PROT_W) != 0u && (e & PTE_W) == 0ull) {
            return 0;
        }
        if ((prot & C6OS_PROT_X) != 0u && (e & PTE_NX) != 0ull) {
            return 0;
        }
    }
    return 1;
}

/* fork：把 src_as 里所有用户页（PTE_U）复制到 dst_as。
 *
 * 只复制 4KB 叶子且带用户位的页：内核低端 0..4MB 的 identity 与 DMAP 都是
 * 2MB 页 + supervisor，既不该也不能复制。子进程拿到的是独立的物理页，
 * 所以父子写同一份数据不会互相踩（代价是 fork 变慢，写时复制留到后面）。 */
int port_as_copy(u64 dst_as, u64 src_as)
{
    u64 *p4;
    u64 i4;

    if (dst_as == 0ull || src_as == 0ull) {
        return -1;
    }
    p4 = tbl(src_as);
    for (i4 = 0; i4 < 512ull; i4++) {
        u64 *p3;
        u64 i3;

        if ((p4[i4] & PTE_P) == 0ull) {
            continue;
        }
        p3 = tbl(p4[i4]);
        for (i3 = 0; i3 < 512ull; i3++) {
            u64 *p2;
            u64 i2;

            if ((p3[i3] & PTE_P) == 0ull || (p3[i3] & PTE_PS) != 0ull) {
                continue;
            }
            p2 = tbl(p3[i3]);
            for (i2 = 0; i2 < 512ull; i2++) {
                u64 *p1;
                u64 i1;

                if ((p2[i2] & PTE_P) == 0ull || (p2[i2] & PTE_PS) != 0ull) {
                    continue;
                }
                p1 = tbl(p2[i2]);
                for (i1 = 0; i1 < 512ull; i1++) {
                    u64 e = p1[i1];
                    u64 va;
                    u64 npa;
                    u8 *src;
                    u8 *dst;
                    u32 k;

                    if ((e & PTE_P) == 0ull || (e & PTE_U) == 0ull) {
                        continue;
                    }
                    va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    npa = port_phys_alloc();
                    if (npa == 0ull) {
                        return -2;
                    }
                    src = (u8 *)port_phys_map(e & PTE_ADDR);
                    dst = (u8 *)port_phys_map(npa);
                    for (k = 0; k < 4096u; k++) {
                        dst[k] = src[k];
                    }
                    if (map4k(dst_as, va, npa,
                              (e & 0xfffull) | (e & PTE_NX)) != 0) {
                        return -3;
                    }
                }
            }
        }
    }
    return 0;
}
