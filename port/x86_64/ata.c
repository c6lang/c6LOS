#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"

/* ATA PIO 读：把磁盘上的扇区搬到物理内存（通过直接映射窗口写）。
 * qemu -drive if=ide 就是主通道 master（I/O 0x1F0）。
 * L2 用它把 ELF 映像拖进内存；L4 的 VFS 块设备直接复用。 */

static void outb(u16 port, u8 v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static u8 inb(u16 port)
{
    u8 v;

    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static u16 inw(u16 port)
{
    u16 v;

    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void outw(u16 port, u16 v)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(port));
}

/* flush/写完只要 BSY 清掉、ERR 没置就行，不能等 DRQ */
static int ata_wait_idle(void)
{
    u32 i;

    for (i = 0; i < 4000000u; i++) {
        u8 st = inb(0x1F7u);
        if ((st & 0x80u) != 0u) {
            continue;
        }
        if ((st & 0x01u) != 0u) {
            return -1;
        }
        return 0;
    }
    return -2;
}

static int ata_wait_ready(void)
{
    u32 i;

    for (i = 0; i < 4000000u; i++) {
        u8 st = inb(0x1F7u);
        if ((st & 0x80u) != 0u) {
            continue;                     /* BSY */
        }
        if ((st & 0x01u) != 0u) {
            return -1;                    /* ERR */
        }
        if ((st & 0x08u) != 0u) {
            return 0;                     /* DRQ */
        }
    }
    return -2;                            /* 超时 */
}

static int ata_read_dev_(u32 dev, u32 lba, u32 count, u64 dst_pa)
{
    u8 *buf = (u8 *)port_phys_map(dst_pa);
    u32 done = 0u;

    while (done < count) {
        u32 n = count - done;
        u32 s;

        if (n > 255u) {
            n = 255u;
        }
        outb(0x1F6u, (u8)(0xE0u | ((dev & 1u) << 4) |
                          (u8)(((lba + done) >> 24) & 0x0Fu)));
        outb(0x1F1u, 0x00u);
        outb(0x1F2u, (u8)n);
        outb(0x1F3u, (u8)((lba + done) & 0xFFu));
        outb(0x1F4u, (u8)(((lba + done) >> 8) & 0xFFu));
        outb(0x1F5u, (u8)(((lba + done) >> 16) & 0xFFu));
        outb(0x1F7u, 0x20u);                            /* READ SECTORS */
        for (s = 0u; s < n; s++) {
            u8 *p = buf + (u64)(done + s) * 512ull;
            int i;

            if (ata_wait_ready() != 0) {
                return -1;
            }
            for (i = 0; i < 256; i++) {
                u16 w = inw(0x1F0u);
                p[i * 2] = (u8)(w & 0xFFu);
                p[i * 2 + 1] = (u8)(w >> 8);
            }
        }
        done += n;
    }
    return 0;
}

int port_ata_read(u32 lba, u32 count, u64 dst_pa)
{
    return ata_read_dev_(0u, lba, count, dst_pa);
}

int port_ata_read_dev(u32 dev, u32 lba, u32 count, u64 dst_pa)
{
    return ata_read_dev_(dev, lba, count, dst_pa);
}

int port_ata_write(u32 lba, u32 count, u64 src_pa)
{
    const u8 *buf = (const u8 *)port_phys_map(src_pa);
    u32 s;

    for (s = 0; s < count; s++) {
        u32 l = lba + s;
        const u8 *p = buf + (u64)s * 512ull;
        int i;

        /* 上一扇区可能还在 BSY：先等空闲再下发命令，否则命令会被忽略 */
        if (ata_wait_idle() != 0) {
            return -1;
        }
        outb(0x1F6u, (u8)(0xE0u | (u8)((l >> 24) & 0x0Fu)));
        outb(0x1F1u, 0x00u);
        outb(0x1F2u, 0x01u);
        outb(0x1F3u, (u8)(l & 0xFFu));
        outb(0x1F4u, (u8)((l >> 8) & 0xFFu));
        outb(0x1F5u, (u8)((l >> 16) & 0xFFu));
        outb(0x1F7u, 0x30u);                            /* WRITE SECTORS */
        if (ata_wait_ready() != 0) {
            return -1;
        }
        for (i = 0; i < 256; i++) {
            u16 w = (u16)((u16)p[i * 2] | (u16)((u16)p[i * 2 + 1] << 8));

            outw(0x1F0u, w);
        }
    }
    outb(0x1F7u, 0xE7u);                                /* FLUSH CACHE */
    (void)ata_wait_idle();
    return 0;
}
