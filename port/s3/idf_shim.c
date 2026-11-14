/* ESP-IDF / 闭源 WiFi·PHY 库的适配层（c6LOS 侧）。
 *
 * IDF 的 libphy.a / libpp.a / libnet80211.a 这些闭源库会调用一批"平台胶水"
 * 符号（原版由 IDF 的 C 代码或 FreeRTOS 提供）。这个文件提供最小可用实现，
 * 缺哪个补哪个 —— 链接器会把缺的符号直接报出来。
 *
 * 约定：函数原型必须和 IDF 侧完全一致（call0 ABI 下参数/返回值不匹配会很难查）。
 * IDF 里的签名见 esp_phy/src/phy_init.c、esp_coex/src/lib_printf.c 等。
 */
#include "c6os/types.h"

/* ---- 临界区：IDF 的 phy_enter_critical()/phy_exit_critical() -------------
 * 原版用 FreeRTOS 的 portENTER_CRITICAL 包住一个自旋锁，返回值恒为 0
 * （中断级别存在 TCB 里）。这里直接用关中断实现，语义等价且更省事。 */
u32 phy_enter_critical(void)
{
    u32 ps = 0;

    /* 内联汇编，避免跨 ABI 调用（本文件是按 windowed 编译给闭源库用的） */
    __asm__ __volatile__("rsil %0, 15" : "=r"(ps));
    return 0u;
}

void phy_exit_critical(u32 level)
{
    (void)level;
    __asm__ __volatile__("movi a2, 0\n\twsr a2, ps\n\trsync" : : : "a2");
}

/* ---- 调试输出：PHY/WiFi 库的 printf 全走这里 ---------------------------
 * 默认静默（真开起来会让射频打印淹没串口）。排查时把 IDF_SHIM_PRINT 打 1。 */
#ifndef IDF_SHIM_PRINT
#define IDF_SHIM_PRINT 0
#endif

int phy_printf(const char *fmt, ...)
{
#if IDF_SHIM_PRINT
    (void)fmt;
#endif
    return 0;
}

int coexist_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

void coex_pti_print(void)
{
}

/* ---- sprintf：闭源库里 debug 分支会用 --------------------------------
 * 我们还没有完整 libc，这里实现常用的那几种格式（%s %c %d %u %x %p %% 与
 * 数字宽度），足够 PHY 的调试路径用。 */
static char *shim_num(char *p, u32 v, u32 base, int upper, int width, int zero)
{
    char tmp[12] = {0};
    int n = 0;
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0u) { tmp[n] = '0'; n++; }
    while (v > 0u) { tmp[n] = digits[v % base]; n++; v = v / base; }
    while (n < width) { tmp[n] = zero ? '0' : ' '; n++; }
    while (n > 0) { n--; p[i] = tmp[n]; i++; }
    return p + i;
}

int sprintf(char *dst, const char *fmt, ...)
{
    __builtin_va_list ap;
    char *p = dst;

    __builtin_va_start(ap, fmt);
    while (*fmt != '\0') {
        if (*fmt != '%') { *p = *fmt; p++; fmt++; continue; }
        fmt++;
        {
            int zero = 0;
            int width = 0;

            if (*fmt == '0') { zero = 1; fmt++; }
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (int)(*fmt - '0'); fmt++; }
            if (*fmt == 'l') { fmt++; }
            if (*fmt == '\0') { break; }

            if (*fmt == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                if (s == 0) { s = "(null)"; }
                while (*s != '\0') { *p = *s; p++; s++; }
            } else if (*fmt == 'c') {
                *p = (char)__builtin_va_arg(ap, int);
                p++;
            } else if (*fmt == 'd' || *fmt == 'i') {
                int v = __builtin_va_arg(ap, int);
                if (v < 0) { *p = '-'; p++; v = -v; }
                p = shim_num(p, (u32)v, 10u, 0, width, zero);
            } else if (*fmt == 'u') {
                p = shim_num(p, __builtin_va_arg(ap, u32), 10u, 0, width, zero);
            } else if (*fmt == 'x') {
                p = shim_num(p, __builtin_va_arg(ap, u32), 16u, 0, width, zero);
            } else if (*fmt == 'X') {
                p = shim_num(p, __builtin_va_arg(ap, u32), 16u, 1, width, zero);
            } else if (*fmt == 'p') {
                u32 v = __builtin_va_arg(ap, u32);
                *p = '0'; p++; *p = 'x'; p++;
                p = shim_num(p, v, 16u, 0, 8, 1);
            } else {
                *p = '%'; p++; *p = *fmt; p++;
            }
        }
        fmt++;
    }
    *p = '\0';
    __builtin_va_end(ap);
    return (int)(p - dst);
}
