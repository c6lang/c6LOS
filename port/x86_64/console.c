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

/* ---- PC/MC146818 CMOS RTC ------------------------------------------------
 * QEMU 默认把宿主当前时间写进 CMOS（REG_B=0x02：BCD + 24 小时制）。墙钟不能
 * 再用固定起点 1700000000：Alpine 的 ssl_client 会校验服务器证书，2023 年的
 * 假时间会让新证书一律 "not yet valid"，wget HTTPS 必败。
 * 寄存器见 QEMU 的 include/hw/rtc/mc146818rtc_regs.h。 */
#define RTC_IO_INDEX   0x70u
#define RTC_IO_DATA    0x71u
#define RTC_SEC        0x00u
#define RTC_MIN        0x02u
#define RTC_HOUR       0x04u
#define RTC_DAY        0x07u
#define RTC_MON        0x08u
#define RTC_YEAR       0x09u
#define RTC_CENTURY    0x32u

static u8 cmos_read_(u8 reg)
{
    outb(RTC_IO_INDEX, reg);
    return inb(RTC_IO_DATA);
}

static u32 cmos_bcd_(u8 v)
{
    return ((u32)(v >> 4) * 10u) + (u32)(v & 0x0fu);
}

/* 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil）。 */
static u64 days_from_civil_(u32 y, u32 m, u32 d)
{
    u32 era;
    u32 yoe;
    u32 mp;
    u32 doe;

    y -= (m <= 2u) ? 1u : 0u;
    era = y / 400u;
    yoe = y - era * 400u;
    mp = (m > 2u) ? (m - 3u) : (m + 9u);
    doe = yoe * 365u + yoe / 4u - yoe / 100u +
          (153u * mp + 2u) / 5u + d - 1u;
    return (u64)era * 146097ull + (u64)doe - 719468ull;
}

u64 port_wallclock_sec(void)
{
    u32 sec = cmos_bcd_(cmos_read_(RTC_SEC));
    u32 min = cmos_bcd_(cmos_read_(RTC_MIN));
    u32 hour = cmos_bcd_(cmos_read_(RTC_HOUR));
    u32 day = cmos_bcd_(cmos_read_(RTC_DAY));
    u32 mon = cmos_bcd_(cmos_read_(RTC_MON));
    u32 year = cmos_bcd_(cmos_read_(RTC_YEAR)) +
               cmos_bcd_(cmos_read_(RTC_CENTURY)) * 100u;

    /* CMOS 全 0xFF/0（没有 RTC）时年份会算出 16665，回退 TSC 假时钟。 */
    if (year < 1970u || year > 2100u) { return 0ull; }
    return days_from_civil_(year, mon, day) * 86400ull +
           (u64)hour * 3600ull + (u64)min * 60ull + (u64)sec;
}

extern void x86_irq_init(void);

u32 port_core_id(void)
{
    return 0u;
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
    x86_irq_init();
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

int port_getc(void)
{
    if ((inb(COM1 + 5u) & 0x01u) == 0u) {
        return -1;
    }
    return (int)inb(COM1);
}

void port_puts(const char *s)
{
    while (*s != '\0') {
        port_putc((int)*s);
        s++;
    }
}

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
    /* PIT 100Hz 已接管时基，睡眠等中断即可。 */
    __asm__ __volatile__("sti");
    __asm__ __volatile__("hlt");
}

void port_reboot(void)
{
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void port_panic_halt(void)
{
    __asm__ __volatile__("cli");
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

/* shell 里 exit / 所有进程退出：通知 QEMU 关机（isa-debug-exit，iobase 0x501）。
 * 批处理 QEMU 没挂这个设备，写端口是无害 no-op，随后照旧 hlt 等 timeout。 */
void port_qemu_exit(void)
{
    /* isa-debug-exit 默认 iobase=0x501 / iosize=2，必须写 16 位宽度，
     * QEMU 的 write handler 会在 width != iosize 时忽略（写 4 字节不退出）。 */
    __asm__ __volatile__("outw %0, %w1" : : "a"((u16)0u), "Nd"((u16)0x501u));
    port_panic_halt();
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
