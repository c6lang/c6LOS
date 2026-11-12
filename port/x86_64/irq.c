#include "c6os/types.h"
#include "c6os/port.h"

typedef struct {
    u16 off_lo;
    u16 sel;
    u8 ist;
    u8 type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed)) IdtEntry;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) IdtPtr;

static IdtEntry g_idt[256];
static IdtPtr g_idtp;

u32 g_in_task;
u32 g_in_isr;
/* 无内核的 hello 载荷也链接本文件：弱符号兜底。 */
__attribute__((weak)) void *g_cur_ctx;
__attribute__((weak)) void *g_sched_sp;
__attribute__((weak)) void os_sched_tick(void) {}

extern void irq0_stub(void);

static void outb(u16 port, u8 v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static u8 inb(u16 port)
{
    u8 v;

    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void gate(int n, void (*fn)(void))
{
    u64 a = (u64)fn;

    g_idt[n].off_lo = (u16)(a & 0xFFFFu);
    g_idt[n].sel = 0x08u;
    g_idt[n].ist = 0;
    g_idt[n].type = 0x8Eu;
    g_idt[n].off_mid = (u16)((a >> 16) & 0xFFFFu);
    g_idt[n].off_hi = (u32)(a >> 32);
    g_idt[n].zero = 0;
}

static void pic_remap(void)
{
    outb(0x20u, 0x11u);
    outb(0xA0u, 0x11u);
    outb(0x21u, 0x20u);
    outb(0xA1u, 0x28u);
    outb(0x21u, 0x04u);
    outb(0xA1u, 0x02u);
    outb(0x21u, 0x01u);
    outb(0xA1u, 0x01u);
    outb(0x21u, 0xFEu);   /* 只开 IRQ0 */
    outb(0xA1u, 0xFFu);
}

static void pit_init(void)
{
    u32 div = 11931u;     /* 1193182 / 100 */

    outb(0x43u, 0x36u);
    outb(0x40u, (u8)(div & 0xFFu));
    outb(0x40u, (u8)((div >> 8) & 0xFFu));
}

void x86_irq_init(void)
{
    u32 i = 0;

    while (i < 256u) {
        gate((int)i, irq0_stub);
        i++;
    }
    g_idtp.limit = (u16)(sizeof(g_idt) - 1u);
    g_idtp.base = (u64)&g_idt[0];
    __asm__ __volatile__("lidt %0" : : "m"(g_idtp));
    pic_remap();
    pit_init();
    __asm__ __volatile__("sti");
}

u32 port_in_isr(void)
{
    return g_in_isr;
}

void port_isr_enter(void)
{
    g_in_isr = g_in_isr + 1u;
}

void port_isr_exit(void)
{
    if (g_in_isr != 0u) {
        g_in_isr = g_in_isr - 1u;
    }
}

u64 port_cycles(void)
{
    u32 lo = 0;
    u32 hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

u32 port_atomic_cas(volatile u32 *p, u32 expect, u32 newv)
{
    if (p == 0) {
        return 0u;
    }
    return __sync_bool_compare_and_swap(p, expect, newv) ? 1u : 0u;
}

void os_test_set_isr(u32 v)
{
    g_in_isr = v;
}

void port_enter_task(void)
{
    g_in_task = 1u;
}

void port_leave_task(void)
{
    g_in_task = 0u;
}

int x86_irq_handler(void)
{
    os_sched_tick();
    outb(0x20u, 0x20u);
    if (g_in_task != 0u && g_cur_ctx != 0) {
        return 1;
    }
    return 0;
}
