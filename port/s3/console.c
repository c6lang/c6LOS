/* ESP32-S3 控制台：USB-Serial/JTAG（默认）。
 * 寄存器（IDF soc/esp32s3/register/soc/usb_serial_jtag_reg.h）：
 *   EP1_REG      +0x00  RDWR_BYTE：写 = 压 TX FIFO
 *   EP1_CONF_REG +0x04  bit1 SERIAL_IN_EP_DATA_FREE，bit0 WR_DONE
 *   CONF0_REG    +0x18  bit14 USB_PAD_ENABLE，bit16 USB_JTAG_BRIDGE_EN，bit0 PHY_SEL
 * 基址 0x60038000（TRM）。 */

#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/tick.h"
#include "c6os/config.h"

extern void os_sched_tick(void);
extern void s3_irq_init(void);

/* 诊断：RTC_CNTL STORE0..7 在复位后保留，esptool read-mem 0x60008050 可读。 */
void s3_mark(u32 idx, u32 v)
{
    *(volatile u32 *)(0x3FCF0000u + idx * 4u) = v;
}

static volatile u32 g_isr_nest[C6OS_MAX_CORES] = {0};

u64 port_cycles(void)
{
    u32 v = 0;
    __asm__ __volatile__("rsr %0, ccount" : "=r"(v));
    return (u64)v;
}

u32 port_atomic_cas(volatile u32 *p, u32 expect, u32 newv)
{
    (void)p;
    (void)expect;
    (void)newv;
    /* 单核 S3：对象锁退化为“总是成功”，避免 S32C1I/非对齐访问陷阱。
     * 双核 SMP 需要后续用正确的 S32C1I 汇编实现。 */
    return 1u;
}

u32 port_in_isr(void)
{
    return g_isr_nest[port_core_id() & (u32)(C6OS_MAX_CORES - 1)];
}

void port_isr_enter(void)
{
    u32 core = port_core_id() & (u32)(C6OS_MAX_CORES - 1);
    g_isr_nest[core] = g_isr_nest[core] + 1u;
}

void port_isr_exit(void)
{
    u32 core = port_core_id() & (u32)(C6OS_MAX_CORES - 1);
    if (g_isr_nest[core] != 0u) {
        g_isr_nest[core] = g_isr_nest[core] - 1u;
    }
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

static u32 g_rx_left[C6OS_MAX_CORES] = {0};

int port_getc(void)
{
    u32 core = port_core_id() & (u32)(C6OS_MAX_CORES - 1);
    u32 *left = &g_rx_left[core];
    volatile u32 *st = (volatile u32 *)(0x6003803Cu);
    volatile u32 *ist = (volatile u32 *)(0x6003800Cu);
    volatile u32 *iclr = (volatile u32 *)(0x60038014u);
    volatile u32 *word = (volatile u32 *)(0x60038000u);

    if (left == 0u) {
        if ((*ist & 0x4u) == 0u) {
            return -1;
        }
        *left = (*st >> 16) & 0x7Fu;
        if (*left == 0u) {
            *iclr = 0x4u;
            return -1;
        }
    }
    *left = *left - 1u;
    {
        int c = (int)(*word & 0xFFu);
        if (*left == 0u) {
            *iclr = 0x4u;
        }
        return c;
    }
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
    /* 核 0 有 level-1 定时器中断，可 waiti；核 1 暂无 IPI，忙轮询才能
     * 看到别的核投递的任务/信号量唤醒。 */
    if (port_core_id() != 0u) {
        __asm__ __volatile__("nop");
        return;
    }
    __asm__ __volatile__("waiti 0");
}

u32 port_core_id(void)
{
    /* 单核 S3 阶段固定 0；PRID 读取路径在 LX7 上待验证。 */
    return 0u;
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
    *(volatile u32 *)(0x60038010u) = 0x4u;   /* INT_ENA: OUT packet */
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

void port_reboot(void)
{
    /* RTC_CNTL_OPTIONS0.SW_SYS_RST */
    *(volatile u32 *)(0x60008000u) |= (1u << 31);
    for (;;) {
        __asm__ __volatile__("waiti 0");
    }
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
