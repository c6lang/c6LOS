/* ESP32-S3 控制台：USB-Serial/JTAG（默认）。
 * 寄存器（IDF soc/esp32s3/register/soc/usb_serial_jtag_reg.h）：
 *   EP1_REG      +0x00  RDWR_BYTE：写 = 压 TX FIFO
 *   EP1_CONF_REG +0x04  bit1 SERIAL_IN_EP_DATA_FREE，bit0 WR_DONE
 *   CONF0_REG    +0x18  bit14 USB_PAD_ENABLE，bit16 USB_JTAG_BRIDGE_EN，bit0 PHY_SEL
 * 基址 0x60038000（TRM）。 */

#include "c6os/types.h"
#include "c6os/port.h"

/* 诊断：RTC_CNTL STORE0..7 在复位后保留，esptool read-mem 0x60008050 可读。 */
static void s3_mark(u32 idx, u32 v)
{
    *(volatile u32 *)(0x60008050u + idx * 4u) = v;
    *(volatile u32 *)(0x3FCF0000u + idx * 4u) = v;
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

void port_putc(int ch)
{
    u32 guard = 0;

    s3_mark(3u, 0x5555u);

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

void port_idle(void)
{
    __asm__ __volatile__("waiti 0");
}

void port_init(void)
{
    volatile u32 i;

    s3_mark(0u, 0x1111u);

    /* 打开 USB-Serial/JTAG 模块时钟，复位一次，再打默认 PHY/PAD。 */
    S3_SYSTEM_PERIP_CLK_EN1 |= S3_USB_DEVICE_BIT;
    S3_SYSTEM_PERIP_RST_EN1 |= S3_USB_DEVICE_BIT;
    S3_SYSTEM_PERIP_RST_EN1 &= ~S3_USB_DEVICE_BIT;
    S3_USB_CONF0 &= ~(1u << 16); /* 断开 JTAG 桥 */
    S3_USB_CONF0 &= ~(1u << 0);  /* 内部 PHY */
    S3_USB_CONF0 |= (1u << 14);  /* USB pad enable */
    s3_mark(1u, 0x2222u);

    /* 等主机枚举 CDC-ACM；40MHz 下约 0.5s。 */
    for (i = 0; i < 20000000u; i++) {
        __asm__ __volatile__("" ::: "memory");
    }

    s3_mark(2u, 0x3333u);
    port_puts("c6os s3 boot\n");
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

u32 port_task_stack_ok(void *ctx)
{
    (void)ctx;
    return 1u;
}
