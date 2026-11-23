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
/* 临界区：把进入前的 PS 存起来，退出时**原样恢复**。
 * 千万不要把 PS 直接写成 0 —— 那会把 PS.WOE（窗口溢出使能）也清掉，
 * 之后闭源库里的 windowed 调用全部走窗口溢出异常，深调用里容易卡死
 * （实测：PHY 初始化会挂住，被看门狗反复复位）。 */
static u32 g_phy_ps;

/* 调试：闭源库回调我们的胶水函数时打一个字符（限 40 个），用来定位它卡在哪 */
int  s3_ets_printf(const char *fmt, ...);
void s3_win_puts(const char *s);
static u32 g_trace_n;

void s3_beacon(u32 slot, u32 val);

void s3_win_trace(const char *s)
{
    s3_beacon(3u, 0xC0000000u | (g_trace_n & 0xFFFFu));
    if (g_trace_n < 40u) {
        g_trace_n = g_trace_n + 1u;
        s3_ets_printf("%s", s);
    }
}

u32 phy_enter_critical(void)
{
    u32 ps = 0;

    s3_win_trace("e");

    /* 内联汇编，避免跨 ABI 调用（本文件是按 windowed 编译给闭源库用的） */
    __asm__ __volatile__("rsil %0, 15" : "=r"(ps));
    g_phy_ps = ps;
    return 0u;                 /* IDF 也是恒返回 0（中断级别存在别处） */
}

void phy_exit_critical(u32 level)
{
    (void)level;
    s3_win_trace("x");
    __asm__ __volatile__("wsr %0, ps\n\trsync" : : "a"(g_phy_ps));
}

/* ---- 调试输出：PHY/WiFi 库的 printf 全走这里 ---------------------------
 * 默认静默（真开起来会让射频打印淹没串口）。排查时把 IDF_SHIM_PRINT 打 1。 */
#ifndef IDF_SHIM_PRINT
#define IDF_SHIM_PRINT 0
#endif

/* ---- 闭源库的调试输出飞行记录仪 ----------------------------------------
 * 不能直接往串口打（本文件是 windowed 编译，port_putc 是 call0 的，跨 ABI 会炸），
 * 所以把格式化结果写进 DRAM 里的环形缓冲；崩溃后用
 *   esptool read-mem <g_idf_log 地址>
 * 读出来，就能看到库死在哪一步（复位后 DRAM 内容还在）。
 */
static int vsprintf_internal(char *dst, int cap, const char *fmt, __builtin_va_list ap);

/* 日志写进 **RTC 慢速内存**（0x50000400 起），因为：
 *  - 复位后不丢（读它也能照常进行）；
 *  - DRAM 里的东西会被 ROM/bootloader 冲掉（实测读回来是 0）。
 * 每个字符存一个 u32（RTC 只支持按 4 字节访问），另有长度存在 0x500003FC。 */
#define RTC_LOG_LEN (*(volatile u32 *)0x500003FCu)
#define RTC_LOG_W(i) (*(volatile u32 *)(0x50000400u + 4u * (i)))

static void idf_log_append(const char *s, int n)
{
    u32 len = RTC_LOG_LEN;
    int i = 0;

    if (len > 200u) { len = 0u; }              /* 简单环形：满了从头再来 */
    while (i < n && len < 200u) {
        u32 c = (u32)(unsigned char)s[i];

        if (c == 10u || c == 13u) { c = (u32)' '; }
        RTC_LOG_W(len) = c;
        len++;
        i++;
    }
    RTC_LOG_LEN = len;
}

int phy_printf(const char *fmt, ...)
{
    char tmp[160];
    __builtin_va_list ap;
    int n = 0;

    __builtin_va_start(ap, fmt);
    n = vsprintf_internal(tmp, (int)sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) { idf_log_append(tmp, n); }
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

static int vsprintf_internal(char *dst, int cap, const char *fmt, __builtin_va_list ap)
{
    char *p = dst;
    char *end = dst + cap - 1;

    (void)end;

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
    return (int)(p - dst);
}

int sprintf(char *dst, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n = 0;

    __builtin_va_start(ap, fmt);
    n = vsprintf_internal(dst, 1024, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

/* §479 ★★★ 我们自己的 ets_printf 实现，**完全不碰 ROM**。
 *
 * 之前一直用 ROM 的 `ets_printf`（0x4004423c，windowed、`entry a1,80`），
 * 但它和我们这套混合 ABI 的调用链反复打架：只要格式串稍长或参数稍多，
 * 就会在它自己的输出循环里跳飞（实测 cause=0x1c 取指错、excvaddr=0x17）。
 * 本文件已经有 `vsprintf_internal`，直接格式化到局部 buffer 再逐字输出即可，
 * 既没有 ABI 边界、也没有 ROM 的行为不确定性。
 *
 * 注意本文件按 `-mabi=windowed` 编译（给闭源库用），所以这里就是 windowed
 * 函数，正好和 `call8 s3_ets_printf` 的调用点匹配。call0 单元走
 * `s3_ets_printf_call0`（`port/s3/w2c.S` 的桥）。 */
int s3_ets_printf(const char *fmt, ...)
{
    char tmp[200];
    __builtin_va_list ap;
    int n = 0;

    if (fmt == 0) { return 0; }
    __builtin_va_start(ap, fmt);
    n = vsprintf_internal(tmp, (int)sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) { s3_win_puts(tmp); }
    return n;
}
