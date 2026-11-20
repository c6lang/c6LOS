/* host/sim 参考端口：只验证内核逻辑与控制台，不模拟抢占。
 * 另含一个可选的输出捕获缓冲，供 host 测试做断言。 */

#include <stdio.h>
#include "c6os/types.h"
#include "c6os/port.h"

static char cap_buf[512];
static u32 cap_len;
static int cap_on;

void port_sim_capture_begin(void)
{
    cap_len = 0;
    cap_buf[0] = '\0';
    cap_on = 1;
}

const char *port_sim_capture_end(void)
{
    cap_on = 0;
    return cap_buf;
}

static void cap_put(int ch)
{
    if (cap_on && (cap_len + 1u < (u32)sizeof(cap_buf))) {
        cap_buf[cap_len++] = (char)ch;
        cap_buf[cap_len] = '\0';
    }
}

u32 port_core_id(void)
{
    return 0u;
}

void port_init(void)
{
}

void port_reboot(void)
{
}

static u32 g_irq_nest;

u32 port_irq_save(void)
{
    u32 old = g_irq_nest;

    g_irq_nest = g_irq_nest + 1u;
    return old;
}

void port_irq_restore(u32 state)
{
    g_irq_nest = state;
}

int port_getc(void)
{
    return -1;
}

void port_putc(int ch)
{
    cap_put(ch);
    putchar(ch);
    fflush(stdout);
}

void port_puts(const char *s)
{
    const char *p = s;

    while (*p != '\0') {
        cap_put((int)*p);
        p++;
    }
    fputs(s, stdout);
    fflush(stdout);
}

void port_delay_ms(u32 ms)
{
    (void)ms;
}

void port_idle(void)
{
}

u32 port_virtual_tick(void)
{
    /* host 参考机：无硬件 tick，由调度器在空闲时推进虚拟时钟。 */
    return 1u;
}

static u32 g_test_isr;

u32 port_in_isr(void)
{
    return g_test_isr;
}

void port_isr_enter(void)
{
    g_test_isr = g_test_isr + 1u;
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
    if (*p == expect) {
        *p = newv;
        return 1u;
    }
    return 0u;
}

void port_isr_exit(void)
{
    if (g_test_isr != 0u) {
        g_test_isr = g_test_isr - 1u;
    }
}

void os_test_set_isr(u32 v)
{
    g_test_isr = v;
}

void port_mark(u32 v)
{
    (void)v;
}

void port_panic_halt(void)
{
    /* host 测试：直接返回，让断言继续执行；目标机由 port 层死循环。 */
}

void port_qemu_exit(void)
{
    /* host 测试没有 QEMU，行为同 panic_halt（返回）。 */
}

u64 port_wallclock_sec(void)
{
    return 0ull;                       /* host/sim 没有 RTC：回退 TSC 假时钟 */
}
