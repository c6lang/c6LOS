#include "c6os/types.h"
#include "c6os/port.h"

static long sys_write(long fd, const void *buf, unsigned long n)
{
    register long a0 __asm__("a0") = fd;
    register long a1 __asm__("a1") = (long)buf;
    register long a2 __asm__("a2") = (long)n;
    register long a7 __asm__("a7") = 64;

    __asm__ __volatile__("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static void sys_exit(long code)
{
    register long a0 __asm__("a0") = code;
    register long a7 __asm__("a7") = 93;

    __asm__ __volatile__("ecall" : : "r"(a0), "r"(a7) : "memory");
    for (;;) {
    }
}

u32 port_core_id(void)
{
    return 0u;
}

void port_init(void)
{
}

int port_getc(void)
{
    return -1;
}

void port_putc(int ch)
{
    char c = (char)ch;

    (void)sys_write(1, &c, 1);
}

void port_puts(const char *s)
{
    while (*s != '\0') {
        port_putc((int)*s);
        s++;
    }
}

extern void os_sched_tick(void);

static u32 g_last_time;

static u32 rdtime32(void)
{
    u32 v;

    __asm__ __volatile__("csrr %0, time" : "=r"(v));
    return v;
}

void port_idle(void)
{
    u32 now = rdtime32();

    if ((u32)(now - g_last_time) >= 1000000u) {
        g_last_time = now;
        os_sched_tick();
    }
}

void port_reboot(void)
{
    for (;;) {
    }
}

void port_panic_halt(void)
{
    sys_exit(1);
}

static volatile u32 g_isr_nest = 0;

u32 port_in_isr(void)
{
    return g_isr_nest;
}

void port_isr_enter(void)
{
    g_isr_nest = g_isr_nest + 1u;
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

void port_mark(u32 v)
{
    (void)v;
}

u32 port_virtual_tick(void)
{
    return 0;
}

void port_delay_ms(u32 ms)
{
    (void)ms;
}
