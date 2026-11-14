/* ESP32-S3 控制台：USB-Serial/JTAG（默认）。
 * 寄存器（IDF soc/esp32s3/register/soc/usb_serial_jtag_reg.h）：
 *   EP1_REG      +0x00  RDWR_BYTE：写 = 压 TX FIFO
 *   EP1_CONF_REG +0x04  bit1 SERIAL_IN_EP_DATA_FREE，bit0 WR_DONE
 *   CONF0_REG    +0x18  bit14 USB_PAD_ENABLE，bit16 USB_JTAG_BRIDGE_EN，bit0 PHY_SEL
 * 基址 0x60038000（TRM）。 */

#include "c6os/types.h"
#include "c6os/flash.h"
#include "c6os/port.h"
#include "c6os/tick.h"
#include "c6os/config.h"

extern void os_sched_tick(void);
extern void s3_irq_init(void);

/* 诊断：RTC 慢速内存（0x50000000，8KB）在 CPU 复位后保留，卡死时用
 * `esptool read-mem 0x50000000 0x40` 读回来，就能知道主流程/中断走到哪一步。 */
/* 诊断标记：RTC 慢速内存（0x50000000 起，IDF 里叫 RTC_DATA_ATTR 那一块），
 * CPU 复位后保留，卡死时用 esptool read-mem 0x50000000 读回。 */
void s3_mark(u32 idx, u32 v)
{
    *(volatile u32 *)(0x50000000u + idx * 4u) = v;
}

/* 读一个内存映射寄存器（c6 那边写指针强转不方便）。 */
u32 s3_reg(u32 addr)
{
    return *(volatile u32 *)addr;
}

u32 s3_mark_get(u32 idx)
{
    return *(volatile u32 *)(0x50000000u + idx * 4u);
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
}

#define S3_USB_JTAG_BASE 0x60038000u
#define S3_USB_EP1      (*(volatile u32 *)(S3_USB_JTAG_BASE + 0x00u))
#define S3_USB_EP1_CONF (*(volatile u32 *)(S3_USB_JTAG_BASE + 0x04u))
#define S3_USB_CONF0    (*(volatile u32 *)(S3_USB_JTAG_BASE + 0x18u))

#define S3_SYSTEM_PERIP_CLK_EN1 (*(volatile u32 *)0x600C001Cu)
#define S3_SYSTEM_PERIP_RST_EN1 (*(volatile u32 *)0x600C0024u)
#define S3_USB_DEVICE_BIT       (1u << 10)
#define S3_RTC_USB_CONF         (*(volatile u32 *)0x60008120u)

/* 启动进度标记（.data 里，复位后保留；见 port/s3/marks.c） */
extern volatile u32 g_boot_marks[8];
#define S3_SYS_CLK_EN0          (*(volatile u32 *)0x600C0018u)
#define S3_TIMG0_BASE           0x6001F000u

void port_putc(int ch)
{
    u32 guard = 0;


    /* 上一笔还没发完（WR_DONE 仍为 1）时再写 FIFO 会被硬件丢弃，
       随后 DATA_FREE 永远不置位，串口在 ~50 字节处整体卡死。 */
    /* 【重要】两个等待都必须**很短**：主机没在读串口时（比如板子独立跑，
     * 上位机没打开 /dev/ttyACM*），WR_DONE 不会自清、DATA_FREE 不置位，
     * 原来每个等待上限 2,000,000 次空转 ≈ 125ms，一个 20 字节的调试行就是
     * **5 秒**的阻塞 —— 表现就是"GUI 连续刷新几秒后画面定格"，而一旦上位机
     * 开始读串口，画面又"自己恢复了"。这就是那个"定格"的真正原因之一：
     * 打印把 GUI 任务卡住了，不是面板不再收数据。
     * 现在每次等待最多 ~1ms，仍然够 USB 满速帧（1ms/帧）跑完，但绝不会
     * 因为没人读串口而把业务任务卡住（丢几个字节无所谓）。 */
    while ((S3_USB_EP1_CONF & 1u) != 0u) {
        guard++;
        if (guard > 20000u) {
            return;
        }
    }
    guard = 0;
    while ((S3_USB_EP1_CONF & (1u << 1)) == 0u) {
        guard++;
        if (guard > 20000u) {
            return; /* 不在 FIFO 上空转死等：丢掉这个字节 */
        }
    }
    S3_USB_EP1 = (u32)(ch & 0xff);
    S3_USB_EP1_CONF = 1u;
}

static u32 g_rx_left[C6OS_MAX_CORES] = {0};

/* RX：数据必须 **32 位读** EP1_REG（8 位读恒为 0，实测），可读字节数在
 * OUT_EP1_ST[22:16]；读完清 INT_CLR bit2，否则同一批数据会被反复报告。
 * 一次中断往往带回 1~3 字节，用 per-core 剩余计数把它们逐字节吐出去。 */
int port_getc(void)
{
    u32 core = port_core_id() & (u32)(C6OS_MAX_CORES - 1);
    u32 *left = &g_rx_left[core];
    volatile u32 *ist = (volatile u32 *)(S3_USB_JTAG_BASE + 0x0Cu);   /* INT_ST */
    volatile u32 *iclr = (volatile u32 *)(S3_USB_JTAG_BASE + 0x14u);  /* INT_CLR */
    volatile u32 *ostat = (volatile u32 *)(S3_USB_JTAG_BASE + 0x3Cu); /* OUT_EP1_ST */

    if (*left == 0u) {
        if ((*ist & 0x4u) == 0u) {
            return -1;
        }
        *left = (*ostat >> 16) & 0x7Fu;
        if (*left == 0u) {
            *iclr = 0x4u;
            return -1;
        }
    }
    *left = *left - 1u;
    {
        int c = (int)(S3_USB_EP1 & 0xFFu);
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

u32 s3_prid_raw(void)
{
    u32 v = 0u;
    __asm__ __volatile__("rsr.prid %0" : "=r"(v));
    return v;
}

/* PRID 基线：这颗 S3 上 `rsr.prid` 读回来的不是 0/1（实测 core0=0xCDCD、
 * core1=0xABAB），但**每核稳定且互不相同**，所以启动时让每个核把自己的
 * PRID 值登记到槽位，之后按值查表就知道"我在哪个核"。 */
/* ctx.S 里直接拿它做比较：切换路径上不能调用 C 函数——本工具的 C 是 windowed
 * ABI，`callx0` 进 C 函数不会转窗口，会把 a12-a15 冲掉（曾导致切换时 a4 变成
 * 脏指针、L32I 触发 LoadStoreAlignment）。 */
u32 g_prid_base[C6OS_MAX_CORES] = {0xFFFFFFFFu, 0xFFFFFFFFu};

void port_core_bind_self(void)
{
    u32 v = s3_prid_raw();
    u32 i = 0;

    while (i < (u32)C6OS_MAX_CORES) {
        if (g_prid_base[i] == v) { return; }
        i++;
    }
    i = 0;
    while (i < (u32)C6OS_MAX_CORES) {
        if (g_prid_base[i] == 0xFFFFFFFFu) {
            g_prid_base[i] = v;
            return;
        }
        i++;
    }
}

u32 port_core_id(void)
{
    u32 v = s3_prid_raw();
    u32 i = 0;

    while (i < (u32)C6OS_MAX_CORES) {
        if (g_prid_base[i] == v) { return i; }
        i++;
    }
    return 0u;   /* 还没登记：按 0 号核处理 */
}

void port_init(void)
{
    s3_beacon(3u, 0xB0B0B0B0u);   /* 进了 port_init */
    volatile u32 i;

    /* 启动阶段标记（RTC_SLOW_MEM）：万一启动期死掉，复位后 esptool read-mem
     * 0x50000008 就能看出死在 port_init 的第几步（0xB0 进函数 / 0xB1 关完看门狗 /
     * 0xB2 USB 就绪 / 0xB3 打完 banner）。start.S 里另有 0xA0/0xA1 两个标记。 */
    g_boot_marks[2] = 0xB0u;

    /* 如果镜像里有 flash 段（IROM），在这里把 MMU 映射建好、打开 I-cache ——
     * 没有二级 bootloader 时 ROM 不会做这件事，不建映射 flash 里的代码就取指失败。 */
    os_flash_init();

    /* 先确认 TIMG0 时钟，再关看门狗；否则启动约 300ms 就会被 TG0WDT 复位。 */
    S3_SYS_CLK_EN0 |= (1u << 13);
    *(volatile u32 *)(S3_TIMG0_BASE + 0x64u) = 0x50D83AA1u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x48u) = 0u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x4Cu) = 0u;
    *(volatile u32 *)(S3_TIMG0_BASE + 0x64u) = 0u;

    /* RTC 看门狗同样要关：esptool 的 --after watchdog-reset 会把 RTC_WDT 配好
     * 再复位，配置留在 RTC 域里，应用不关它就会每隔几秒被整片复位（表现为
     * 只打印一行 banner 就掉 USB）。 */
    *(volatile u32 *)0x600080B0u = 0x50D83AA1u;   /* RTC_CNTL_WDTWPROTECT */
    *(volatile u32 *)0x60008098u = 0u;            /* RTC_CNTL_WDTCONFIG0 */
    *(volatile u32 *)0x600080B0u = 0u;

    /* 还有 RTC 超级看门狗（SWD）：ROM/esptool 会给它装一个几秒的超时，应用不关
     * 就每 ~3.5s 被整片复位一次（实测 RTC_CNTL_RESET_CAUSE_PROCPU=0x12
     * SYS_SUPER_WDT，表现是串口只能看到头几行、每隔几秒 USB 重新枚举）。
     * SWD_CONF.SWD_DISABLE=bit30，写保护钥匙在 SWD_WPROTECT。 */
    *(volatile u32 *)0x600080B8u = 0x8F1D312Au;   /* RTC_CNTL_SWD_WPROTECT */
    *(volatile u32 *)0x600080B4u |= (1u << 30);   /* SWD_DISABLE = 1 */
    *(volatile u32 *)0x600080B8u = 0u;
    g_boot_marks[3] = 0xB1u;   /* 三个看门狗都关了 */

#ifndef S3_NO_USB_REINIT
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

    /* 等主机枚举 CDC-ACM；40MHz 下约 0.5s。 */
    for (i = 0; i < 1000000u; i++) {
        __asm__ __volatile__("" ::: "memory");
    }

    g_boot_marks[4] = 0xB2u;   /* USB 已重新使能 */
#endif /* S3_NO_USB_REINIT */

    port_core_bind_self();   /* 先登记本核 PRID，后面 port_core_id() 才有意义 */
    port_puts("c6os s3 boot\n");
    s3_irq_init();
    g_boot_marks[5] = 0xB3u;
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
