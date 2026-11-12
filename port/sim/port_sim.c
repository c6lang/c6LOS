/* host/sim 参考端口：只验证内核逻辑与控制台，不模拟抢占。 */

#include <stdio.h>
#include "c6os/types.h"
#include "c6os/port.h"

void port_init(void)
{
}

void port_putc(int ch)
{
    putchar(ch);
    fflush(stdout);
}

void port_puts(const char *s)
{
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
