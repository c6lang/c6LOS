#include "c6os/types.h"
#include "c6os/port.h"

#define COM1 0x3F8u

static void outb(u32 port, u8 v)
{
    __asm__ __volatile__("outb %0, %w1" : : "a"(v), "Nd"(port));
}

static u8 inb(u32 port)
{
    u8 v;
    __asm__ __volatile__("inb %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void port_init(void)
{
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x80u);
    outb(COM1 + 0u, 0x01u);
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x03u);
    outb(COM1 + 2u, 0xC7u);
    outb(COM1 + 4u, 0x0Bu);
}

void port_putc(int ch)
{
    u32 guard = 0;

    while ((inb(COM1 + 5u) & 0x20u) == 0u) {
        guard++;
        if (guard > 2000000u) {
            return;
        }
    }
    outb(COM1, (u8)(ch & 0xff));
}

void port_puts(const char *s)
{
    while (*s != '\0') {
        port_putc((int)*s);
        s++;
    }
}

extern void os_sched_tick(void);

static u64 g_last_tsc;

static u64 rdtsc(void)
{
    u32 lo;
    u32 hi;

    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

void port_idle(void)
{
    /* 裸机没接中断：用 rdtsc 忙等出一个 tick，不能 hlt（没有唤醒源）。 */
    for (;;) {
        u64 now = rdtsc();

        if ((now - g_last_tsc) >= 10000000ull) {
            g_last_tsc = now;
            os_sched_tick();
            return;
        }
        __asm__ __volatile__("pause");
    }
}

void port_panic_halt(void)
{
    __asm__ __volatile__("cli");
    while (1) {
        __asm__ __volatile__("hlt");
    }
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
    u64 end = rdtsc() + (u64)ms * 1000000ull;

    while (rdtsc() < end) {
        __asm__ __volatile__("pause");
    }
}
