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


/* ---- 真正可用的 OSI 内存函数（其余仍为 glue_ret0 桩）----
 * 注意：port/s3/fast.c 里的系统堆只在非 LEAN 构建里链接，而 WiFi 场景必须
 * 用 S3_LEAN=1，所以这里**自建一个独立的堆**，放在空闲 DRAM 里。
 * 0x3FCB0000 ~ 0x3FCD9000 之间是可用区间（避开栈和坏区）。 */
#define S3_WIFI_HEAP_BASE 0x3FCB8000u
#define S3_WIFI_HEAP_SIZE 0x00010000u   /* 64KB，落在 app 的 DRAM 段之后的空闲区 */

/* 自包含的分配器：块头 16 字节 { magic, size, used, next }，首次适配 + 释放合并。
 * 不依赖 kernel/heap.c6（它在 S3_LEAN=1 下不参与链接）。 */
#define WH_MAGIC 0x57484646u
typedef struct WhBlk { u32 magic; u32 size; u32 used; u32 pad; } WhBlk;
static int g_wh_ready = 0;

static void wh_ensure(void)
{
    WhBlk *b;
    if (g_wh_ready != 0) { return; }
    b = (WhBlk *)S3_WIFI_HEAP_BASE;
    b->magic = WH_MAGIC;
    b->size  = S3_WIFI_HEAP_SIZE - sizeof(WhBlk);
    b->used  = 0u;
    b->pad   = 0u;
    g_wh_ready = 1;
}

static void *osi_malloc(u32 n)
{
    u32 want = (n + 15u) & ~15u;
    WhBlk *b;
    wh_ensure();
    b = (WhBlk *)S3_WIFI_HEAP_BASE;
    while (b->magic == WH_MAGIC) {
        if (b->used == 0u && b->size >= want) {
            /* 够大就切一块出来 */
            if (b->size >= want + sizeof(WhBlk) + 16u) {
                WhBlk *nb = (WhBlk *)((u8 *)b + sizeof(WhBlk) + want);
                nb->magic = WH_MAGIC;
                nb->size  = b->size - want - sizeof(WhBlk);
                nb->used  = 0u;
                nb->pad   = 0u;
                b->size   = want;
            }
            b->used = 1u;
            return (u8 *)b + sizeof(WhBlk);
        }
        b = (WhBlk *)((u8 *)b + sizeof(WhBlk) + b->size);
    }
    return 0;
}

static void osi_free(void *p)
{
    WhBlk *hdr;
    if (p == 0) { return; }
    hdr = (WhBlk *)((u8 *)p - sizeof(WhBlk));
    if (hdr->magic == WH_MAGIC) { hdr->used = 0u; }
}

static u32 osi_free_heap_size(void) { return S3_WIFI_HEAP_SIZE / 2u; }

static void *osi_realloc(void *p, u32 n)
{
    void *q;
    u32 old;
    u32 i = 0u;
    u8 *a; u8 *b;
    if (p == 0) { return osi_malloc(n); }
    q = osi_malloc(n);
    if (q == 0) { return 0; }
    old = ((WhBlk *)((u8 *)p - sizeof(WhBlk)))->size;
    if (old > n) { old = n; }
    a = (u8 *)p; b = (u8 *)q;
    while (i < old) { b[i] = a[i]; i = i + 1u; }
    osi_free(p);
    return q;
}
static void *osi_calloc(u32 n, u32 sz)    { void *p = osi_malloc(n * sz); 
                                            u32 i = 0; u8 *q = (u8 *)p;
                                            if (p == 0) { return 0; }
                                            while (i < n * sz) { q[i] = 0u; i = i + 1u; }
                                            return p; }
static void *osi_zalloc(u32 n)            { return osi_calloc(1u, n); }

void s3_wifi_osi_init(void)
{
    void **tab = (void **)&g_wifi_osi_funcs;
    u32 n = (u32)(sizeof(wifi_osi_funcs_t) / sizeof(void *));
    u32 i = 0;

    while (i < n) {
        tab[i] = (void *)glue_ret0;
        i = i + 1u;
    }
    /* 头尾两个字不是函数指针，而是版本号和魔数 —— 闭源库会用它们校验整张表：
     *   ESP_WIFI_OS_ADAPTER_VERSION = 0x00000008
     *   ESP_WIFI_OS_ADAPTER_MAGIC   = 0xDEADBEAF
     * 之前这里全被填成了 glue_ret0，所以 wifi_osi_funcs_register() 返回
     * ESP_ERR_INVALID_ARG(0x102)。 */
    tab[0]     = (void *)0x00000008u;
    /* 注意：不能用 tab[n-1]！闭源库是从**固定偏移 0x1DC（第 119 个字）**读 _magic 的，
     * 而头文件里的 wifi_osi_funcs_t 因为 #if CONFIG_IDF_TARGET_ESP32C6/C5/C61
     * 那两个条件字段对我们不成立，算出来的 sizeof 比库期望的小两格。 */
    tab[119]   = (void *)0xDEADBEAFu;

    /* 接上真正的内存函数（字段号 = wifi_os_adapter.h 里的行号 - 30） */
    tab[43] = (void *)osi_malloc;          /* _malloc */
    tab[44] = (void *)osi_free;            /* _free */
    tab[46] = (void *)osi_free_heap_size;  /* _get_free_heap_size */
    tab[92] = (void *)osi_malloc;          /* _malloc_internal */
    tab[93] = (void *)osi_realloc;         /* _realloc_internal */
    tab[94] = (void *)osi_calloc;          /* _calloc_internal */
    tab[95] = (void *)osi_zalloc;          /* _zalloc_internal */
    tab[96] = (void *)osi_malloc;          /* _wifi_malloc */
    tab[97] = (void *)osi_realloc;         /* _wifi_realloc */
    tab[98] = (void *)osi_calloc;          /* _wifi_calloc */
    tab[99] = (void *)osi_zalloc;          /* _wifi_zalloc（若有） */
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

/* 只有**不链接** WiFi 闭源库的普通构建才需要这个弱兜底。
 * 注意：弱定义会满足引用，导致归档成员不被拉取 —— 所以 WiFi 构建必须定义
 * S3_WITH_WIFI 把它关掉。 */
#if !defined(S3_WITH_WIFI)
__attribute__((weak)) int esp_wifi_init_internal(const void *cfg)
{
    (void)cfg;
    return -1;
}
#endif
