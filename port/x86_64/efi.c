/* x86-64 参考目标：UEFI 应用（无 OS，qemu + OVMF 可跑）。 */
#include "c6os/types.h"
#include "c6os/port.h"

typedef unsigned short char16;

typedef struct {
    void *reset;
    void *output_string;
} SimpleTextOut;

typedef struct {
    char pad0[64];
    SimpleTextOut *con_out;
} SystemTable;

void os_main(void);

static SystemTable *g_st;

void __attribute__((sysv_abi)) port_init(void)
{
}

void __attribute__((sysv_abi)) port_putc(int ch)
{
    char16 buf[2];

    if (g_st == 0 || g_st->con_out == 0) {
        return;
    }
    buf[0] = (char16)(ch & 0xff);
    buf[1] = 0;
    {
        void (*out)(SimpleTextOut *, char16 *) = (void (*)(SimpleTextOut *, char16 *))g_st->con_out->output_string;
        out(g_st->con_out, buf);
    }
}

void __attribute__((sysv_abi)) port_puts(const char *s)
{
    while (*s != '\0') {
        port_putc((int)*s);
        s++;
    }
}

void __attribute__((sysv_abi)) port_idle(void)
{
    __asm__ __volatile__("hlt");
}

int efi_main(void *image, SystemTable *st)
{
    (void)image;
    g_st = st;
    port_init();
    port_puts("c6os x86_64 efi boot\n");
    os_main();
    while (1) {
        port_idle();
    }
    return 0;
}
