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

void s3_mark2(u32 idx, u32 v) { s3_mark(idx, v); }

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

extern int s3_ets_printf_call0(const char *fmt, ...);

/* 诊断快照：在空闲循环里周期性地把关键计数/记录一次性打到串口。
 * 这样得到的是**同一次启动**的完整快照 —— esptool read-mem 每次都会复位，
 * 逐字段读出来的值其实来自不同次启动，之前多次被这一点误导。 */
/* 一次性诊断快照：把关键计数与记录整体写进 RTC 0x50001D00。
 * esptool read-mem 会先复位再读，所以读到的是**上一次启动稳定后**的定格；
 * 逐字段分多次 read-mem 得到的是不同启动的值，不可比。 */
__attribute__((section(".flash.text"))) void port_switch_trace(u32 tag, u32 id, u32 ctx)
{
    volatile u32 *t = (volatile u32 *)0x50001500u;
    static u32 n_;
    u32 k_ = n_ & 7u;
    n_ = n_ + 1u;
    t[k_ * 4u]      = tag;
    t[k_ * 4u + 1u] = id;
    t[k_ * 4u + 2u] = ctx;
    t[k_ * 4u + 3u] = n_;
#ifdef S3_ONE_ABI
    /* §892: 全 windowed ABI 下本函数若连续 callx8 到 port_putc 30 次，
     * callx8 不轮转窗口、port_putc 的 entry 反复压同一帧 → 只打印出第一个
     * 'S' 就崩。改成**只写 RTC 环形区、完全不打印**（RTC 可由 JTAG 读）。 */
    (void)tag; (void)id; (void)ctx;
#endif
}

__attribute__((section(".flash.text"))) void port_snapshot(void)
{
    volatile u32 *sn = (volatile u32 *)0x50001D00u;
    volatile u32 *b = (volatile u32 *)0x50000C00u;
    volatile u32 *y = (volatile u32 *)0x50001800u;
    u32 i = 0;

    sn[0]  = 0x5A5A5A5Au;
    sn[1]  = (u32)os_tick_get();
    sn[2]  = *(volatile u32 *)0x50001000u;
    sn[3]  = *(volatile u32 *)0x50000200u;
    sn[4]  = y[7];
    sn[5]  = y[4];
    sn[6]  = y[5];
    sn[7]  = *(volatile u32 *)0x50001600u;
    while (i < 20u) { sn[8u + i] = b[i]; i = i + 1u; }
    sn[28] = *(volatile u32 *)0x3fcef904u;
    sn[29] = *(volatile u32 *)0x50000D00u;
    sn[30] = *(volatile u32 *)0x50000D04u;
    sn[31] = *(volatile u32 *)0x50000D08u;
    sn[32] = *(volatile u32 *)0x50000D0Cu;
    sn[33] = *(volatile u32 *)0x50000D10u;
    sn[34] = *(volatile u32 *)0x50000D14u;
    i = 0;
    while (i < 32u) {
        sn[40u + i] = *(volatile u32 *)(0x50001500u + i * 4u);
        i = i + 1u;
    }
}

__attribute__((section(".flash.text"))) void port_diag_dump(void)
{
    volatile u32 *y  = (volatile u32 *)0x50001800u;
    volatile u32 *b  = (volatile u32 *)0x50000C00u;
    volatile u32 *st = (volatile u32 *)0x50000A00u;

    s3_ets_printf_call0("D1 starts=%u switch=%u yield=%u ylctx=%08x ylsp=%08x ylcaller=%08x\n",
               (unsigned)st[0], (unsigned)*(volatile u32 *)0x50001000u,
               (unsigned)y[7], (unsigned)y[4], (unsigned)y[5], (unsigned)y[8]);
    s3_ets_printf_call0("D2 blk0=%08x/%u/%08x blk1=%08x/%u/%08x\n",
               (unsigned)b[0], (unsigned)b[1], (unsigned)b[2],
               (unsigned)b[4], (unsigned)b[5], (unsigned)b[6]);
    s3_ets_printf_call0("D3 blk2=%08x/%u/%08x blk3=%08x/%u/%08x\n",
               (unsigned)b[8], (unsigned)b[9], (unsigned)b[10],
               (unsigned)b[12], (unsigned)b[13], (unsigned)b[14]);
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
/* §442：**初值必须是这颗芯片真实的 PRID，不能是 0xFFFFFFFF**。
 *
 * `port/s3/ctx.S` 里 `port_task_switch` / `port_task_yield` 判断核心号的
 * 汇编是：
 *
 *     rsr  a5, prid ; l32r a3, .Lprid_base ; l32i a6, a3, 0
 *     movi a11, 0   ; beq  a5, a6, 2f      ; movi a11, 1
 *
 * —— **只比较 [0]，不相等就直接用索引 1**，没有 C 版 `port_core_id()`
 * 那个"还没登记就返回 0"的兜底。
 *
 * 而 ESP32-S3 的 `prid` 是脏值（实测 **0x0000cdcd**）。于是初值 0xFFFFFFFF
 * 永远不相等 ⇒ 所有 yield/switch 都去操作 `g_s3_cur[1]`，那里是 0 ⇒
 * `beqz a4, 1f` 直接跳过保存 ⇒ **任务帧一次都存不下来**，
 * `ctx[0].sp` 永远停在 `port_task_ctx_create` 造的那个 tramp 帧上。
 *
 * 用实测值做初值，单核场景下比较必然相等 ⇒ 索引 0 ⇒ 正确。 */
u32 g_prid_base[C6OS_MAX_CORES] = {0x0000CDCDu, 0xFFFFFFFFu};

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
#ifndef S3_SKIP_FLASH_INIT
    os_flash_init();
#endif

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
    /* RTC 慢存跨 CPU 复位是**保留**的，诊断区若不清零，读回来的就是上一次
     * （甚至更早）启动的残留 —— 我们已经被这一点误导过：两处"库侧返回地址"
     * 看起来都落在 ppTask 里，其实是前一次启动写下的。所以启动时统一清掉。 */
    {
        static const u32 diag_base[8] = { 0x50000200u, 0x50000300u, 0x500003C0u,
                                          0x50000A00u, 0x50000C00u, 0x50000F00u,
                                          0x50001540u, 0x50001C00u };
        static const u32 diag_words[8] = { 65u, 16u, 16u, 16u, 20u, 24u, 24u, 40u };
        u32 i;
        u32 j;

        for (i = 0u; i < 8u; i++) {
            volatile u32 *p = (volatile u32 *)diag_base[i];

            for (j = 0u; j < diag_words[i]; j++) { p[j] = 0u; }
        }
    }
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

/* §964：延时 tick 的待处理计数。定时器 ISR 每次 +1（纯内存操作），
 * 调度主循环在这里消费，然后**在任务上下文里**调 os_sched_tick()。
 *
 * 为什么不在 ISR 里直接调：真板实测 ISR 里
 * s3_timer_irq → s3_win_tramp → os_sched_tick 是第 3~4 次窗口旋转，
 * 到 os_sched_tick 的 `entry a1,144` 就抛 cause=0 IllegalInstruction
 * （EPC1 = 入口，PS=0x00060031：EXCM=1、WOE=0）。放到任务上下文里，
 * 窗口深度归零，这条链就没有问题了。
 *
 * 用 RTC 慢速内存而不是 .bss：当前布局下 _stack_limit 只比 _stack_top
 * 低 10 字节，一个 u32 都塞不进去。0x50001580 这段没别的用处。 */
#define S3_TICK_PEND (*(volatile u32 *)0x50001580u)

u32 port_virtual_tick(void)
{
    if (S3_TICK_PEND == 0u) {
        return 0u;
    }
    S3_TICK_PEND = S3_TICK_PEND - 1u;
    return 1u;
}

/* ---- 诊断输出：纯 call0，不经过 windowed 岛 ------------------------------
 * 之前把诊断打印放在 windowed 岛里，调用链一深就在 retw.n 上抛
 * cause=0 IllegalInstruction（窗口用尽），读数时有时无。改到这里之后
 * 整条链都是 call0，没有窗口轮转，稳定得多。
 */
extern u32 os_task_probe(u32 id, u32 what);
extern void os_task_delay(u32 ticks);

static char g_pd_buf[384];

__attribute__((section(".flash.text")))
static char *pd_s(char *o, const char *s)
{
    while (*s != 0) { *o++ = *s++; }
    return o;
}

__attribute__((section(".flash.text")))
static char *pd_x(char *o, u32 v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        u32 d = (v >> (u32)(i * 4)) & 0xFu;

        *o++ = (char)((d < 10u) ? ('0' + d) : ('a' + (d - 10u)));
    }
    return o;
}

/* which: 0=信号量环 1=任务状态 2=阻塞环 3=OSI 调用序列 */
__attribute__((section(".flash.text")))
void port_diag_ring(u32 which)
{
    char *o = g_pd_buf;
    u32 i;
    volatile u32 *p;

    if (which == 0u) {
        volatile u32 *sem = (volatile u32 *)0x50001C00u;

        o = pd_s(o, "[sem]");
        for (i = 0u; i < 8u; i++) {
            if (sem[i * 5u] == 0u) { continue; }
            o = pd_s(o, " op="); o = pd_x(o, sem[i * 5u + 1u]);
            o = pd_s(o, " o=");  o = pd_x(o, sem[i * 5u + 2u]);
            o = pd_s(o, " t=");  o = pd_x(o, sem[i * 5u + 3u]);
            o = pd_s(o, " r=");  o = pd_x(o, sem[i * 5u + 4u]);
        }
        o = pd_s(o, "\n");
        *o = 0;
        port_puts(g_pd_buf);
        return;
    }
    if (which == 1u) {
        o = pd_s(o, "[task]");
        for (i = 0u; i < 5u; i++) {
            u32 st = os_task_probe(i, 0u);

            if (st == 0xFFFFFFFFu) { continue; }
            o = pd_s(o, " t"); o = pd_x(o, i);
            o = pd_s(o, " s="); o = pd_x(o, st);
            o = pd_s(o, "/");  o = pd_x(o, os_task_probe(i, 1u));
            o = pd_s(o, "/");  o = pd_x(o, os_task_probe(i, 2u));
        }
        o = pd_s(o, "\n");
        *o = 0;
        port_puts(g_pd_buf);
        return;
    }
    if (which == 2u) {
        volatile u32 *blk = (volatile u32 *)0x50000C00u;

        o = pd_s(o, "[blk]");
        for (i = 0u; i < 4u; i++) {
            if (blk[i * 5u + 1u] == 0u) { continue; }
            o = pd_s(o, " o=");  o = pd_x(o, blk[i * 5u]);
            o = pd_s(o, " k=");  o = pd_x(o, blk[i * 5u + 1u]);
            o = pd_s(o, " c=");  o = pd_x(o, blk[i * 5u + 2u]);
        }
        o = pd_s(o, "\n");
        *o = 0;
        port_puts(g_pd_buf);
        return;
    }
    {
        volatile u32 *cnt = (volatile u32 *)0x50000200u;
        u32 n = *cnt;

        p = (volatile u32 *)0x50000204u;
        if (n > 28u) { n = 28u; }
        o = pd_s(o, "[osi] n=");
        o = pd_x(o, *cnt);
        for (i = 0u; i < n; i++) {
            if ((i & 7u) == 0u) { o = pd_s(o, "\n  "); }
            o = pd_x(o, p[i]);
            *o++ = ' ';
        }
        o = pd_s(o, "\n");
        *o = 0;
        port_puts(g_pd_buf);
    }
}

/* 低优先级监控任务：每次只打一类，避免缓冲/栈压力 */
__attribute__((section(".flash.text")))
void port_diag_task(void *arg)
{
    u32 ph = 0u;

    (void)arg;
    for (;;) {
        os_task_delay(90u);
        port_diag_ring(ph & 3u);
        ph = ph + 1u;
    }
}
