#include "los_task.h"
#include "los_config.h"

#define S3_USB_JTAG_BASE 0x60038000u
#define S3_USB_EP1      (*(volatile unsigned int *)(S3_USB_JTAG_BASE + 0x00u))
#define S3_USB_EP1_CONF (*(volatile unsigned int *)(S3_USB_JTAG_BASE + 0x04u))
#define S3_USB_CONF0    (*(volatile unsigned int *)(S3_USB_JTAG_BASE + 0x18u))
#define S3_SYSTEM_PERIP_CLK_EN1 (*(volatile unsigned int *)0x600C001Cu)
#define S3_SYSTEM_PERIP_RST_EN1 (*(volatile unsigned int *)0x600C0024u)
#define S3_RTC_USB_CONF (*(volatile unsigned int *)0x60008120u)
#define MMIO_EXIT 0x40000004u

void board_putc(int c) {
    unsigned int guard = 0;
    while ((S3_USB_EP1_CONF & 1u) != 0u) {
        guard++;
        if (guard > 2000000u) { return; }
    }
    guard = 0;
    while ((S3_USB_EP1_CONF & (1u << 1)) == 0u) {
        guard++;
        if (guard > 2000000u) { return; }
    }
    S3_USB_EP1 = (unsigned int)(c & 0xFF);
    S3_USB_EP1_CONF = 1u;
}

void board_puts(const char *s) { while (*s) { board_putc(*s++); } }
void board_puthex(unsigned int v) {
    int i; const char *h = "0123456789abcdef";
    for (i = 28; i >= 0; i -= 4) { board_putc(h[(v >> i) & 15]); }
}

static void wdt_disable(void) {
    *(volatile unsigned int *)0x600C0018u |= (1u << 13);
    *(volatile unsigned int *)(0x6001F000u + 0x64u) = 0x50D83AA1u;
    *(volatile unsigned int *)(0x6001F000u + 0x48u) = 0u;
    *(volatile unsigned int *)(0x6001F000u + 0x4Cu) = 0u;
    *(volatile unsigned int *)(0x6001F000u + 0x64u) = 0u;
}

static void console_init(void) {
    volatile unsigned int i;
    S3_RTC_USB_CONF = (S3_RTC_USB_CONF & ~(1u << 19)) | (1u << 20);
    S3_SYSTEM_PERIP_CLK_EN1 |= (1u << 10);
    S3_SYSTEM_PERIP_RST_EN1 &= ~(1u << 10);
    S3_USB_CONF0 &= ~(1u << 16);
    S3_USB_CONF0 |= (1u << 14);
    *(volatile unsigned int *)(0x60038010u) = 0x4u;
    S3_USB_EP1_CONF = 1u;
    for (i = 0; i < 2000000u; i++) { __asm__ __volatile__("" ::: "memory"); }
    for (i = 0; i < 1000000u; i++) { __asm__ __volatile__("" ::: "memory"); }
}

extern int bench_start(void);

int main(void) {
    UINT32 r;
    *(volatile unsigned int *)0x3FCF0000 = 0x5555;
    wdt_disable();
    *(volatile unsigned int *)0x3FCF0000 = 0x5556;
    console_init();
    *(volatile unsigned int *)0x3FCF0000 = 0x6666;
    board_puts("LiteOS-M S3 boot\n");
    r = LOS_KernelInit();
    board_puts("kernel init rc="); board_puthex(r); board_putc('\n');
    if (r != LOS_OK) {
        *(volatile unsigned int *)MMIO_EXIT = 2u;
        while (1) {}
    }
    r = (UINT32)bench_start();
    board_puts("bench start rc="); board_puthex(r); board_putc('\n');
    if (r != 0u) {
        *(volatile unsigned int *)MMIO_EXIT = 3u;
        while (1) {}
    }
    LOS_Start();
    *(volatile unsigned int *)MMIO_EXIT = 4u;
    while (1) {}
}
