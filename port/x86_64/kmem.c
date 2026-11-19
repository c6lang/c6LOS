#include "c6os/types.h"
#include "c6os/port.h"

/* 内核堆（M13-2b / M13-3 的地基）。
 *
 * 为什么需要它：VMA、动态进程/fd 表、等待队列节点这些内核对象以前要么是固定
 * 数组（有编译期上限），要么根本没地方分配。内核镜像必须整体留在 0xA0000 以下
 * （0xA0000-0xFFFFF 是 VGA/BIOS 空洞，写进去 QEMU 直接 abort），所以任何"随
 * RAM 变大"的元数据都不能进 .bss；这里把元信息全部放在页头，.bss 里只留 7 个
 * 类头指针，堆页从 PMM 现取、经直接映射窗口访问。
 *
 * 两个分配器：
 *   - 小对象（<= 2048B）：每页一个 slab。页头 { u32 magic; u32 cls; }，对象从
 *     页 + 16 开始，空闲对象用自身前 8 字节串成自由链。释放只回链、不还页
 *     （避免抖动；slab 页随对象复用，长期上限受真实并发对象数约束）。
 *   - 大对象（> 2048B）：取连续页，页头 { u32 magic; u32 npages; }，返回
 *     页 + 16；释放时整块还页。
 * kfree 靠页头 magic 区分两者，所以指针必须来自 port_kmalloc。
 */

#define KMEM_SLAB  0x4b534c42u        /* 'KSLB' */
#define KMEM_LARGE 0x4b4c5247u        /* 'KLRG' */
#define KMEM_HDR   16ull
#define KMEM_PAGE  4096ull
#define KMEM_CLS   7u

static const u32 g_cls_sz[KMEM_CLS] = { 32u, 64u, 128u, 256u, 512u, 1024u, 2048u };

/* 每个类的空闲对象链。7 个指针 = 56 字节，可以安心放 .bss。 */
static void *g_free_obj[KMEM_CLS];
static int g_kmem_ready;

void port_kmem_init(void)
{
    u32 i;

    for (i = 0u; i < KMEM_CLS; i++) {
        g_free_obj[i] = (void *)0;
    }
    g_kmem_ready = 1;
}

/* 需要多少字节 → 类号；-1 = 走大对象路径。 */
static int cls_for(u64 need)
{
    u32 i;

    for (i = 0u; i < KMEM_CLS; i++) {
        if (need <= (u64)g_cls_sz[i]) {
            return (int)i;
        }
    }
    return -1;
}

/* 取一页新 slab，把它切成对象挂到类链上，返回链头（失败返回 0）。 */
static void *slab_grow(int ci)
{
    u64 pa = port_phys_alloc();
    u64 sz = (u64)g_cls_sz[ci];
    u8 *pg;
    void *head = (void *)0;
    u64 off;

    if (pa == 0ull) {
        return (void *)0;
    }
    pg = (u8 *)port_phys_map(pa);
    *(volatile u32 *)(void *)pg = KMEM_SLAB;
    *(volatile u32 *)(void *)(pg + 4u) = (u32)ci;
    for (off = KMEM_HDR; off + sz <= KMEM_PAGE; off += sz) {
        void *o = (void *)(pg + off);

        *(void **)o = head;
        head = o;
    }
    return head;
}

void *port_kmalloc(u64 bytes)
{
    int ci;

    if (g_kmem_ready == 0) {
        return (void *)0;
    }
    if (bytes == 0ull) {
        bytes = 8ull;
    }
    ci = cls_for((bytes + 15ull) & ~15ull);
    if (ci >= 0) {
        void *o;

        if (g_free_obj[ci] == (void *)0) {
            g_free_obj[ci] = slab_grow(ci);
            if (g_free_obj[ci] == (void *)0) {
                return (void *)0;
            }
        }
        o = g_free_obj[ci];
        g_free_obj[ci] = *(void **)o;
        return o;
    }
    {
        u64 need = bytes + KMEM_HDR;
        u64 npages = (need + KMEM_PAGE - 1ull) / KMEM_PAGE;
        u64 pa = port_phys_alloc_pages(npages);
        u8 *pg;

        if (pa == 0ull) {
            return (void *)0;
        }
        pg = (u8 *)port_phys_map(pa);
        *(volatile u32 *)(void *)pg = KMEM_LARGE;
        *(volatile u32 *)(void *)(pg + 4u) = (u32)npages;
        return (void *)(pg + KMEM_HDR);
    }
}

void *port_kcalloc(u64 n, u64 bytes)
{
    u64 total;
    u8 *p;
    u64 i;

    if (n != 0ull && bytes > (~0ull) / n) {
        return (void *)0;
    }
    total = n * bytes;
    p = (u8 *)port_kmalloc(total);
    if (p == (u8 *)0) {
        return (void *)0;
    }
    for (i = 0ull; i < total; i++) {
        p[i] = 0u;
    }
    return (void *)p;
}

void port_kfree(void *p)
{
    u8 *pg;
    u32 magic;

    if (p == (void *)0) {
        return;
    }
    pg = (u8 *)((u64)p & ~(KMEM_PAGE - 1ull));
    magic = *(const volatile u32 *)(const void *)pg;
    if (magic == KMEM_SLAB) {
        u32 ci = *(const volatile u32 *)(const void *)(pg + 4u);

        if (ci < KMEM_CLS) {
            *(void **)p = g_free_obj[ci];
            g_free_obj[ci] = p;
        }
        return;
    }
    if (magic == KMEM_LARGE) {
        u64 npages = (u64)(*(const volatile u32 *)(const void *)(pg + 4u));
        u64 pa = (u64)pg - port_dmap_base();

        port_phys_free_pages(pa, npages);
    }
}
