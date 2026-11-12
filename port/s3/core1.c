/* ESP32-S3 第二核最小启动：core1 在 IRAM 里跑一个计数循环。 */
#include "c6os/types.h"

#define SYS_CORE1_CTRL (*(volatile u32 *)0x600C0014u)
#define SYS_CORE1_RUNSTALL 0x1u
#define SYS_CORE1_CLKGATE  0x2u
#define SYS_CORE1_RESET    0x4u

static u8 g_core1_stack[4096] __attribute__((aligned(16)));
static volatile u32 g_core1_count;

static void core1_entry(void)
{
    extern void _vecbase;
    __asm__ __volatile__("mov a1, %0" : : "r"(&g_core1_stack[4096]));
    __asm__ __volatile__("wsr %0, vecbase; isync" : : "r"(&_vecbase));
    while (1) {
        g_core1_count++;
    }
}

int s3_core1_start(void)
{
    u32 v = SYS_CORE1_CTRL;

    if (g_core1_count != 0u) {
        return 0;
    }
    v = (v & ~0xFFFF0000u) | (((u32)core1_entry & 0xFFFFu) << 16);
    v |= SYS_CORE1_CLKGATE;
    v |= SYS_CORE1_RESET;
    SYS_CORE1_CTRL = v;
    __asm__ __volatile__("memw");
    v &= ~SYS_CORE1_RESET;
    SYS_CORE1_CTRL = v;
    __asm__ __volatile__("memw");
    v &= ~SYS_CORE1_RUNSTALL;
    SYS_CORE1_CTRL = v;
    __asm__ __volatile__("memw");
    return 0;
}

u32 s3_core1_count(void)
{
    return g_core1_count;
}

void s3_core1_stack_reset(void)
{
    g_core1_count = 0;
}
