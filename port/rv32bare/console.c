#include "c6os/types.h"
#include "c6os/port.h"

#define BARE_CONSOLE 0x40000000u
#define BARE_EXIT    0x40000004u

extern void rv_bare_trap(void);
extern void os_sched_tick(void);

static volatile u32 g_isr_nest = 0;

void rv_bare_tick_irq(void)
{
    g_isr_nest = g_isr_nest + 1u;
    os_sched_tick();
    if (g_isr_nest != 0u) {
        g_isr_nest = g_isr_nest - 1u;
    }
}
extern u32 risc6_trap_save[];

u32 port_core_id(void)
{
    u32 v;
    __asm__ __volatile__("csrr %0, mhartid" : "=r"(v));
    return v;
}

void port_init(void)
{
    u32 t = 0x80u;

    __asm__ __volatile__("csrw mtvec, %0" : : "r"(&rv_bare_trap));
    __asm__ __volatile__("csrw mscratch, %0" : : "r"(&risc6_trap_save[port_core_id() * 40u]));
    __asm__ __volatile__("csrs mie, %0" : : "r"(t));
    __asm__ __volatile__("csrsi mstatus, 8");
}

void port_putc(int ch)
{
    *(volatile u32 *)BARE_CONSOLE = (u32)(u8)ch;
}

void port_puts(const char *s)
{
    while (*s != '\0') {
        port_putc((int)*s);
        s++;
    }
}

int port_getc(void)
{
    return -1;
}

void port_idle(void)
{
    __asm__ __volatile__("nop");
}

void port_bare_exit(u32 code)
{
    *(volatile u32 *)BARE_EXIT = code & 0xFFu;
    for (;;) {
    }
}

void port_reboot(void)
{
    *(volatile u32 *)BARE_EXIT = 1u;
    for (;;) {
    }
}

void port_panic_halt(void)
{
    *(volatile u32 *)BARE_EXIT = 2u;
    for (;;) {
    }
}

void port_qemu_exit(void)
{
    port_panic_halt();
}

void port_mark(u32 v)
{
    (void)v;
}

u64 port_cycles(void)
{
    u32 v = 0;
    __asm__ __volatile__("rdcycle %0" : "=r"(v));
    return (u64)v;
}

u32 port_atomic_cas(volatile u32 *p, u32 expect, u32 newv)
{
    if (p == 0) {
        return 0u;
    }
    return __sync_bool_compare_and_swap(p, expect, newv) ? 1u : 0u;
}

u32 port_in_isr(void)
{
    return g_isr_nest;
}

void port_isr_enter(void)
{
    g_isr_nest = g_isr_nest + 1u;
}

void port_isr_exit(void)
{
    if (g_isr_nest != 0u) {
        g_isr_nest = g_isr_nest - 1u;
    }
}

void os_test_set_isr(u32 v)
{
    (void)v;
}

void port_enter_task(void)
{
}

void port_leave_task(void)
{
}

u32 port_virtual_tick(void)
{
    return 0u;
}

void port_delay_ms(u32 ms)
{
    volatile u32 i;
    while (ms != 0u) {
        for (i = 0; i < 40000u; i++) {
        }
        ms--;
    }
}

u64 port_wallclock_sec(void)
{
    return 0ull;                       /* 没有 RTC：回退 TSC 假时钟 */
}
