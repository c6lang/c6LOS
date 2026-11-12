/* ESP32-S3 第二核最小启动：core1 在 IRAM 里跑一个计数循环。 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/task.h"

#define SYS_CORE1_CTRL0 (*(volatile u32 *)0x600C0000u)
#define SYS_CORE1_CTRL1 (*(volatile u32 *)0x600C0004u)
#define SYS_CORE1_RUNSTALL 0x1u
#define SYS_CORE1_CLKGATE  0x2u
#define SYS_CORE1_RESET    0x4u

static u8 g_core1_stack[4096] __attribute__((aligned(16)));
#define CORE1_STACK_MAGIC 0xC6C0DEADu
static volatile u32 g_core1_count;
static volatile u32 g_core1_cmd;
static volatile u32 g_core1_ack;
static volatile u32 g_core1_prid = 0xFFFFFFFFu;
static volatile u32 g_core1_mode;

static void core1_entry(void)
{
    extern void _vecbase;
    *(volatile u32 *)&g_core1_stack[0] = CORE1_STACK_MAGIC;
    *(volatile u32 *)&g_core1_stack[4] = CORE1_STACK_MAGIC;
    __asm__ __volatile__("mov a1, %0" : : "r"(&g_core1_stack[4096]));
    __asm__ __volatile__("wsr %0, vecbase; isync" : : "r"(&_vecbase));
    g_core1_prid = port_core_id();
    if (g_core1_mode != 0u) {
        os_task_start_core(1u);
    }
    while (1) {
        g_core1_count++;
        if (g_core1_cmd != g_core1_ack) {
            g_core1_ack = g_core1_cmd;
        }
    }
}

int s3_core1_start(void);

int s3_core1_start_mode(u32 mode)
{
    g_core1_mode = mode;
    return s3_core1_start();
}

int s3_core1_start(void)
{
    u32 v = SYS_CORE1_CTRL0;

    if (g_core1_count != 0u) {
        return 0;
    }
    SYS_CORE1_CTRL1 = (u32)core1_entry;
    __asm__ __volatile__("memw");
    v |= SYS_CORE1_CLKGATE;
    v |= SYS_CORE1_RESET;
    SYS_CORE1_CTRL0 = v;
    __asm__ __volatile__("memw");
    v &= ~SYS_CORE1_RESET;
    SYS_CORE1_CTRL0 = v;
    __asm__ __volatile__("memw");
    v &= ~SYS_CORE1_RUNSTALL;
    SYS_CORE1_CTRL0 = v;
    __asm__ __volatile__("memw");
    return 0;
}

int s3_core1_send(u32 cmd)
{
    u32 guard = 0;

    g_core1_cmd = cmd;
    while (g_core1_ack != cmd) {
        guard++;
        if (guard > 2000000u) {
            return -1;
        }
    }
    return 0;
}

u32 s3_core1_ack(void)
{
    return g_core1_ack;
}

u32 s3_core1_count(void)
{
    if (*(volatile u32 *)&g_core1_stack[0] != CORE1_STACK_MAGIC ||
        *(volatile u32 *)&g_core1_stack[4] != CORE1_STACK_MAGIC) {
        return 0u;
    }
    return g_core1_count;
}

u32 s3_core1_prid(void)
{
    return g_core1_prid;
}

void s3_core1_stack_reset(void)
{
    g_core1_count = 0;
}
