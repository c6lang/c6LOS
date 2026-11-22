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
/* 软件位：COW 共享页。bit 9-11 硬件不用，正好用来标记"父子共享、写时复制"。
 * 置 COW 时一定同时清 W：写触发 #PF，由 port_pf_resolve 复制出新页。 */
#define PTE_COW   (1ull << 9)
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
/* 每帧引用计数：一个页框可以同时是父子地址空间的映射、页缓存的内容，只有最后
 * 一个引用消失才能回 free list。表不能放 .bss：内核必须整体留在 0xA0000 以下
 * （0xA0000-0xFFFFF 是 VGA/BIOS 空洞，写进去就是写设备寄存器，QEMU 会直接
 * abort）。所以启动后从 PMM 里取一段连续页做表，指针经直接映射窗口访问。
 * NULL = 表还没建好（或没放下）：此时退化为"立即回收"，早期页表页从不共享。 */
static u8 *g_ref;
static u64 g_ref_bytes;                   /* 表覆盖多少字节（= 1 byte/frame） */
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
    /* 内核自己就住在低端：boot16 装到 0x10000，stage1 后面紧跟链接在
     * 0x11000，.bss 到 ~0x92000。这段在 E820 里是 usable，必须整块扣掉
     * —— 否则分配器跑到内核镜像之后会把内核代码/数据当作页表页清零
     * （L2 一次映射上百页时必然踩到）。 */
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
            if (g_ref != (u8 *)0 && f < g_ref_bytes) {
                g_ref[f] = 1u;
            }
            g_free--;
            g_rover = f + 1ull;
            return f << 12;
        }
        f++;
        scanned++;
    }
    return 0;
}

void port_phys_ref(u64 pa)
{
    u64 f = pa >> 12;

    if (g_ref == (u8 *)0 || f >= g_ref_bytes || f < 4ull || bmp_get(f) == 0) {
        return;
    }
    if (g_ref[f] < 255u) {
        g_ref[f]++;
    }
}

/* 计数归零才真正回 free list；对未分配/保留帧幂等（防 double free）。 */
void port_phys_put(u64 pa)
{
    u64 f = pa >> 12;

    if (f < 4ull || f >= PMM_MAX_FRAMES || bmp_get(f) == 0) {
        return;
    }
    if (g_ref != (u8 *)0 && f < g_ref_bytes) {
        if (g_ref[f] > 1u) {
            g_ref[f]--;
            return;
        }
        g_ref[f] = 0u;
    }
    bmp_clr(f);
    g_free++;
}

void port_phys_free(u64 pa)
{
    port_phys_put(pa);
}

/* 连续 n 页物理帧：页表页、内核堆的 slab 页都要求"页对齐起点 + 整页"，大对象
 * 还要物理连续（直接映射窗口里才连续可读写）。失败返回 0。 */
u64 port_phys_alloc_pages(u64 n)
{
    u64 f = g_rover;
    u64 scanned = 0ull;
    u64 run = 0ull;
    u64 base = 0ull;

    if (n == 0ull) {
        return 0ull;
    }
    while (scanned < g_frames) {
        if (f >= g_frames) {
            f = 4ull;
            run = 0ull;                   /* 绕回低端：连续性在这里断掉 */
        }
        if (!bmp_get(f)) {
            if (run == 0ull) {
                base = f;
            }
            run++;
            if (run >= n) {
                u64 k;

                for (k = base; k < base + n; k++) {
                    bmp_set(k);
                    if (g_ref != (u8 *)0 && k < g_ref_bytes) {
                        g_ref[k] = 1u;
                    }
                    g_free--;
                }
                g_rover = base + n;
                return base << 12;
            }
        } else {
            run = 0ull;
        }
        f++;
        scanned++;
    }
    return 0ull;
}

void port_phys_free_pages(u64 pa, u64 n)
{
    u64 k;

    for (k = 0ull; k < n; k++) {
        port_phys_put(pa + (k << 12));
    }
}

u64 port_dmap_base(void)
{
    return DMAP_BASE;
}

/* 当前引用数。COW 用：只剩一个引用时直接放开写，不必复制。
 * 表还没建好时返回 1（退化为"独占"）。 */
static u32 phys_refs_(u64 pa)
{
    u64 f = pa >> 12;

    if (g_ref == (u8 *)0 || f >= g_ref_bytes) {
        return 1u;
    }
    return (g_ref[f] == 0u) ? 1u : (u32)g_ref[f];
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

/* 建立"每帧一字节"的引用计数表：从 PMM 空闲区取一段连续页，经直接映射访问。
 * 表本身是 1 byte/frame（256MB→64KB，1GB→256KB），所以不能放 .bss；放不下
 * 就保持 NULL，退化为"put 立即回收"（早期内核页表页从不共享，不会出错）。 */
static void ref_table_init(void)
{
    u64 need = g_frames;                  /* 1 byte per frame */
    u64 pages = (need + PAGE_SZ - 1ull) >> 12;
    u64 base = 0ull;
    u64 run = 0ull;
    u64 f;

    if (pages == 0ull) {
        return;
    }
    for (f = 4ull; f < g_frames; f++) {
        if (!bmp_get(f)) {
            if (run == 0ull) {
                base = f;
            }
            run++;
            if (run >= pages) {
                break;
            }
        } else {
            run = 0ull;
        }
    }
    if (run < pages) {
        return;
    }
    for (f = base; f < base + pages; f++) {
        bmp_set(f);
        g_free--;
    }
    g_ref = (u8 *)port_phys_map(base << 12);
    g_ref_bytes = pages * PAGE_SZ;
    for (f = 0ull; f < g_ref_bytes; f++) {
        g_ref[f] = 0u;
    }
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
    /* 旧 AS 的最后一个引用可能是在它还挂在 CR3 上时放掉的；现在换下来了，
     * 可以安全回收它的页表。 */
    port_as_gc();
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
    ref_table_init();                     /* 引用计数表要等直接映射可用 */
    port_kmem_init();                     /* 内核堆：内核对象（VMA/动态表）的地基 */
}

/* ---- 地址空间生命周期 ----------------------------------------------------
 *
 * 句柄就是 PML4 的物理地址，没有地方塞引用计数，所以先用一张小登记表：
 * 线程组共享一个 AS（CLONE_VM），最后一个线程退出才真正销毁。槽位可复用；
 * 表满时 create 失败（不静默覆盖）。M13-2 把它收进 OsAs 对象后这张表消失。
 *
 * 销毁必须做三件事：还叶子页框、还中间页表页、还 PML4 自己。漏掉任何一层
 * 都会在"反复 fork/exec"的负载下把内存吃光 —— 这是 L2 里 fork 100 次就
 * OOM 的根因。
 */

#define AS_SLOTS 64u

static u64 g_as_pml4[AS_SLOTS];
static u32 g_as_ref[AS_SLOTS];

static u64 cur_as_(void)
{
    u64 v;

    __asm__ __volatile__("movq %%cr3, %0" : "=r"(v));
    return v;
}

static int as_slot_find_(u64 as)
{
    u32 i;

    for (i = 0u; i < AS_SLOTS; i++) {
        if (g_as_pml4[i] == as) {
            return (int)i;
        }
    }
    return -1;
}

static int as_slot_alloc_(u64 as)
{
    u32 i;

    for (i = 0u; i < AS_SLOTS; i++) {
        if (g_as_pml4[i] == 0ull) {
            g_as_pml4[i] = as;
            g_as_ref[i] = 1u;
            return (int)i;
        }
    }
    return -1;
}

/* AS 释放钩子：上层（linux/vma.c 等）在地址空间真正拆掉前要把自己的元信息还掉。
 * port 层不认识 VMA/进程，所以只发事件、不反向依赖 linux/。 */
static void (*g_as_free_hook)(u64 as);

void port_as_set_free_hook(void (*fn)(u64 as))
{
    g_as_free_hook = fn;
}

static void as_free_(u64 as)
{
    u64 *p4;
    u32 i4;

    if (as == 0ull || as == g_kernel_as) {
        return;
    }
    p4 = tbl(as);
    for (i4 = 0u; i4 < 512u; i4++) {
        u64 e4 = p4[i4];
        u64 *p3;
        u32 i3;

        /* 内核/DMAP 的 PML4 项没有 U 位：共享，不能还 */
        if ((e4 & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            continue;
        }
        p3 = tbl(e4);
        for (i3 = 0u; i3 < 512u; i3++) {
            u64 e3 = p3[i3];
            u64 *p2;
            u32 i2;

            if ((e3 & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
                continue;
            }
            p2 = tbl(e3);
            for (i2 = 0u; i2 < 512u; i2++) {
                u64 e2 = p2[i2];
                u64 *p1;
                u32 i1;

                if ((e2 & PTE_P) == 0ull || (e2 & PTE_PS) != 0ull ||
                    (e2 & PTE_U) == 0ull) {
                    continue;
                }
                p1 = tbl(e2);
                for (i1 = 0u; i1 < 512u; i1++) {
                    u64 e1 = p1[i1];

                    if ((e1 & (PTE_P | PTE_U)) == (PTE_P | PTE_U)) {
                        port_phys_put(e1 & PTE_ADDR);
                        p1[i1] = 0ull;
                    }
                }
                port_phys_put(e2 & PTE_ADDR);       /* PT 页 */
            }
            port_phys_put(e3 & PTE_ADDR);           /* PD 页 */
        }
        port_phys_put(e4 & PTE_ADDR);               /* PDPT 页 */
    }
    port_phys_put(as);                              /* PML4 页 */
    if (g_as_free_hook != (void (*)(u64))0) {
        g_as_free_hook(as);                         /* 上层还自己的元信息 */
    }
}

u64 port_as_create(void)
{
    u64 as = build_as();

    if (as == 0ull) {
        return 0ull;
    }
    if (as_slot_alloc_(as) < 0) {
        as_free_(as);
        return 0ull;
    }
    return as;
}

void port_as_destroy(u64 as)
{
    int slot = as_slot_find_(as);

    as_free_(as);
    if (slot >= 0) {
        g_as_pml4[slot] = 0ull;
        g_as_ref[slot] = 0u;
    }
}

void port_as_ref(u64 as)
{
    int slot = as_slot_find_(as);

    if (slot >= 0 && g_as_ref[slot] != 0xffffffffu) {
        g_as_ref[slot]++;
    }
}

void port_as_unref(u64 as)
{
    int slot = as_slot_find_(as);

    if (slot < 0) {
        return;
    }
    if (g_as_ref[slot] > 1u) {
        g_as_ref[slot]--;
        return;
    }
    g_as_ref[slot] = 0u;
    if (as == cur_as_()) {
        return;                             /* 还在 CPU 上：交给 port_as_gc */
    }
    g_as_pml4[slot] = 0ull;
    as_free_(as);
}

void port_as_gc(void)
{
    u64 cur = cur_as_();
    u32 i;

    for (i = 0u; i < AS_SLOTS; i++) {
        u64 as = g_as_pml4[i];

        if (as == 0ull || g_as_ref[i] != 0u || as == cur) {
            continue;
        }
        g_as_pml4[i] = 0ull;
        as_free_(as);
    }
}

/* 叶子页是否已经映射。惰性匿名页第一次缺页后就有页了；之后 copy_*_user
 * 的 VMA 校验不能再把它换成新零页 —— port_as_map 会先还掉旧页框，用户刚
 * 写进去的数据（malloc 的 chunk header 等）会被整个抹掉。 */
static int mapped_leaf_(u64 as, u64 va)
{
    u64 *p4 = tbl(as);
    u64 *p3;
    u64 *p2;
    u64 e;

    if ((p4[IDX4(va)] & PTE_P) == 0ull) {
        return 0;
    }
    p3 = tbl(p4[IDX4(va)]);
    if ((p3[IDX3(va)] & (PTE_P | PTE_PS)) != PTE_P) {
        return 0;
    }
    p2 = tbl(p3[IDX3(va)]);
    e = p2[IDX2(va)];
    if ((e & (PTE_P | PTE_PS)) != PTE_P) {
        return 0;
    }
    e = tbl(e)[IDX1(va)];
    return (e & (PTE_P | PTE_U)) == (PTE_P | PTE_U);
}

/* 覆盖映射：把 va 上已有的用户叶子页还掉。页表层级不存在 = 本来就没映射。 */
static void unmap_leaf_(u64 as, u64 va)
{
    u64 *p4 = tbl(as);
    u64 *p3;
    u64 *p2;
    u64 *p1;
    u64 e;

    if ((p4[IDX4(va)] & PTE_P) == 0ull) {
        return;
    }
    p3 = tbl(p4[IDX4(va)]);
    if ((p3[IDX3(va)] & PTE_P) == 0ull) {
        return;
    }
    p2 = tbl(p3[IDX3(va)]);
    e = p2[IDX2(va)];
    if ((e & (PTE_P | PTE_PS)) != PTE_P) {
        return;
    }
    p1 = tbl(e);
    e = p1[IDX1(va)];
    if ((e & (PTE_P | PTE_U)) == (PTE_P | PTE_U)) {
        port_phys_put(e & PTE_ADDR);
    }
    p1[IDX1(va)] = 0ull;
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
        u64 va = vaddr + off;
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
        /* MAP_FIXED：先还掉旧页框，再装新页（否则旧帧永远漏掉） */
        unmap_leaf_(as, va);
        if (map4k(as, va, pa, leaf | PTE_U) != 0) {
            if (paddr == 0ull) {
                port_phys_put(pa);
            }
            return -3;
        }
        /* 同一 VA 重映射（MAP_FIXED 覆盖旧页）时，TLB 里可能还是旧页框；
         * 不刷的话紧接着写这个 VA 会写进已经没人看的旧帧。 */
        invlpg(va);
        off += PAGE_SZ;
    }
    return 0;
}

/* 惰性匿名页：分配→清零→挂表。清零必须在这里做：port_as_map 的分配路径不清零
 * （它也可能是 MAP_FIXED 换页），漏了就是"新进程读到别人数据"的信息泄漏。
 * 只挂一页，失败时自己还帧（port_as_map 对给定 paddr 的失败不负责回收）。 */
int port_as_map_zero(u64 as, u64 vaddr, u32 prot)
{
    u64 pa;
    u8 *p;
    u64 i;

    /* 已经映射过就直接返回：lx_user_ok 每次校验用户缓冲都会走
     * lx_vma_fault_range，而 LAZY 标记在首次缺页后并没有清掉。 */
    if (mapped_leaf_(as, vaddr) != 0) {
        return 0;
    }
    pa = port_phys_alloc();
    if (pa == 0ull) {
        return -1;
    }
    p = (u8 *)port_phys_map(pa);
    for (i = 0ull; i < PAGE_SZ; i++) {
        p[i] = 0u;
    }
    if (port_as_map(as, vaddr, pa, PAGE_SZ, prot) != 0) {
        port_phys_put(pa);
        return -2;
    }
    return 0;
}

u64 port_as_current(void)
{
    return cur_as_();
}

int port_as_unmap(u64 as, u64 vaddr, u64 bytes)
{
    u64 off = 0;

    if (as == 0ull) {
        return -1;
    }
    while (off < bytes) {
        u64 va = vaddr + off;
        u64 *p4 = tbl(as);
        u64 *p3;
        u64 *p2;
        u64 *p1;
        u64 e;
        u32 i;
        int empty = 1;

        off += PAGE_SZ;
        /* munmap 允许范围里有洞：没有映射就跳过，不算错 */
        if ((p4[IDX4(va)] & PTE_P) == 0ull) {
            continue;
        }
        p3 = tbl(p4[IDX4(va)]);
        if ((p3[IDX3(va)] & PTE_P) == 0ull) {
            continue;
        }
        p2 = tbl(p3[IDX3(va)]);
        if ((p2[IDX2(va)] & (PTE_P | PTE_PS)) != PTE_P) {
            continue;
        }
        p1 = tbl(p2[IDX2(va)]);
        e = p1[IDX1(va)];
        if ((e & (PTE_P | PTE_U)) == (PTE_P | PTE_U)) {
            port_phys_put(e & PTE_ADDR);
        }
        p1[IDX1(va)] = 0ull;
        invlpg(va);
        /* 整张 PT 空了就还掉：长期进程反复 mmap/munmap 也不漏页表页 */
        for (i = 0u; i < 512u; i++) {
            if (p1[i] != 0ull) {
                empty = 0;
                break;
            }
        }
        if (empty != 0) {
            u64 pt = p2[IDX2(va)] & PTE_ADDR;

            p2[IDX2(va)] = 0ull;
            port_phys_put(pt);
        }
    }
    return 0;
}

/* 把 slot 上的 COW 页私有化（ref>1 时复制，ref==1 时原地去 COW）。
 * 返回 1 = 已处理（slot 里没有 COW 了）；0 = 本来就是私有页或没内存。
 * mprotect 必须先走这一步：否则"改权限"与 COW 软件位会互相打架
 * （例如 PROT_READ 之后又 PROT_WRITE，会把共享页直接变成可写）。 */
static int cow_break_(u64 *slot)
{
    u64 e = *slot;
    u64 pa = e & PTE_ADDR;
    u64 npa;
    u8 *src;
    u8 *dst;
    u32 k;

    if ((e & PTE_COW) == 0ull) {
        return 0;
    }
    if (phys_refs_(pa) <= 1u) {
        *slot = e & ~PTE_COW;
        return 1;
    }
    npa = port_phys_alloc();
    if (npa == 0ull) {
        return 0;
    }
    src = (u8 *)port_phys_map(pa);
    dst = (u8 *)port_phys_map(npa);
    for (k = 0u; k < 4096u; k++) {
        dst[k] = src[k];
    }
    port_phys_put(pa);
    *slot = (npa & PTE_ADDR) | (e & 0xfffull & ~PTE_COW) | (e & PTE_NX);
    return 1;
}

int port_as_protect(u64 as, u64 vaddr, u64 bytes, u32 prot)
{
    u64 off;

    if (as == 0ull) {
        return -1;
    }
    for (off = 0ull; off < bytes; off += PAGE_SZ) {
        u64 va = vaddr + off;
        u64 *p4 = tbl(as);
        u64 *p3;
        u64 *p2;
        u64 *p1;
        u64 e;

        if ((p4[IDX4(va)] & PTE_P) == 0ull) {
            return -2;
        }
        p3 = tbl(p4[IDX4(va)]);
        if ((p3[IDX3(va)] & PTE_P) == 0ull) {
            return -2;
        }
        p2 = tbl(p3[IDX3(va)]);
        e = p2[IDX2(va)];
        if ((e & (PTE_P | PTE_PS)) != PTE_P) {
            return -2;
        }
        p1 = tbl(e);
        e = p1[IDX1(va)];
        if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            return -2;
        }
        if ((e & PTE_COW) != 0ull) {
            /* 共享页先私有化：mprotect 的"意图权限"要落在独占页上，否则
             * COW 软件位与 W 位互相覆盖，父子隔离就破了。 */
            if (cow_break_(&p1[IDX1(va)]) == 0) {
                return -3;                 /* 没内存 */
            }
            e = p1[IDX1(va)];
        }
        e &= ~(PTE_W | PTE_NX);
        if ((prot & C6OS_PROT_W) != 0u) {
            e |= PTE_W;
        }
        if ((prot & C6OS_PROT_X) == 0u) {
            e |= PTE_NX;
        }
        p1[IDX1(va)] = e;
        invlpg(va);
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
        /* COW 页在 OS 看来是可写的：写会走 #PF 私有化。调用方（copy_to_user
         * 的前置校验）不该因此判 EFAULT —— fork 之后 wait4 写状态失败、父进程
         * 收不到尸，就是漏了这条。 */
        if ((prot & C6OS_PROT_W) != 0u && (e & PTE_W) == 0ull &&
            (e & PTE_COW) == 0ull) {
            return 0;
        }
        if ((prot & C6OS_PROT_X) != 0u && (e & PTE_NX) != 0ull) {
            return 0;
        }
    }
    return 1;
}

/* fork：把 src_as 的用户页共享给 dst_as（写时复制）。
 *
 * 只处理 4KB 叶子且带用户位的页：内核低端 0..4MB 的 identity 与 DMAP 都是
 * 2MB 页 + supervisor，既不该也不能共享。
 *
 * 可写页：两边都清 W、置软件位 PTE_COW，页框引用 +1；任何一方写都会 #PF，
 * 由 port_pf_resolve 复制出新页（引用只剩 1 时直接放开 W）。
 * 只读页（代码/RELRO）：只加引用、原样映射，写它仍然是 SIGSEGV。
 *
 * src 的 PTE 被改成只读了，必须逐页 invlpg：否则父进程 TLB 里还是可写项，
 * 会绕过 COW 直接改写共享页。 */
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
                    u64 pa;
                    u64 nf;
                    u64 va;

                    if ((e & PTE_P) == 0ull || (e & PTE_U) == 0ull) {
                        continue;
                    }
                    va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    pa = e & PTE_ADDR;
                    nf = e;
                    if ((e & PTE_W) != 0ull) {
                        nf = (e & ~PTE_W) | PTE_COW;
                        p1[i1] = nf;              /* 父进程也变成只读 */
                        invlpg(va);
                    }
                    if (map4k(dst_as, va, pa,
                              (nf & 0xfffull) | (nf & PTE_NX)) != 0) {
                        return -3;
                    }
                    port_phys_ref(pa);
                }
            }
        }
    }
    return 0;
}

/* #PF 兜底：写一个 COW 页时把页框私有化。
 *
 * 由异常出口（port/x86_64/linux.c 的 x86_exc_dump）调用：ring3 直接写会进来，
 * 内核写用户缓冲（copy_to_user / wait4 的状态写回）也会进来 —— 后者如果只按
 * CS==3 判断就会漏掉，所以这个函数不看 CPL，只看 PTE。
 *
 * 返回 1 = 已修好，原指令可以重试；0 = 不是 COW 缺页，交给上层投 SIGSEGV。
 * err 位：bit0 = present（保护违例），bit1 = 写，bit2 = 用户页。 */
int port_pf_resolve(u64 va, u32 err)
{
    u64 as = cur_as_();
    u64 *p4 = tbl(as);
    u64 *p3;
    u64 *p2;
    u64 *p1;
    u64 e;

    if ((err & 7u) != 7u) {               /* 必须是 present + write + user */
        return 0;
    }
    if ((p4[IDX4(va)] & PTE_P) == 0ull) {
        return 0;
    }
    p3 = tbl(p4[IDX4(va)]);
    if ((p3[IDX3(va)] & PTE_P) == 0ull) {
        return 0;
    }
    p2 = tbl(p3[IDX3(va)]);
    if ((p2[IDX2(va)] & (PTE_P | PTE_PS)) != PTE_P) {
        return 0;
    }
    p1 = tbl(p2[IDX2(va)]);
    e = p1[IDX1(va)];
    if ((e & (PTE_P | PTE_U | PTE_COW)) != (PTE_P | PTE_U | PTE_COW)) {
        return 0;
    }
    if (cow_break_(&p1[IDX1(va)]) == 0) {
        return 0;                          /* 没内存：交上层 */
    }
    p1[IDX1(va)] |= PTE_W;
    invlpg(va);
    return 1;
}
