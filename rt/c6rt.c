/* 目标侧最小运行库（freestanding）。host 用 libtcc1，这里只保留编译器
 * 生成代码真正需要的外部符号。编译时带 -fno-builtin，避免自我递归。
 *
 * 注意（待办）：IDF 闭源库是 windowed ABI，它 call8 调 memcpy 时返回必须用
 * retw；我们自己的 call0 代码必须用 ret。port/s3/abi_thunk.S 是"双 ABI"版本
 * 的思路，但实测接上后我们自己的调用路径会挂（见 docs/wifi-plan.md §21），
 * 暂时没启用，等 M1 继续往前时再解决。 */

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}
