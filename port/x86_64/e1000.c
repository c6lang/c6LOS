/* port/x86_64/e1000.c —— Intel 82540EM 网卡（qemu `-nic user,model=e1000`）
 *
 * 纯轮询：不注册中断，收包靠 `port_net_recv()` 一问一答。对 M10 的
 * "收包在任务上下文里处理"来说是正好——协议栈本来就不该在 IRQ 里跑。
 *
 * 访问路径：PCI 配置空间用 0xCF8/0xCFC 的 IO 口找到 BAR0，BAR0 是 MMIO，
 * 物理地址在 0xFExxxxxx（3~4GB），靠 mmu.c 的直接映射窗口 1..4GB 段
 * （2MB 页、UC）访问，即 DMAP_BASE + pa。
 *
 * 描述符环：RX/TX 各 16 个，每个描述符 16 字节，缓冲 2048 字节，
 * 全部用 port_phys_alloc() 拿物理帧，再经 port_phys_map() 拿虚拟地址。
 */
#include "c6os/types.h"
#include "c6os/port.h"

#define E1000_VENDOR        0x8086u
#define E1000_DEV_82540     0x100Eu       /* qemu 默认 */
#define E1000_DEV_82541     0x100Fu
#define E1000_DEV_82574     0x10D3u       /* e1000e */

#define E1000_CTRL          0x0000u
#define E1000_STATUS        0x0008u
#define E1000_EECD          0x0010u
#define E1000_ICR           0x00C0u
#define E1000_IMS           0x00D0u
#define E1000_IMC           0x00D8u
#define E1000_TIPG          0x0410u
#define E1000_RCTL          0x0100u
#define E1000_TCTL          0x0400u
#define E1000_RDBAL         0x2800u
#define E1000_RDBAH         0x2804u
#define E1000_RDLEN         0x2808u
#define E1000_RDH           0x2810u
#define E1000_RDT           0x2818u
#define E1000_TDBAL         0x3800u
#define E1000_TDBAH         0x3804u
#define E1000_TDLEN         0x3808u
#define E1000_TDH           0x3810u
#define E1000_TDT           0x3818u
#define E1000_MTA           0x5200u
#define E1000_RAR0          0x5400u
#define E1000_RAR1          0x5404u

#define CTRL_RST            (1u << 26)
#define RCTL_EN             (1u << 1)
#define RCTL_BAM            (1u << 15)
#define RCTL_SECRC          (1u << 26)
#define TCTL_EN             (1u << 1)
#define TCTL_PSP            (1u << 3)

#define RING_NUM            16u
#define BUF_SIZE            2048u
#define TX_CMD_EOP_IFCS_RS  0x0Bu         /* EOP | IFCS | RS */

typedef struct E1000RxDesc {
    u64 addr;
    u16 length;
    u16 csum;
    u8  status;
    u8  errors;
    u16 special;
} E1000RxDesc;

typedef struct E1000TxDesc {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} E1000TxDesc;

static int g_found;
static u64 g_bar;                       /* MMIO 物理基址 */
static volatile u8 *g_mmio;
static u32 g_io_win;                    /* BAR1：IO 窗口（0 表示没有） */
static int g_use_io;

static u64 g_rx_ring_pa;
static u64 g_tx_ring_pa;
static u64 g_rx_buf_pa[RING_NUM];
static u64 g_tx_buf_pa[RING_NUM];
static volatile E1000RxDesc *g_rx_desc;
static volatile E1000TxDesc *g_tx_desc;
static volatile u8 *g_rx_buf[RING_NUM];
static volatile u8 *g_tx_buf[RING_NUM];
static u32 g_rx_tail;
static u32 g_tx_tail;
static u32 g_rx_packets;
static u32 g_tx_packets;
static u32 g_rx_dropped;

static void outl(u16 port, u32 v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

static u32 inl(u16 port)
{
    u32 v;

    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off)
{
    u32 addr = 0x80000000u | ((bus & 0xFFu) << 16) | ((dev & 0x1Fu) << 11) |
               ((fn & 0x07u) << 8) | (off & 0xFCu);

    outl(0xCF8u, addr);
    return inl(0xCFCu);
}

static void pci_write(u32 bus, u32 dev, u32 fn, u32 off, u32 v)
{
    u32 addr = 0x80000000u | ((bus & 0xFFu) << 16) | ((dev & 0x1Fu) << 11) |
               ((fn & 0x07u) << 8) | (off & 0xFCu);

    outl(0xCF8u, addr);
    outl(0xCFCu, v);
}

/* 82540EM 有两个 BAR：BAR0 = MMIO，BAR1 = IO 窗口。
 * qemu 两个都实现；这里优先用 IO 窗口（不依赖内核页表是否覆盖 0xFExxxxxx），
 * 没有 IO BAR 时才退回 MMIO。 */
static u32 reg_rd(u32 off)
{
    if (g_use_io != 0) {
        outl((u16)g_io_win, off);
        return inl((u16)(g_io_win + 4u));
    }
    return *(volatile u32 *)(g_mmio + off);
}

static void reg_wr(u32 off, u32 v)
{
    if (g_use_io != 0) {
        outl((u16)g_io_win, off);
        outl((u16)(g_io_win + 4u), v);
        return;
    }
    *(volatile u32 *)(g_mmio + off) = v;
}

static void mem_barrier(void)
{
    __asm__ __volatile__("mfence" : : : "memory");
}

/* 在总线上找 e1000，顺手把命令寄存器的 Memory Space + Bus Master 打开。 */
static int pci_locate(void)
{
    u32 bus;
    u32 dev;
    u32 fn;
    u32 id;
    u32 devid;
    u32 bar;
    u32 cmd;

    for (bus = 0u; bus < 4u; bus++) {
        for (dev = 0u; dev < 32u; dev++) {
            for (fn = 0u; fn < 8u; fn++) {
                id = pci_read(bus, dev, fn, 0x00u);
                if ((id & 0xFFFFu) != E1000_VENDOR) {
                    continue;
                }
                devid = (id >> 16) & 0xFFFFu;
                if (devid != E1000_DEV_82540 && devid != E1000_DEV_82541 &&
                    devid != E1000_DEV_82574) {
                    continue;
                }
                bar = pci_read(bus, dev, fn, 0x10u);
                bar = bar & 0xFFFFFFF0u;
                if (bar == 0u) {
                    continue;
                }
                cmd = pci_read(bus, dev, fn, 0x04u);
                pci_write(bus, dev, fn, 0x04u, cmd | 0x0007u);  /* IOSpace|MemSpace|BusMaster */
                g_bar = (u64)bar;
                cmd = pci_read(bus, dev, fn, 0x14u);
                if ((cmd & 0x01u) != 0u) {          /* BAR1 是 IO 空间，留作回退 */
                    g_io_win = cmd & 0xFFFFFFFCu;
                    g_use_io = 0;                   /* 优先 MMIO：mmu.c 的 1..4GB 窗口 */
                }
                return 1;
            }
        }
    }
    return 0;
}

int port_net_init(void)
{
    u32 i;
    u32 mac_lo;
    u32 mac_hi;
    u64 pa;

    if (g_found) {
        return 0;
    }
    if (pci_locate() == 0) {
            return -1;
    }
    if (g_use_io == 0) {
        g_mmio = (volatile u8 *)port_phys_map(g_bar);
    }
    /* 复位 */
    reg_wr(E1000_CTRL, reg_rd(E1000_CTRL) | CTRL_RST);
    for (i = 0; i < 2000000u; i++) {
        if ((reg_rd(E1000_CTRL) & CTRL_RST) == 0u) {
            break;
        }
    }
    /* 中断全关（我们是轮询） */
    reg_wr(E1000_IMC, 0xFFFFFFFFu);
    (void)reg_rd(E1000_ICR);
    /* 多播表清空 */
    for (i = 0; i < 128u; i++) {
        reg_wr(E1000_MTA + i * 4u, 0u);
    }
    /* 等 EEPROM 自动读完成，再读 MAC（qemu 把 -nic 的 mac 放进 RAR0） */
    for (i = 0; i < 1000000u; i++) {
        if ((reg_rd(E1000_EECD) & (1u << 4)) == 0u) {
            break;
        }
    }
    mac_lo = reg_rd(E1000_RAR0);
    mac_hi = reg_rd(E1000_RAR1);
    (void)mac_lo;
    (void)mac_hi;

    /* ---- 描述符环 ---- */
    pa = port_phys_alloc();
    if (pa == 0ull) {
            return -1;
    }
    g_rx_ring_pa = pa;
    g_rx_desc = (volatile E1000RxDesc *)port_phys_map(pa);
    pa = port_phys_alloc();
    if (pa == 0ull) {
            return -1;
    }
    g_tx_ring_pa = pa;
    g_tx_desc = (volatile E1000TxDesc *)port_phys_map(pa);
    for (i = 0; i < RING_NUM; i++) {
        u64 b;

        b = port_phys_alloc();
        if (b == 0ull) {
                    return -1;
        }
        g_rx_buf_pa[i] = b;
        g_rx_buf[i] = (volatile u8 *)port_phys_map(b);
        b = port_phys_alloc();
        if (b == 0ull) {
                    return -1;
        }
        g_tx_buf_pa[i] = b;
        g_tx_buf[i] = (volatile u8 *)port_phys_map(b);
    }
    for (i = 0; i < RING_NUM; i++) {
        g_rx_desc[i].addr = g_rx_buf_pa[i];
        g_rx_desc[i].length = 0u;
        g_rx_desc[i].csum = 0u;
        g_rx_desc[i].status = 0u;
        g_rx_desc[i].errors = 0u;
        g_rx_desc[i].special = 0u;
        g_tx_desc[i].addr = g_tx_buf_pa[i];
        g_tx_desc[i].length = 0u;
        g_tx_desc[i].cso = 0u;
        g_tx_desc[i].cmd = 0u;
        g_tx_desc[i].status = 0x01u;            /* DD：空槽可写 */
        g_tx_desc[i].css = 0u;
        g_tx_desc[i].special = 0u;
    }
    g_rx_tail = 0u;
    g_tx_tail = 0u;
    /* RX 环 */
    reg_wr(E1000_RDBAL, (u32)(g_rx_ring_pa & 0xFFFFFFFFull));
    reg_wr(E1000_RDBAH, (u32)(g_rx_ring_pa >> 32));
    reg_wr(E1000_RDLEN, RING_NUM * 16u);
    reg_wr(E1000_RDH, 0u);
    reg_wr(E1000_RDT, RING_NUM - 1u);
    /* TX 环 */
    reg_wr(E1000_TDBAL, (u32)(g_tx_ring_pa & 0xFFFFFFFFull));
    reg_wr(E1000_TDBAH, (u32)(g_tx_ring_pa >> 32));
    reg_wr(E1000_TDLEN, RING_NUM * 16u);
    reg_wr(E1000_TDH, 0u);
    reg_wr(E1000_TDT, 0u);
    mem_barrier();
    /* 开收/发 */
    reg_wr(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    reg_wr(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    reg_wr(E1000_TIPG, 10u | (8u << 10) | (6u << 20));
    mem_barrier();
    g_found = 1;
    return 0;
}

int port_net_found(void)
{
    return g_found;
}

u64 port_net_bar(void)
{
    return g_bar;
}

void port_net_mac(u8 *out)
{
    u32 lo;
    u32 hi;

    if (out == 0) {
        return;
    }
    if (g_mmio == 0 && g_use_io == 0) {
        out[0] = 0u;
        out[1] = 0u;
        out[2] = 0u;
        out[3] = 0u;
        out[4] = 0u;
        out[5] = 0u;
        return;
    }
    lo = reg_rd(E1000_RAR0);
    hi = reg_rd(E1000_RAR1);
    out[0] = (u8)(lo & 0xFFu);
    out[1] = (u8)((lo >> 8) & 0xFFu);
    out[2] = (u8)((lo >> 16) & 0xFFu);
    out[3] = (u8)((lo >> 24) & 0xFFu);
    out[4] = (u8)(hi & 0xFFu);
    out[5] = (u8)((hi >> 8) & 0xFFu);
}

int port_net_link_up(void)
{
    if (g_mmio == 0 && g_use_io == 0) {
        return 0;
    }
    return ((reg_rd(E1000_STATUS) & 0x02u) != 0u) ? 1 : 0;
}

int port_net_send(const void *frame, u32 len)
{
    volatile E1000TxDesc *d;
    u32 slot;
    u32 i;
    const u8 *src;

    if (g_found == 0 || frame == 0 || len == 0u || len > BUF_SIZE) {
        return -1;
    }
    slot = g_tx_tail;
    d = &g_tx_desc[slot];
    for (i = 0; i < 1000000u; i++) {           /* 等这个槽上一次发送完成 */
        if ((d->status & 0x01u) != 0u) {
            break;
        }
        mem_barrier();
    }
    if ((d->status & 0x01u) == 0u) {
        return -2;
    }
    src = (const u8 *)frame;
    for (i = 0; i < len; i++) {
        g_tx_buf[slot][i] = src[i];
    }
    d->length = (u16)len;
    d->cso = 0u;
    d->cmd = TX_CMD_EOP_IFCS_RS;
    d->status = 0u;
    d->css = 0u;
    d->special = 0u;
    mem_barrier();
    g_tx_tail = (slot + 1u) % RING_NUM;
    reg_wr(E1000_TDT, g_tx_tail);
    g_tx_packets = g_tx_packets + 1u;
    return 0;
}

/* 返回收到的字节数；0 = 没包；<0 = 错误。 */
int port_net_recv(void *frame, u32 max)
{
    volatile E1000RxDesc *d;
    u32 slot;
    u32 len;
    u32 i;
    u8 *dst;

    if (g_found == 0 || frame == 0 || max == 0u) {
        return -1;
    }
    slot = g_rx_tail;
    d = &g_rx_desc[slot];
    if ((d->status & 0x01u) == 0u) {
        return 0;                              /* DD 没置：没包 */
    }
    if ((d->status & 0x02u) != 0u) {           /* EOP */
        len = (u32)d->length;
    } else {
        len = 0u;
    }
    if (len == 0u || len > max || len > BUF_SIZE) {
        g_rx_dropped = g_rx_dropped + 1u;
        len = 0u;
    } else {
        dst = (u8 *)frame;
        for (i = 0; i < len; i++) {
            dst[i] = g_rx_buf[slot][i];
        }
        g_rx_packets = g_rx_packets + 1u;
    }
    d->status = 0u;
    d->length = 0u;
    mem_barrier();
    g_rx_tail = (slot + 1u) % RING_NUM;
    reg_wr(E1000_RDT, slot);                   /* 这个槽还给网卡 */
    return (int)len;
}

u32 port_net_rx_packets(void)
{
    return g_rx_packets;
}

u32 port_net_tx_packets(void)
{
    return g_tx_packets;
}

u32 port_net_rx_dropped(void)
{
    return g_rx_dropped;
}
