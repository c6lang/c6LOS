#include "c6os/types.h"
#include "c6os/tick.h"
#include "c6os/port.h"
#include "memmap.h"

extern void os_sched_tick(void);
extern void s3_wdt_kick(void);
extern void s3_mark(u32 idx, u32 v);
extern void *port_cur_ctx(void);
extern void port_set_cur_ctx(void *ctx);
extern void port_irq_fill(u32 frame_sp);
extern void *os_sched_preempt_ctx(void);

static void (*g_hook)(void);
static u32 g_isr_cc;

u32 s3_irq_cc(void)
{
    return g_isr_cc;
}

u32 s3_ccount_now(void)
{
    u32 cc = 0;

    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    return cc;
}

void s3_irq_set_hook(void (*fn)(void))
{
    g_hook = fn;
}

#define S3_IRQ_HZ 100u
#define S3_XTAL_HZ 40000000u

static u32 g_jlast;
static u32 g_jmin = 0xFFFFFFFFu;
static u32 g_jmax;
static u32 g_jsum;
static u32 g_jn;
static u32 g_jhist[16];

static void jitter_sample(u32 cc)
{
    if (g_jlast != 0u) {
        u32 d = cc - g_jlast;
        u32 dev = (d > 400000u) ? (d - 400000u) : (400000u - d);
        u32 b = dev / 2000u;

        if (b > 15u) {
            b = 15u;
        }
        g_jhist[b] = g_jhist[b] + 1u;
        if (d < g_jmin) {
            g_jmin = d;
        }
        if (d > g_jmax) {
            g_jmax = d;
        }
        g_jsum = g_jsum + d;
        g_jn = g_jn + 1u;
    }
    g_jlast = cc;
}

u32 os_jitter_min(void) { return g_jmin; }
u32 os_jitter_max(void) { return g_jmax; }
u32 os_jitter_count(void) { return g_jn; }
u32 os_jitter_avg(void) {
    return g_jn != 0u ? g_jsum / g_jn : 0u;
}
u32 os_jitter_p99(void)
{
    u32 i = 0;
    u32 acc = 0;
    u32 need = (g_jn * 99u) / 100u;

    while (i < 16u) {
        acc = acc + g_jhist[i];
        if (acc >= need) {
            return i * 2000u;
        }
        i++;
    }
    return 15u * 2000u;
}

volatile u32 g_sw_try;
volatile u32 g_sw_done;

static void s3_timer_rearm(void)
{
    u32 cc = 0;

    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    cc = cc + (S3_XTAL_HZ / S3_IRQ_HZ);
    __asm__ __volatile__("wsr %0, ccompare0" : : "r"(cc));
}

void s3_irq_init(void)
{
    u32 mask = (1u << 6);   /* XCHAL_TIMER0_INTERRUPT = 6, level 1 */
    u32 old = 0;
    s3_timer_rearm();
    __asm__ __volatile__("rsr %0, intenable" : "=r"(old));
    __asm__ __volatile__("wsr %0, intenable" : : "r"(old | mask));
#ifndef S3_IRQ_HOLD_OFF
    /* 排查用开关：定义 S3_IRQ_HOLD_OFF 时先不开中断（验证非中断路径） */
    __asm__ __volatile__("rsil a2, 0" : : : "a2");
#endif
}

static void trap_hex(u32 v)
{
    static const char h[] = "0123456789abcdef";
    int i = 28;

    while (i >= 0) {
        port_putc((int)h[(v >> i) & 0xFu]);
        i = i - 4;
    }
}

/* WiFi 闭源库通过我们的 osi_funcs 表回调；表里每个函数的调用次数记在
 * g_osi_calls[]。崩溃时在串口把它打出来，就知道库走到哪一步了。
 * 用 weak 引用，非 WiFi 构建时地址为 0，自动跳过。 */
extern volatile unsigned int g_osi_calls[128] __attribute__((weak));

void s3_trap(u32 frame_sp)
{
    u32 *f = (u32 *)frame_sp;
    u32 cause = 0;
    u32 pc = 0;
    u32 ps = 0;

    __asm__ __volatile__("rsr %0, exccause" : "=r"(cause));
    /* 直接读 EPC1（异常时它保存出错 PC），比从帧里取更可靠 */
    __asm__ __volatile__("rsr %0, epc1" : "=r"(pc));
    __asm__ __volatile__("rsr %0, ps" : "=r"(ps));
    (void)f;
    s3_mark(7u, 0xDEAD0000u | (cause & 0xFFFFu));

    port_puts("*** TRAP sp=");
    trap_hex(frame_sp);
    port_puts(" cause=");
    trap_hex(cause);
    port_puts(" pc=");
    trap_hex(pc);
    port_puts(" ps=");
    trap_hex(ps);
    {
        u32 va = 0;
        __asm__ __volatile__("rsr %0, excvaddr" : "=r"(va));
        port_puts(" vaddr=");
        trap_hex(va);
        port_puts(" ws=");
        trap_hex((ps >> 16) & 0xFu);
        port_puts(" a0=");
        trap_hex(((u32 *)frame_sp)[0]);
    }
    if (&g_osi_calls[0] != 0) {
        u32 i = 0;
        port_puts("\n  osi:");
        while (i < 128u) {
            if (g_osi_calls[i] != 0u) {
                port_puts(" "); trap_hex(i); port_puts("="); trap_hex(g_osi_calls[i]);
            }
            i++;
        }
    }
    port_puts("\n");
    /* 串口不可靠时用 LCD 背光(GPIO13)把异常码闪出来：
     *   先长亮 1 下（表示"进陷阱了"），然后慢慢闪 (cause + 1) 下。 */
    {
        u32 i = 0;
        u32 n = (cause & 0xFu) + 1u;

        *(volatile u32 *)(0x60009000u + 0x04u + 4u * 13u) =
            (*(volatile u32 *)(0x60009000u + 0x04u + 4u * 13u) & ~(7u << 12)) | (1u << 12);
        *(volatile u32 *)(0x60004000u + 0x554u + 4u * 13u) = 0x100u;
        *(volatile u32 *)(0x60004000u + 0x24u) = 1u << 13;

        *(volatile u32 *)(0x60004000u + 0x08u) = 1u << 13;      /* 长亮 */
        for (i = 0; i < 4000000u; i++) { }
        *(volatile u32 *)(0x60004000u + 0x0Cu) = 1u << 13;
        for (i = 0; i < 2000000u; i++) { }

        while (1) {
            i = 0;
            while (i < n) {
                *(volatile u32 *)(0x60004000u + 0x08u) = 1u << 13;
                { u32 k = 0; while (k < 1500000u) { k++; } }
                *(volatile u32 *)(0x60004000u + 0x0Cu) = 1u << 13;
                { u32 k = 0; while (k < 1500000u) { k++; } }
                i++;
            }
            { u32 k = 0; while (k < 8000000u) { k++; } }
        }
    }
    port_panic_halt();
}

/* 诊断：ISR 分段计数（RTC 0x50000710 + 4*i，每 256 次写一次）。
 * 1=port_cur_ctx 非零 2=port_irq_fill 返回 3=port_cur_ctx 为零
 * 4=port_isr_enter 返回 5=os_sched_tick 返回 */
static void s3_isr_mark(u32 i)
{
    static u32 m[8];
    volatile u32 *r = (volatile u32 *)0x50000710u;

    if (i < 8u) {
        m[i] = m[i] + 1u;
        if ((m[i] & 0xFFu) == 0u) { r[i] = m[i]; }
    }
}

void *s3_timer_irq(u32 frame_sp)
{
    u32 cc = 0;

    /* 记下被打断的主流程 PC（陷阱帧偏移 60 = epc1）：卡死时读 mark[3]
     * 就知道主流程停在哪条指令上。 */
    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    g_isr_cc = cc;
    {
        /* 诊断：把被打断的主流程 PC 采样进 RTC 0x50000780（每 256 次刷新一次）。
         * 卡死时读它就知道主流程停在哪条指令上。 */
        static u32 pc_n;
        static u32 pc_last;
        u32 ipc = 0;
        volatile u32 *pd = (volatile u32 *)0x50000780u;

        __asm__ __volatile__("rsr %0, epc1" : "=r"(ipc));
        pc_n = pc_n + 1u;
        if (ipc != pc_last) {
            pc_last = ipc;
            pd[0] = ipc;
            pd[1] = pd[1] + 1u;      /* PC 变化次数 */
        }
        if ((pc_n & 0xFFu) == 0u) { pd[2] = pc_n; }
    }
    jitter_sample(cc);
    s3_timer_rearm();
    s3_wdt_kick();
    {
        static u32 isr_n;
        volatile u32 *ic = (volatile u32 *)0x50000700u;
        isr_n = isr_n + 1u;
        /* 诊断：每 256 次中断把计数写进 RTC，确认定时器中断在不在跑 */
        if ((isr_n & 0xFFu) == 0u) {
            ic[0] = isr_n;
            ic[1] = ic[1] + 1u;
        }
    }
    if (port_cur_ctx() != 0) {
        s3_isr_mark(1u);
        port_irq_fill(frame_sp);
        s3_isr_mark(2u);
    } else {
        s3_isr_mark(3u);
    }
    port_isr_enter();
    s3_isr_mark(4u);
    {
        /* 诊断：记录进入 os_sched_tick 的次数（RTC 0x50000708），
         * 与 0x50000700（INR 计数）对比，判断 tick 推进是否被跳过。 */
        static u32 st_n;
        volatile u32 *ic = (volatile u32 *)0x50000708u;
        st_n = st_n + 1u;
        if ((st_n & 0xFFu) == 0u) { ic[0] = st_n; }
    }
    os_sched_tick();
    s3_isr_mark(5u);
    if (g_hook != 0) {
        g_hook();
    }
    port_isr_exit();
    {
        void *cur = port_cur_ctx();
        void *next = 0;

        if (cur == 0) {
            return 0;
        }
        next = os_sched_preempt_ctx();
        g_sw_try = g_sw_try + 1u;
        if (next == 0 || next == cur) {
            return 0;
        }
        g_sw_done = g_sw_done + 1u;
        port_set_cur_ctx(next);
        return next;
    }
}
