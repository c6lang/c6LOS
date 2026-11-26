/* port/x86_64/fwcfg.c —— QEMU fw_cfg：把 Chrome rootfs 用 DMA 灌进内存
 *
 * 第二块 IDE 盘是生产路径，但 617MB 的 PIO 读在 TCG/KVM 下都要几分钟，调试
 * Chrome 不现实。QEMU 的 fw_cfg 提供 DMA 通道：hypervisor 直接按物理地址
 * 写 guest 内存，一次拷贝整块 rootfs，几秒完成。没有 fw_cfg 的环境自动回退
 * 到 ATA（tests/t_iso_alpine.c6 里判断）。
 *
 * 协议：
 *   outw(0x510, selector) 选条目；inb(0x511) 顺序读数据；
 *   0x514/0x518 是 64 位 DMA 地址寄存器（写入触发传输）。
 * 结构 FWCfgDmaAccess 全 big-endian：control(u32) length(u32) address(u64)。
 */
#include "c6os/types.h"
#include "c6os/port.h"

#define FW_SEL      0x510u
#define FW_DATA     0x511u
#define FW_DMA_LO   0x514u
#define FW_DMA_HI   0x518u
#define FW_FILE_DIR 0x19u
#define FW_CFG_FILE_FIRST 0x20u

static u64 g_dma_pa;

typedef struct {
    u8 control[4];
    u8 length[4];
    u8 address[8];
} FwDma;

static void outw_(u16 port, u16 v)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(port));
}

static u8 inb_(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static u32 inl_(u16 port)
{
    u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void outl_(u16 port, u32 v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

static void dbg_hex_(u64 v)
{
    static const char h[] = "0123456789abcdef";
    int i;
    port_puts("0x");
    for (i = 60; i >= 0; i -= 4) {
        port_putc((int)h[(v >> (u32)i) & 0xFu]);
    }
}

static int k_str_eq_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static u16 be16_(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

static u32 be32_(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be32_(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

static void put_be64_(u8 *p, u64 v)
{
    put_be32_(p, (u32)(v >> 32));
    put_be32_(p + 4, (u32)v);
}

/* 选一个 fw_cfg 条目；返回 0 表示存在。 */
static int fw_select_(u16 sel)
{
    outw_(FW_SEL, sel);
    return 0;
}

static u32 fw_read32_(u16 sel)
{
    u8 b[4];
    u32 i;
    fw_select_(sel);
    for (i = 0; i < 4u; i++) {
        b[i] = inb_(FW_DATA);
    }
    return be32_(b);
}

/* 找 named file：返回 selector 和大小（u64）。 */
int port_fwcfg_find(const char *name, u16 *out_sel, u64 *out_size)
{
    u8 sig[4];
    u32 id;
    u32 count;
    u32 i;
    u32 j;

    outw_(FW_SEL, 0u);
    for (j = 0; j < 4u; j++) {
        sig[j] = inb_(FW_DATA);
    }
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        return -1;
    }
    id = fw_read32_(1u);
    /* QEMU 有的构建把 ID 以 little-endian 返回；签名已确认是 QEMU，这里
     * 不再用 ID 的 bit1 拒绝（DMA 不可用时 control 会报 error）。 */
    (void)id;
    count = fw_read32_(FW_FILE_DIR);
    if (count == 0u || count > 512u) {
        return -3;
    }
    fw_select_(FW_FILE_DIR);
    for (i = 0; i < 4u; i++) {                   /* 跳过 count */
        (void)inb_(FW_DATA);
    }
    for (i = 0; i < count; i++) {
        u8 ent[64];
        u32 k;
        for (k = 0; k < 64u; k++) {
            ent[k] = inb_(FW_DATA);
        }
        if (ent[8] == 0) {                       /* 空名字 */
            continue;
        }
        if (k_str_eq_((const char *)(ent + 8), name)) {
            *out_sel = be16_(ent + 4);
            *out_size = (u64)be32_(ent);
            return 0;
        }
    }
    return -4;
}

/* DMA 读 sel 的 size 字节到 dst_pa（必须连续物理页）。 */
int port_fwcfg_read(u16 sel, u64 dst_pa, u64 size)
{
    FwDma *d;
    u32 ctrl;
    u32 loops;

    if (dst_pa == 0ull || size == 0ull) {
        return -1;
    }
    /* 0x7F000 落在内核镜像/.bss 范围里，绝不能拿来放 DMA 结构；分配一页
     * 物理页，既安全又保证 4 字节对齐。 */
    if (g_dma_pa == 0ull) {
        g_dma_pa = port_phys_alloc_pages(1ull);
        if (g_dma_pa == 0ull) {
            return -1;
        }
    }
    d = (FwDma *)port_phys_map(g_dma_pa);
    put_be32_(d->control, 0u);
    put_be32_(d->length, (u32)size);
    put_be64_(d->address, dst_pa);
    put_be32_(d->control, 0x0Au | ((u32)sel << 16));   /* SELECT|READ */
    __asm__ __volatile__("mfence" ::: "memory");
    /* IOport 路径实际是低 32 位先写、再写高 32 位触发（与 MMIO 相反）；
     * 地址在 4GB 内，高半为 0。 */
    /* IOport DMA 地址寄存器：0x514 是**高 32 位**，0x518 是低 32 位，
     * 写低半触发传输（与 0x514/0x518 的字面“高低”相反，踩过坑）。 */
    outl_(FW_DMA_LO, __builtin_bswap32((u32)(g_dma_pa >> 32)));
    outl_(FW_DMA_HI, __builtin_bswap32((u32)(g_dma_pa & 0xFFFFFFFFu)));
    for (loops = 0u; loops < 200000000u; loops++) {
        ctrl = be32_(d->control);
        if ((ctrl & 1u) != 0u) {
            port_puts("fwcfg: dma err dma_pa=");
            dbg_hex_(g_dma_pa);
            port_puts(" dst=");
            dbg_hex_(dst_pa);
            port_puts(" ctrl=");
            dbg_hex_((u64)ctrl);
            port_puts("\n");
            return -2;
        }
        if (ctrl == 0u) {
            return 0;
        }
        __asm__ __volatile__("pause");
    }
    return -3;
}
