/* ESP32-S3 控制台：USB-Serial/JTAG（默认）。
 * 寄存器（IDF soc/esp32s3/register/soc/usb_serial_jtag_reg.h）：
 *   EP1_REG      +0x00  RDWR_BYTE：写 = 压 TX FIFO
 *   EP1_CONF_REG +0x04  bit1 SERIAL_IN_EP_DATA_FREE，bit0 WR_DONE
 *   CONF0_REG    +0x18  bit14 USB_PAD_ENABLE，bit16 USB_JTAG_BRIDGE_EN，bit0 PHY_SEL
 * 基址 0x60038000（TRM）。 */

#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/tick.h"

extern void os_sched_tick(void);
extern void s3_irq_init(void);

/* 诊断：RTC_CNTL STORE0..7 在复位后保留，esptool read-mem 0x60008050 可读。 */
void s3_mark(u32 idx, u32 v)
{
    *(volatile u32 *)(0x3FCF0000u + idx * 4u) = v;
}

u32 port_in_isr(void)
{
    return 0u;
}

void os_test_set_isr(u32 v)
{
    (void)v;
}

void port_enter_task(void)
{
}

void port_mark(u32 v)
{
    s3_mark(4u, v);
}

#define S3_USB_JTAG_BASE 0x60038000u
#define S3_USB_EP1      (*(volatile u32 *)(S3_USB_JTAG_BASE + 0x00u))
#define S3_USB_EP1_CONF (*(volatile u32 *)(S3_USB_JTAG_BASE + 0x04u))
#define S3_USB_CONF0    (*(volatile u32 *)(S3_USB_JTAG_BASE + 0x18u))

#define S3_SYSTEM_PERIP_CLK_EN1 (*(volatile u32 *)0x600C001Cu)
#define S3_SYSTEM_PERIP_RST_EN1 (*(volatile u32 *)0x600C0024u)
#define S3_USB_DEVICE_BIT       (1u << 10)
#define S3_RTC_USB_CONF         (*(volatile u32 *)0x60008120u)
#define S3_SYS_CLK_EN0          (*(volatile u32 *)0x600C0018u)
#define S3_TIMG0_BASE           0x6001F000u

void port_putc(int ch)
{
    u32 guard = 0;

    s3_mark(3u, 0x5555u);

    /* 上一笔还没发完（WR_DONE 仍为 1）时再写 FIFO 会被硬件丢弃，
       随后 DATA_FREE 永远不置位，串口在 ~50 字节处整体卡死。 */
    while ((S3_USB_EP1_CONF & 1u) != 0u) {
        guard++;
        if (guard > 2000000u) {
            return;
        }
    }
    guard = 0;
    while ((S3_USB_EP1_CONF & (1u << 1)) == 0u) {
        guard++;
        if (guard > 2000000u) {
            return; /* 不在 FIFO 上空转死等：丢掉这个字节 */
        }
    }
    S3_USB_EP1 = (u32)(ch & 0xff);
    S3_USB_EP1_CONF = 1u;
}

void port_puts(const char *s)
{
    while (*s != '\0') {
        port_putc((int)*s);
        s++;
    }
}

void port_delay_ms(u32 ms)
{
    volatile u32 i;
    while (ms != 0u) {
        for (i = 0; i < 40000u; i++) {
            __asm__ __volatile__("" ::: "memory");
        }
        ms--;
    }
}

static u32 s3_ccount(void)
{
    u32 v = 0u;
    __asm__ __volatile__("rsr %0, ccount" : "=r"(v));
    return v;
}

/* ROM/bootloader 留下的 TG0 看门狗约 3s 复位一次；关掉并在 idle 里复喂，
   即使关闭没生效也不会把应用打断。 */
void s3_wdt_kick(void)
{
    *(volatile u32 *)(S3_TIMG0_BASE + 0x64u) = 0x50D83AA1u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x48u) = 0u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x4Cu) = 0u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x64u) = 0u;
}

void port_idle(void)
{
    /* tick 已由 level-1 定时器中断推进；睡到下一次中断再回调度器。 */
    __asm__ __volatile__("waiti 0");
}

void port_init(void)
{
    volatile u32 i;

    s3_mark(0u, 0x1111u);

    /* 先确认 TIMG0 时钟，再关看门狗；否则启动约 300ms 就会被 TG0WDT 复位。 */
    S3_SYS_CLK_EN0 |= (1u << 13);
    *(volatile u32 *)(S3_TIMG0_BASE + 0x64u) = 0x50D83AA1u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x48u) = 0u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x4Cu) = 0u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x64u) = 0u;

    /* CPU 复位会把 USB-Serial/JTAG 的枚举打断，应用必须重新使能时钟和 PAD。
     * 只置位时钟、确保退出复位，不做 assert-reset（会再次掉枚举）。 */
    /* 内部 USB FSLS PHY 默认可能映射给 USB-OTG；USJ 要先抢回 PHY：
     * RTCCNTL.usb_conf.SW_HW_USB_PHY_SEL=1, SW_USB_PHY_SEL=0。 */
    S3_RTC_USB_CONF = (S3_RTC_USB_CONF & ~(1u << 19)) | (1u << 20);
    S3_SYSTEM_PERIP_CLK_EN1 |= S3_USB_DEVICE_BIT;
    S3_SYSTEM_PERIP_RST_EN1 &= ~S3_USB_DEVICE_BIT;
    S3_USB_CONF0 &= ~(1u << 16);
    S3_USB_CONF0 |= (1u << 14);
    S3_USB_EP1_CONF = 1u;

    /* 等主机重新枚举。 */
    for (i = 0; i < 2000000u; i++) {
        __asm__ __volatile__("" ::: "memory");
    }
    s3_mark(1u, 0x2222u);

    /* 等主机枚举 CDC-ACM；40MHz 下约 0.5s。 */
    for (i = 0; i < 1000000u; i++) {
        __asm__ __volatile__("" ::: "memory");
    }

    s3_mark(2u, 0x3333u);
    port_puts("c6os s3 boot\n");
    s3_irq_init();
}

void port_panic_halt(void)
{
    for (;;) {
        __asm__ __volatile__("waiti 0");
    }
}

u32 port_virtual_tick(void)
{
    return 0u;
}
