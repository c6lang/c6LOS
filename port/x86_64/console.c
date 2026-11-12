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

void port_idle(void)
{
    __asm__ __volatile__("hlt");
}
