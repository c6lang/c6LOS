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

/* 32 位目标没有 __udivdi3/__umoddi3，内核 tick 换算会引用。 */
unsigned long long __udivdi3(unsigned long long a, unsigned long long b)
{
    unsigned long long q = 0;
    unsigned long long r = 0;
    int i;

    if (b == 0ull) {
        return 0ull;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1ull);
        if (r >= b) {
            r -= b;
            q |= (1ull << i);
        }
    }
    return q;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b)
{
    unsigned long long r = 0;
    int i;

    if (b == 0ull) {
        return 0ull;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1ull);
        if (r >= b) {
            r -= b;
        }
    }
    return r;
}
