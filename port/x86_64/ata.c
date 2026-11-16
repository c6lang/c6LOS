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

int port_ata_read(u32 lba, u32 count, u64 dst_pa)
{
    u8 *buf = (u8 *)port_phys_map(dst_pa);
    u32 s;

    for (s = 0; s < count; s++) {
        u32 l = lba + s;
        u8 *p = buf + (u64)s * 512ull;
        int i;

        outb(0x1F6u, (u8)(0xE0u | (u8)((l >> 24) & 0x0Fu)));   /* LBA 模式 */
        outb(0x1F1u, 0x00u);
        outb(0x1F2u, 0x01u);                                    /* 1 个扇区 */
        outb(0x1F3u, (u8)(l & 0xFFu));
        outb(0x1F4u, (u8)((l >> 8) & 0xFFu));
        outb(0x1F5u, (u8)((l >> 16) & 0xFFu));
        outb(0x1F7u, 0x20u);                                    /* READ SECTORS */

        if (ata_wait_ready() != 0) {
            return -1;
        }
        for (i = 0; i < 256; i++) {
            u16 w = inw(0x1F0u);
            p[i * 2] = (u8)(w & 0xFFu);
            p[i * 2 + 1] = (u8)(w >> 8);
        }
    }
    return 0;
}
