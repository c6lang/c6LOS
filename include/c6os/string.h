#ifndef C6OS_STRING_H
#define C6OS_STRING_H

/* 编译器/工具链标准库的**声明层**（实现在 rt/c6rt.c 与 rt/x86_libc.c）。
 *
 * 规则见 docs/architecture.md §2.1：调用点一律用标准名字（memcpy/strlen/…），
 * 不要自己写循环，也不要起 c6_memcpy 这种别名。
 *
 *   - S3：工具链有 newlib（…/esp32s3/libc.a），但链接是 -nostdlib，所以用
 *         rt/c6rt.c 的 freestanding 替身 + Makefile 的 -D 重定向；
 *   - x86-64 裸机：clang 没有 libc，用 rt/x86_libc.c；
 *   - host/sim：宿主 libc 直接可用，这个头只是保持同一组原型。
 * 一律用 <stddef.h> 的 size_t，保证和实现里的签名不打架。 */
#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);

#endif
