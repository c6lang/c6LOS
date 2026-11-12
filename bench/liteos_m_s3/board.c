#include "los_task.h"
#include "los_config.h"

#define MMIO_CONSOLE 0x40000000u
#define MMIO_EXIT    0x40000004u

static void putc_(int c) { *(volatile unsigned int *)MMIO_CONSOLE = (unsigned int)(unsigned char)c; }
static void puts_(const char *s) { while (*s) { putc_(*s++); } }
static void puthex_(unsigned int v) { int i; const char *h="0123456789abcdef"; for(i=28;i>=0;i-=4) putc_(h[(v>>i)&15]); }

extern int bench_start(void);

static void *demo(UINT32 arg) {
    (void)arg;
    puts_("LiteOS-M S3 up\n");
    while (1) {
    }
    return 0;
}

int main(void) {
    UINT32 id = 0;
    TSK_INIT_PARAM_S p;
    UINT32 r;

    p.pfnTaskEntry = demo;
    p.uwArg = 0;
    p.usTaskPrio = 5;
    p.uwStackSize = 0x2000;
    p.stackAddr = 0;
    p.pcName = "demo";
    p.uwResved = 0;
    r = LOS_KernelInit();
    puts_("kernel init rc="); puthex_(r); putc_('\n');
    if (r != LOS_OK) {
        *(volatile unsigned int *)MMIO_EXIT = 2u;
        while (1) {}
    }
    (void)id;
    (void)p;
    r = (UINT32)bench_start();
    puts_("bench start rc="); puthex_(r); putc_('\n');
    if (r != 0u) {
        *(volatile unsigned int *)MMIO_EXIT = 3u;
        while (1) {}
    }
    LOS_Start();
    *(volatile unsigned int *)MMIO_EXIT = 4u;
    while (1) {}
}
