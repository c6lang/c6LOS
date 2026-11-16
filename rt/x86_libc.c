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

/* ---- 字符串（标准库，全内核共用一份实现）--------------------------------
 * 之前只补了 memcpy/memmove/memset，VFS/协议栈这些"按名字找东西"的模块一
 * 上来就要 strlen/strcmp/strncpy。与其每个模块手搓，不如在这里补齐——
 * 和 rt/c6rt.c（freestanding 目标）保持同一组符号。 */

unsigned long strlen(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

unsigned long strnlen(const char *s, unsigned long max)
{
    unsigned long n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    unsigned long i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
        i++;
    }
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

int strncmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i = 0;

    while (i < n && a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
        i++;
    }
    if (i == n) {
        return 0;
    }
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

char *strcpy(char *dst, const char *src)
{
    unsigned long i = 0;

    while (src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

char *strncpy(char *dst, const char *src, unsigned long n)
{
    unsigned long i = 0;

    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i] = '\0';
        i++;
    }
    return dst;
}
