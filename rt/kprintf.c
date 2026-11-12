#include <stdarg.h>
#include "c6os/types.h"
#include "c6os/log.h"

/* 目标侧最小格式化实现（C，va_list）。不分配、不依赖 libc。
 * c6 侧通过 include/c6os/log.h 的 `void kprintf(const char *, ...)` 调用。 */

static void kputsn(const char *s, u32 max)
{
    while (*s != '\0' && max != 0u) {
        kputc((int)*s);
        s++;
        max--;
    }
}

static void kutoi(u64 v, u32 base, int upper, u32 width, int zero)
{
    char buf[32];
    u32 i = 0;
    u32 n = 0;
    u64 x = v;

    if (x == 0u) {
        buf[i++] = '0';
    }
    while (x != 0u) {
        u32 d = (u32)(x % (u64)base);
        if (d < 10u) {
            buf[i++] = (char)('0' + d);
        } else if (upper) {
            buf[i++] = (char)('A' + (d - 10u));
        } else {
            buf[i++] = (char)('a' + (d - 10u));
        }
        x /= (u64)base;
    }
    n = i;
    while (width > n) {
        kputc(zero ? '0' : ' ');
        width--;
    }
    while (i != 0u) {
        i--;
        kputc((int)buf[i]);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    const char *p = fmt;

    if (fmt == 0) {
        return;
    }
    va_start(ap, fmt);
    while (*p != '\0') {
        if (*p != '%') {
            kputc((int)*p);
            p++;
            continue;
        }
        p++;
        if (*p == '%') {
            kputc('%');
            p++;
            continue;
        }

        {
            int zero = 0;
            int is_long = 0;
            u32 width = 0;

            while (*p == '0') {
                zero = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                if (width < 256u) {
                    width = width * 10u + (u32)(*p - '0');
                    if (width > 256u) {
                        width = 256u;
                    }
                }
                p++;
            }
            while (*p == 'l') {
                is_long++;
                p++;
            }

            if (*p == 'd' || *p == 'i') {
                i64 v;
                if (is_long >= 2) {
                    v = va_arg(ap, i64);
                } else {
                    v = (i64)va_arg(ap, int);
                }
                if (v < 0) {
                    u64 uv = (u64)(-(v + 1));
                    kputc('-');
                    kutoi(uv + 1u, 10u, 0, width, zero);
                } else {
                    kutoi((u64)v, 10u, 0, width, zero);
                }
            } else if (*p == 'u') {
                u64 v = (is_long >= 2) ? va_arg(ap, u64)
                                       : (u64)(u32)va_arg(ap, unsigned int);
                kutoi(v, 10u, 0, width, zero);
            } else if (*p == 'x' || *p == 'X') {
                u64 v = (is_long >= 2) ? va_arg(ap, u64)
                                       : (u64)(u32)va_arg(ap, unsigned int);
                kutoi(v, 16u, *p == 'X', width, zero);
            } else if (*p == 'c') {
                kputc(va_arg(ap, int));
            } else if (*p == 's') {
                const char *s = va_arg(ap, const char *);
                if (s == 0) {
                    kputs("(null)");
                } else {
                    kputsn(s, 512u);
                }
            } else if (*p == 'p') {
                u64 v = (u64)(unsigned long)va_arg(ap, void *);
                kputs("0x");
                kutoi(v, 16u, 0, 0, 0);
            } else {
                kputc('%');
                if (*p != '\0') {
                    kputc((int)*p);
                }
            }
        }
        if (*p != '\0') {
            p++;
        }
    }
    va_end(ap);
}
