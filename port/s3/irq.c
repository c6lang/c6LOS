#include "c6os/types.h"
#include "c6os/tick.h"
#include "c6os/port.h"

extern void os_sched_tick(void);
extern void s3_wdt_kick(void);
extern void s3_mark(u32 idx, u32 v);
extern void *port_cur_ctx(void);
extern void port_set_cur_ctx(void *ctx);
extern void port_irq_fill(u32 frame_sp);
extern void *os_sched_preempt_ctx(void);

static void (*g_hook)(void);
static u32 g_isr_cc;

void s3_irq_set_hook(void (*fn)(void))
{
    g_hook = fn;
}

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
    s3_timer_rearm();
    __asm__ __volatile__("wsr %0, intenable" : : "r"(mask));
    __asm__ __volatile__("rsil a2, 0" : : : "a2");
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

void s3_trap(u32 frame_sp)
{
    u32 *f = (u32 *)frame_sp;
    u32 cause = 0;
    u32 pc = 0;
    u32 ps = 0;

    __asm__ __volatile__("rsr %0, exccause" : "=r"(cause));
    if (frame_sp != 0u) {
        pc = f[15];
        ps = f[16];
    }
    s3_mark(7u, 0xDEAD0000u | (cause & 0xFFFFu));
    port_puts("*** TRAP cause=");
    trap_hex(cause);
    port_puts(" pc=");
    trap_hex(pc);
    port_puts(" ps=");
    trap_hex(ps);
    port_puts("\n");
    port_panic_halt();
}

void *s3_timer_irq(u32 frame_sp)
{
    u32 cc = 0;

    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    g_isr_cc = cc;
    jitter_sample(cc);
    s3_timer_rearm();
    s3_wdt_kick();
    s3_mark(6u, (u32)os_tick_get());
    if (port_cur_ctx() != 0) {
        port_irq_fill(frame_sp);
    }
    os_sched_tick();
    {
        void *cur = port_cur_ctx();
        void *next = 0;

        if (cur == 0) {
            return 0;
        }
        next = os_sched_preempt_ctx();
        if (next == 0 || next == cur) {
            return 0;
        }
        port_set_cur_ctx(next);
        return next;
    }
}
