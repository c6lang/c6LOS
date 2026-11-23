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

/* 给 personality 用：安装指定 DPL 的门（int 0x80 需要 DPL3）。
 * type 字节的 bit6..5 是 DPL；0x8E 是"64 位中断门、P=1、DPL0"。 */
void x86_idt_set(int n, void (*fn)(void), u8 dpl)
{
    gate(n, fn);
    g_idt[n].type = (u8)(0x8Eu | (u8)((dpl & 3u) << 5));
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

/* ---- TSC 频率标定：让 port_cycles() 的返回值真的是纳秒 ----------------
 *
 * 上层一律按"1 个 port_cycles 计数 = 1ns"在算（linux/proc.c 的 alarm/
 * itimer/timerfd、clock_gettime、/proc/uptime，linux/socket.c 的自旋预算，
 * linux/fs.c 的 mtime）。可 rdtsc 数的是 TSC，频率等于 CPU 基频 —— 这台
 * Xeon 是 2.2GHz，不是 1GHz。直接交出去，所有时间都跑快 2.2 倍：
 * ping -i 1 变成 0.45s 一发、sleep 1 只睡 0.45s、date 也是 2.2 倍速。
 *
 * 启动时用 PIT 通道 2 标定（通道 0 正跑着调度 tick，不能动）：通道 2 在
 * gate 拉高后从 65535 递减，1193182/65535 ≈ 54.93ms 后 port 0x61 的 bit5
 * （timer 2 OUT）拉高。量 4 段共 ~220ms 换算成 TSC 频率。标定失败或结果
 * 离谱（有的虚拟机不实现通道 2）就退回 1GHz 的老口径，行为与以前一致。 */
static u64 g_tsc_hz = 1000000000ull;
static u64 g_ns_mul = 0ull;                /* ns = (rdtsc * g_ns_mul) >> 32 */

static u64 rdtsc_(void)
{
    u32 lo = 0u;
    u32 hi = 0u;

    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

/* 量一段 65535/1193182 秒的 TSC 数；返回 0 表示通道 2 没有反应。 */
static u64 pit2_window_(void)
{
    u64 t0;
    u64 t1;
    u8 v = (u8)(inb(0x61u) & 0xFCu);

    outb(0x61u, v);                        /* gate2=0、喇叭数据位清掉 */
    outb(0x43u, 0xB0u);                    /* 通道 2、模式 0（单次）、先低后高 */
    outb(0x42u, 0xFFu);
    outb(0x42u, 0xFFu);
    outb(0x61u, (u8)(v | 0x01u));          /* gate2=1：开始递减 */
    t0 = rdtsc_();
    for (;;) {
        t1 = rdtsc_();
        if ((inb(0x61u) & 0x20u) != 0u) {  /* OUT2 拉高 = 计到 0 */
            return t1 - t0;
        }
        if ((t1 - t0) > 4000000000ull) {   /* 等太久：认失败（别卡住启动） */
            return 0ull;
        }
    }
}

static void tsc_calibrate_(void)
{
    u64 sum = 0ull;
    u64 hz;
    u32 k = 0u;
    u8 save = inb(0x61u);

    while (k < 4u) {
        u64 d = pit2_window_();

        if (d == 0ull) {
            break;
        }
        sum = sum + d;
        k++;
    }
    outb(0x61u, save);                     /* 喇叭/门控恢复原状 */
    if (k < 2u) {                          /* 至少两段才敢算 */
        return;
    }
    hz = (sum * 1193182ull) / ((u64)k * 65535ull);
    if (hz < 500000000ull || hz > 20000000000ull) {
        return;
    }
    g_tsc_hz = hz;
    g_ns_mul = (1000000000ull << 32) / hz;
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
    tsc_calibrate_();      /* 标定 TSC：就在开中断之前，不受调度干扰 */
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

/* 返回"纳秒口径"的周期数：上层全部按 1 计数 = 1ns 用，所以这里把 rdtsc
 * 按标定出来的 TSC 频率缩放（见上面的 tsc_calibrate_）。没标定成功时
 * g_ns_mul 还是 0，就原样返回 raw TSC —— 和以前 1GHz 假设的行为一致。 */
u64 port_cycles(void)
{
    u64 t = rdtsc_();

    if (g_ns_mul == 0ull) {
        return t;
    }
    return (u64)(((unsigned __int128)t * (unsigned __int128)g_ns_mul) >> 32);
}

u64 port_tsc_hz(void)
{
    return g_tsc_hz;
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
