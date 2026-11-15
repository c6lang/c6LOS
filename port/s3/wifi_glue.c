/* wifi_glue.c —— WiFi 闭源库的 OS 适配层（第一步：先能链接 + 能返回）。
 *
 * 现状：所有 119 个成员先指向同一个"返回 0"的桩，目的是让 esp_wifi_init()
 * 能链接通过；之后用 USB-JTAG（tools/dbg.sh）在 esp_wifi_init 里断点/单步，
 * 按崩溃顺序逐个换成真实现。优先级：
 *   内存 -> 队列 -> 信号量/互斥 -> 定时器 -> 线程 -> 中断 -> 日志/事件
 */
#include "c6os/types.h"
#include "c6os/wifi_osi.h"

wifi_osi_funcs_t g_wifi_osi_funcs;
void *g_osi_funcs_p = (void *)&g_wifi_osi_funcs;

/* 通用的空实现：不碰参数、返回 0 */
static u32 glue_ret0(void)
{
    return 0u;
}

void s3_wifi_osi_init(void)
{
    void **tab = (void **)&g_wifi_osi_funcs;
    u32 n = (u32)(sizeof(wifi_osi_funcs_t) / sizeof(void *));
    u32 i = 0;

    while (i < n) {
        tab[i] = (void *)glue_ret0;
        i = i + 1u;
    }
}

/* 给 .c6 侧拿表地址用（c6 里不能直接取全局地址） */
void *s3_wifi_osi_funcs_ptr(void)
{
    return (void *)&g_wifi_osi_funcs;
}

/* ---- 链接期还需要的一些小符号（先给最小实现，跑起来再按崩溃顺序替换）---- */
void esp_log_level_set(const char *tag, int level)
{
    (void)tag;
    (void)level;
}

/* 注意：g_espnow_user_oui / g_mt 由 libespnow.a(manatick.o) 提供，
 * 这里不能再定义，否则 "multiple definition"。 */

int hexstr2bin(const char *hex, u8 *buf, u32 len)
{
    u32 i = 0;
    u32 n = 0;

    (void)len;
    while (hex[0] != '\0' && hex[1] != '\0' && i < len) {
        u32 v = 0u;
        int k = 0;
        while (k < 2) {
            char c = hex[k];
            u32 d;
            if (c >= '0' && c <= '9') { d = (u32)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { d = (u32)(c - 'a') + 10u; }
            else if (c >= 'A' && c <= 'F') { d = (u32)(c - 'A') + 10u; }
            else { return -1; }
            v = (v << 4) | d;
            k++;
        }
        buf[i] = (u8)v;
        i++;
        n = n + 2u;
        hex = hex + 2;
    }
    return (int)i;
}

int esp_supplicant_init(void) { return 0; }
int esp_supplicant_deinit(void) { return 0; }

/* 我们 -nostdlib，newlib 的这几个函数自己给最小实现 */
void *malloc(u32 n);
void free(void *p) { (void)p; }
int puts(const char *s) { (void)s; return 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++) != '\0') { } return r; }
u32 strlen(const char *s) { u32 n = 0; while (s[n] != '\0') { n++; } return n; }
int strncmp(const char *a, const char *b, u32 n) { u32 i = 0; while (i < n) { if (a[i] != b[i]) { return (int)(u8)a[i] - (int)(u8)b[i]; } if (a[i] == '\0') { return 0; } i++; } return 0; }
char *strncpy(char *d, const char *s, u32 n) { u32 i = 0; while (i < n && s[i] != '\0') { d[i] = s[i]; i++; } while (i < n) { d[i] = '\0'; i++; } return d; }
u32 strnlen(const char *s, u32 n) { u32 i = 0; while (i < n && s[i] != '\0') { i++; } return i; }
int vsnprintf(char *buf, u32 n, const char *fmt, void *ap);
int vsnprintf(char *buf, u32 n, const char *fmt, void *ap) { (void)ap; if (n > 0u) { buf[0] = '\0'; } (void)fmt; return 0; }

/* esp_event（先给最小实现；WiFi 事件回调以后要接真的） */
u32 esp_event_handler_register(u32 a, int b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; return 0u; }
u32 esp_event_handler_unregister(u32 a, int b, void *c) { (void)a; (void)b; (void)c; return 0u; }
u32 esp_event_post(u32 a, int b, const void *c, u32 d, u32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0u; }

int snprintf(char *buf, u32 n, const char *fmt, ...)
{
    (void)fmt;
    if (n > 0u) { buf[0] = '\0'; }
    return 0;
}

/* 不链接 WiFi 闭源库的构建里给 esp_wifi_init_internal 一个弱兜底，
 * 这样普通 make s3 也能链接通过；链接了 WiFi 库时真实符号会覆盖它。 */
__attribute__((weak)) int esp_wifi_init_internal(const void *cfg)
{
    (void)cfg;
    return -1;
}
