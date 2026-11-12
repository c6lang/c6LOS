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

void port_init(void)
{
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
