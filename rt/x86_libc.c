/* x86-64 裸机最小 mem*：c6 IR / kprintf 会引用。 */
#include "c6os/types.h"

void *memcpy(void *dst, const void *src, unsigned long n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    while (n-- > 0u) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d < s) {
        while (n-- > 0u) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n-- > 0u) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    u8 *d = (u8 *)dst;

    while (n-- > 0u) {
        *d++ = (u8)c;
    }
    return dst;
}
