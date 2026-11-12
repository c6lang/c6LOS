#include "c6os/types.h"
#include "c6os/tick.h"
#include "c6os/port.h"

extern void os_sched_tick(void);
extern void s3_wdt_kick(void);
extern void s3_mark(u32 idx, u32 v);

#define S3_IRQ_HZ 100u
#define S3_XTAL_HZ 40000000u

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

void s3_timer_irq(void)
{
    s3_timer_rearm();
    s3_wdt_kick();
    s3_mark(6u, (u32)os_tick_get());
    os_sched_tick();
}
