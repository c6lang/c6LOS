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

extern volatile u32 g_osi_calls[128];
extern int ets_printf(const char *fmt, ...);

/* 诊断：把库的回调序列记进 RTC 慢存（0x50000000 区，CPU/看门狗复位都不丢），
 * 串口不可靠时用 `esptool read-mem 0x50000200` 直接读回来。
 *   0x50000200 = 回调总次数
 *   0x50000204 + 4*(n & 63) = 第 n 次回调的字段号                          */
#define OSI_RING_CNT  ((volatile u32 *)0x50000200u)
#define OSI_RING_BUF  ((volatile u32 *)0x50000204u)
static u32 g_osi_ring_n = 0u;

#define OSI_HIT(i) do { \
    g_osi_calls[(i)] = g_osi_calls[(i)] + 1u; \
    { \
        u32 k_ = g_osi_ring_n; \
        OSI_RING_BUF[k_ & 63u] = (u32)(i); \
        g_osi_ring_n = k_ + 1u; \
        *OSI_RING_CNT = g_osi_ring_n; \
    } } while (0)

#define HIT_OF(f) OSI_HIT((u32)(__builtin_offsetof(wifi_osi_funcs_t, f) / 4u))

/* 通用的空实现：不碰参数、返回 0 */
static u32 glue_ret0(void)
{
    return 0u;
}


/* ---- 真正可用的 OSI 内存函数（其余仍为 glue_ret0 桩）----
 * 注意：port/s3/fast.c 里的系统堆只在非 LEAN 构建里链接，而 WiFi 场景必须
 * 用 S3_LEAN=1，所以这里**自建一个独立的堆**，放在空闲 DRAM 里。
 * 0x3FCB0000 ~ 0x3FCD9000 之间是可用区间（避开栈和坏区）。 */
/* 堆内存放在 **BSS 静态数组**里：由 IDF bootloader 统一清零、不会和
 * .data/.bss 里的其它符号重叠（之前硬编码 0x3FCB8000 正好压在 _bss 段里，
 * 一分配就踩坏内核全局变量）。 */
static u32 g_wifi_heap_mem[16384];      /* 64KB */
#define S3_WIFI_HEAP_BASE ((u32)g_wifi_heap_mem)
#define S3_WIFI_HEAP_SIZE ((u32)sizeof(g_wifi_heap_mem))

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
    OSI_HIT(43);
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
    OSI_HIT(44);
    hdr = (WhBlk *)((u8 *)p - sizeof(WhBlk));
    if (hdr->magic == WH_MAGIC) { hdr->used = 0u; }
}


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


/* ---- OSI 队列：自包含的环形缓冲（WiFi 初始化阶段就要用）---- */
typedef struct { u32 len; u32 size; u32 head; u32 tail; u32 count; u8 *buf; } WQ;

static u32 osi_wifi_create_queue(u32 len, u32 size)
{
    WQ *q = (WQ *)osi_malloc((u32)sizeof(WQ) + len * size);
    OSI_HIT(94);
    if (q == 0) { ets_printf("QFAIL %u %u\n", len, size); return 0u; }
    ets_printf("Qok %u %u -> %08x\n", len, size, (u32)(void *)q);
    q->len = len; q->size = size; q->head = 0u; q->tail = 0u; q->count = 0u;
    q->buf = (u8 *)(q + 1);
    return (u32)q;
}

static u32 osi_wifi_queue_send(u32 qh, const void *item, u32 ticks)
{
    WQ *q = (WQ *)qh;
    const u8 *src = (const u8 *)item;
    u8 *dst;
    u32 i = 0u;
    if (q == 0 || item == 0 || q->count >= q->len) { return 0u; }
    dst = q->buf + q->tail * q->size;
    while (i < q->size) { dst[i] = src[i]; i = i + 1u; }
    q->tail = (q->tail + 1u) % q->len;
    q->count = q->count + 1u;
    return 1u;
}

static u32 osi_wifi_queue_recv(u32 qh, void *item, u32 ticks)
{
    WQ *q = (WQ *)qh;
    u8 *dst = (u8 *)item;
    const u8 *src;
    u32 i = 0u;
    if (q == 0 || item == 0 || q->count == 0u) { return 0u; }
    src = q->buf + q->head * q->size;
    while (i < q->size) { dst[i] = src[i]; i = i + 1u; }
    q->head = (q->head + 1u) % q->len;
    q->count = q->count - 1u;
    return 1u;
}

static void osi_wifi_delete_queue(u32 qh) { if (qh != 0u) { osi_free((void *)qh); } }

/* ===================== OSI 层：其余基础函数 =====================
 * 说明：wifi_glue.c 整体用 windowed ABI 编译（闭源库回调我们时是 call8/retw），
 * 这里只调用 call0 的外部符号（port_puts / os_tick_get）——windowed → call0
 * 是安全方向（call0 被调方不发 entry，不会泄漏寄存器窗口）。 */
#include "c6os/port.h"
#include "c6os/tick.h"

/* 调用计数：上板用信标读，能看出库走到哪一步了 */
volatile u32 g_osi_calls[128];


/* ---- 信号量 ---- */
typedef struct { u32 magic; u32 cnt; u32 max; } WSem;
#define WSEM_MAGIC 0x53454D31u

static void *osi_semphr_create(u32 max, u32 init)
{
    WSem *s = (WSem *)osi_malloc((u32)sizeof(WSem));
    OSI_HIT(13);
    if (s == 0) { return 0; }
    s->magic = WSEM_MAGIC; s->cnt = init; s->max = max;
    return (void *)s;
}
static void osi_semphr_delete(void *sem) { if (sem != 0) { osi_free(sem); } }
static int osi_semphr_take(void *sem, u32 ticks)
{
    WSem *s = (WSem *)sem;
    OSI_HIT(15);
    if (s == 0 || s->magic != WSEM_MAGIC) { return 0; }
    if (s->cnt == 0u) { return 0; }     /* 没有真正阻塞（单线程初始化阶段够用） */
    s->cnt = s->cnt - 1u;
    return 1;
}
static int osi_semphr_give(void *sem)
{
    WSem *s = (WSem *)sem;
    if (s == 0 || s->magic != WSEM_MAGIC) { return 0; }
    s->cnt = s->cnt + 1u;
    return 1;
}

/* ---- 互斥量（复用信号量）---- */
static void *osi_mutex_create(void)
{
    OSI_HIT(18);
    return osi_semphr_create(1u, 1u);      /* 互斥量创建即可用 */
}
static void osi_mutex_delete(void *m) { osi_semphr_delete(m); }
static int osi_mutex_lock(void *m)
{
    WSem *s = (WSem *)m;
    OSI_HIT(21);
    if (s == 0 || s->magic != WSEM_MAGIC) { return 0; }
    if (s->cnt != 0u) { s->cnt = s->cnt - 1u; }
    return 1;
}
static int osi_mutex_unlock(void *m)
{
    WSem *s = (WSem *)m;
    OSI_HIT(22);
    if (s == 0 || s->magic != WSEM_MAGIC) { return 0; }
    if (s->cnt < s->max) { s->cnt = s->cnt + 1u; }
    return 1;
}
static void *osi_recursive_mutex_create(void)
{
    OSI_HIT(19);
    return osi_semphr_create(0xFFFFFFFFu, 1u);   /* 递归锁：给个大上限即可 */
}

/* 当前线程的信号量（库里拿它做阻塞等待；单线程阶段给一个固定的即可） */
static WSem g_thread_sem = { WSEM_MAGIC, 0u, 1u };
static void *osi_thread_semphr_get(void) { OSI_HIT(17); return (void *)&g_thread_sem; }

/* ---- 事件组 ---- */
typedef struct { u32 magic; volatile u32 bits; } WEvt;
#define WEVT_MAGIC 0x45565431u

static void *osi_event_group_create(void)
{
    HIT_OF(_event_group_create);
    WEvt *e = (WEvt *)osi_malloc((u32)sizeof(WEvt));
    OSI_HIT(31);
    if (e == 0) { return 0; }
    e->magic = WEVT_MAGIC; e->bits = 0u;
    return (void *)e;
}
static void osi_event_group_delete(void *g) { if (g != 0) { osi_free(g); } }
static u32 osi_event_group_set_bits(void *g, u32 bits)
{
    WEvt *e = (WEvt *)g;
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    e->bits = e->bits | bits;
    return e->bits;
}
static u32 osi_event_group_clear_bits(void *g, u32 bits)
{
    WEvt *e = (WEvt *)g;
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    e->bits = e->bits & ~bits;
    return e->bits;
}
static u32 osi_event_group_wait_bits(void *g, u32 bits, int clear, int wait_all,
                                     u32 ticks, u32 *out)
{
    HIT_OF(_event_group_wait_bits);
    WEvt *e = (WEvt *)g;
    u32 got;
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    got = e->bits & bits;
    if (wait_all != 0) {
        if (got != bits) { return 0u; }
    } else {
        if (got == 0u) { return 0u; }
    }
    if (clear != 0) { e->bits = e->bits & ~bits; }
    if (out != 0) { *out = e->bits; }
    return 1u;
}

/* ---- 通用队列（_queue_* 系列，和 _wifi_* 用同一实现）---- */
static u32 osi_queue_create(u32 len, u32 size) { OSI_HIT(23); return osi_wifi_create_queue(len, size); }
static void osi_queue_delete(u32 q) { osi_wifi_delete_queue(q); }
static int osi_queue_send(u32 q, void *item, u32 ticks) { OSI_HIT(25); return (int)osi_wifi_queue_send(q, item, ticks); }
static int osi_queue_recv(u32 q, void *item, u32 ticks) { OSI_HIT(29); return (int)osi_wifi_queue_recv(q, item, ticks); }
static int osi_queue_msg_waiting(u32 q)
{
    WQ *p = (WQ *)q;
    if (p == 0) { return 0; }
    return (int)p->count;
}

/* ---- 任务（初始化阶段单线程，先给非零句柄）---- */
static u32 g_fake_task = 0x7A5B0001u;
static int osi_task_create(void *fn, const char *name, u32 stack, void *arg,
                           u32 prio, void *handle)
{
    HIT_OF(_task_create);
    OSI_HIT(37);
    if (handle != 0) { *(u32 *)handle = (u32)&g_fake_task; }
    return 1;                              /* 1 = pdPASS */
}
static void osi_task_delete(void *h) { (void)h; }
static u32 osi_task_get_current_task(void) { return (u32)&g_fake_task; }
static u32 osi_task_ms_to_tick(u32 ms) { return ms; }
static u32 osi_task_get_max_priority(void) { OSI_HIT(42); return 25u; }
static void osi_task_yield_from_isr(void) { }

/* ---- 日志：把库自己打的信息送到我们的串口 ---- */
/* 注意：这里**不能**调 port_puts —— 它是 call0 编译的，从 windowed 代码
 * 调过去会把寄存器窗口搞乱（实测 cause=0x14 InstrPageTableErr，
 * 返回地址被 OR 上 bit31）。ROM 的 ets_printf 从 windowed 调是安全的
 * （s3_win_log 一直这么用）。 */
extern int ets_printf(const char *fmt, ...);

/* 只有落在 flash/IROM/IRAM/DRAM 里的指针才当字符串用。 */
static int c6_ok_str(u32 p)
{
    if (p >= 0x3C000000u && p < 0x3C800000u) { return 1; }   /* DROM */
    if (p >= 0x42000000u && p < 0x42800000u) { return 1; }   /* IROM */
    if (p >= 0x40300000u && p < 0x40400000u) { return 1; }   /* IRAM */
    if (p >= 0x3F000000u && p < 0x3FE00000u) { return 1; }   /* DRAM */
    return 0;
}

/* 注意：窗口是 8 个寄存器，call8 的轮转量是 8，所以库放在 a10..a15 的参数
 * 到我们这里就是 a2..a7 —— 也就是 6 个参数。
 *   _log_write(level, tag, fmt, arg)
 *   _log_writev(level, tag, fmt, &va_list, buf, len)                        */
/* ROM ld 里没有 vsnprintf，所以自己按格式串把 va_list 里的参数还原出来。
 * Xtensa 的 va_list 是"指向参数保存区的指针"，这里 ap 又是这个 va_list
 * 的地址，所以 *(u32*)ap 才是参数区首地址。 */
static void c6_log_fmt(const char *fmt, void *app)
{
    char out[128];
    u32 o = 0u;
    u32 n = 0u;
    u32 *args = 0;

    if (app != 0) {
        u32 p = *(u32 *)app;
        if (p >= 0x3F000000u && p < 0x3FE00000u) { args = (u32 *)p; }
    }
    while (fmt != 0 && *fmt != '\0' && o < (u32)sizeof(out) - 16u) {
        if (*fmt != '%') { out[o] = *fmt; o++; fmt++; continue; }
        fmt++;
        if (*fmt == '%') { out[o] = '%'; o++; fmt++; continue; }
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' ||
               (*fmt >= '0' && *fmt <= '9') || *fmt == '.' ||
               *fmt == 'l' || *fmt == 'h' || *fmt == 'z') { fmt++; }
        {
            u32 v = 0u;
            u32 k;
            u32 i;
            char t[12];
            if (args != 0) { v = args[n]; }
            n++;
            if (*fmt == 's') {
                u32 sp = v;
                if (c6_ok_str(sp)) {
                    const char *cs = (const char *)sp;
                    while (*cs != '\0' && o < (u32)sizeof(out) - 2u) { out[o] = *cs; o++; cs++; }
                } else { out[o] = '?'; o++; }
            } else if (*fmt == 'c') {
                out[o] = (char)(v & 0xFFu); o++;
            } else if (*fmt == 'd') {
                u32 u;
                int d = (int)v;
                k = 0u;
                if (d < 0) { out[o] = '-'; o++; u = (u32)(-d); } else { u = (u32)d; }
                if (u == 0u) { t[k] = '0'; k++; }
                while (u != 0u) { t[k] = (char)('0' + (u % 10u)); k++; u = u / 10u; }
                for (i = 0u; i < k; i++) { out[o] = t[k - 1u - i]; o++; }
            } else if (*fmt == 'x' || *fmt == 'X' || *fmt == 'u' || *fmt == 'p') {
                u32 u = v;
                u32 base = (*fmt == 'u') ? 10u : 16u;
                k = 0u;
                if (*fmt == 'p') { out[o] = '0'; o++; out[o] = 'x'; o++; }
                if (u == 0u) { t[k] = '0'; k++; }
                while (u != 0u) { t[k] = "0123456789abcdef"[u % base]; k++; u = u / base; }
                for (i = 0u; i < k; i++) { out[o] = t[k - 1u - i]; o++; }
            } else {
                out[o] = '?'; o++;
            }
            if (*fmt != '\0') { fmt++; }
        }
    }
    out[o] = '\0';
    ets_printf("%s", out);
}

static void osi_log_write(u32 level, const char *tag, const char *fmt, const char *arg)
{
    OSI_HIT(83);
    if (level < 2u) {
        ets_printf("[w%u]%s|", level, c6_ok_str((u32)(void *)tag) ? tag : "?");
        if (c6_ok_str((u32)(void *)fmt)) { ets_printf("%s", fmt); }
        ets_printf("\n");
    }
    (void)arg;
}

static void osi_log_writev(u32 level, const char *tag, const char *fmt,
                           void *ap, char *buf, u32 len)
{
    OSI_HIT(84);
    if (level > 2u) { return; }             /* 只打 WARN/ERROR，避免刷屏 */
    ets_printf("[v%u]%s|", level, c6_ok_str((u32)(void *)tag) ? tag : "?");
    if (!c6_ok_str((u32)(void *)fmt)) {
        /* fmt 不是字符串：把 va_list 里的东西直接摊开看 */
        u32 p = (ap != 0) ? *(u32 *)ap : 0u;
        ets_printf("(fmt=%08x)", (u32)(void *)fmt);
        if (p >= 0x3F000000u && p < 0x3FE00000u) {
            u32 i;
            u32 *w = (u32 *)p;
            ets_printf(" args@%08x:", p);
            for (i = 0u; i < 6u; i++) { ets_printf(" %08x", w[i]); }
        }
        ets_printf("\n");
        return;
    }
    c6_log_fmt(fmt, ap);
    ets_printf("\n");
    (void)buf; (void)len;
}
static u32 osi_log_timestamp(void) { OSI_HIT(85); return 0u; }

/* ---- 时间 / 随机数 ---- */
static u64 osi_esp_timer_get_time(void)
{
    HIT_OF(_esp_timer_get_time);
    u32 hz = os_tick_hz();
    u64 t = os_tick_get();
    if (hz == 0u) { return t * 1000u; }
    return (t * 1000000u) / (u64)hz;
}
static u32 osi_rand(void) { static u32 r = 0x12345678u; r = r * 1103515245u + 12345u; return r >> 16; }
static u32 osi_get_time(void) { return (u32)(osi_esp_timer_get_time() / 1000000u); }
static u32 osi_slowclk_cal_get(void) { return 0u; }

/* ---- 其它琐碎项 ---- */
static int osi_env_is_chip(void) {
    HIT_OF(_env_is_chip); return 1; }
static u32 osi_read_mac(u8 *mac, u32 type)
{
    HIT_OF(_read_mac);
    u32 i = 0;
    (void)type;
    if (mac != 0) { while (i < 6u) { mac[i] = 0u; i = i + 1u; } }
    return 0u;
}

/* 库对同一份实现会用不同槽位调用（内部堆 / wifi 专用堆），分开包装
 * 才能从 RTC 追踪里看出到底是哪个槽位在失败。 */
static void *osi_malloc_internal(u32 n)  { OSI_HIT(86); return osi_malloc(n); }
static void *osi_realloc_internal(void *p, u32 n) { OSI_HIT(87); return osi_realloc(p, n); }
static void *osi_calloc_internal(u32 n, u32 sz)   { OSI_HIT(88); return osi_calloc(n, sz); }
static void *osi_zalloc_internal(u32 n)  { OSI_HIT(89); return osi_calloc(1u, n); }
static void *osi_wifi_malloc(u32 n)      { OSI_HIT(90); return osi_malloc(n); }
static void *osi_wifi_realloc(void *p, u32 n)     { OSI_HIT(91); return osi_realloc(p, n); }
static void *osi_wifi_calloc(u32 n, u32 sz)       { OSI_HIT(92); return osi_calloc(n, sz); }
static void *osi_wifi_zalloc(u32 n)      { OSI_HIT(93); return osi_calloc(1u, n); }

static u32 osi_free_heap_size(void) { return S3_WIFI_HEAP_SIZE / 2u; }

/* ---- 表装配：用 __builtin_offsetof，彻底避免手数下标 ---- */
#define OSI_SET(slot, fn) \
    (((void **)(void *)&g_wifi_osi_funcs)[__builtin_offsetof(wifi_osi_funcs_t, slot) / 4u] \
        = (void *)(fn))

/* 编译期断言：_magic 必须在偏移 0x1DC（库硬读这个位置） */
typedef char s3_osi_magic_offset_check[
    (__builtin_offsetof(wifi_osi_funcs_t, _magic) == 0x1DCu) ? 1 : -1];
typedef char s3_osi_size_check[(sizeof(wifi_osi_funcs_t) == 480u) ? 1 : -1];

void s3_wifi_osi_init(void)
{
    void **tab = (void **)(void *)&g_wifi_osi_funcs;
    u32 n = (u32)(sizeof(wifi_osi_funcs_t) / 4u);
    u32 i = 0;

    while (i < n) { tab[i] = (void *)glue_ret0; i = i + 1u; }

    /* 头尾不是函数指针，而是版本号和魔数：
     *   ESP_WIFI_OS_ADAPTER_VERSION = 0x00000008
     *   ESP_WIFI_OS_ADAPTER_MAGIC   = 0xDEADBEAF   （固定偏移 0x1DC = 第 119 个字） */
    OSI_SET(_version, 0x00000008u);
    OSI_SET(_magic,   0xDEADBEAFu);

    /* ---- 内存 ---- */
    OSI_SET(_malloc,          osi_malloc);
    OSI_SET(_free,            osi_free);
    OSI_SET(_get_free_heap_size, osi_free_heap_size);
    OSI_SET(_malloc_internal, osi_malloc_internal);
    OSI_SET(_realloc_internal, osi_realloc_internal);
    OSI_SET(_calloc_internal, osi_calloc_internal);
    OSI_SET(_zalloc_internal, osi_zalloc_internal);
    OSI_SET(_wifi_malloc,     osi_wifi_malloc);
    OSI_SET(_wifi_realloc,    osi_wifi_realloc);
    OSI_SET(_wifi_calloc,     osi_wifi_calloc);
    OSI_SET(_wifi_zalloc,     osi_wifi_zalloc);

    /* ---- 队列 ---- */
    OSI_SET(_wifi_create_queue, osi_wifi_create_queue);
    OSI_SET(_wifi_delete_queue, osi_wifi_delete_queue);
    OSI_SET(_queue_create,      osi_queue_create);
    OSI_SET(_queue_delete,      osi_queue_delete);
    OSI_SET(_queue_send,        osi_queue_send);
    OSI_SET(_queue_send_from_isr, osi_queue_send);
    OSI_SET(_queue_send_to_back,  osi_queue_send);
    OSI_SET(_queue_send_to_front, osi_queue_send);
    OSI_SET(_queue_recv,        osi_queue_recv);
    OSI_SET(_queue_msg_waiting, osi_queue_msg_waiting);

    /* ---- 信号量 / 互斥量 ---- */
    OSI_SET(_semphr_create,     osi_semphr_create);
    OSI_SET(_semphr_delete,     osi_semphr_delete);
    OSI_SET(_semphr_take,       osi_semphr_take);
    OSI_SET(_semphr_give,       osi_semphr_give);
    OSI_SET(_wifi_thread_semphr_get, osi_thread_semphr_get);
    OSI_SET(_mutex_create,      osi_mutex_create);
    OSI_SET(_recursive_mutex_create, osi_recursive_mutex_create);
    OSI_SET(_mutex_delete,      osi_mutex_delete);
    OSI_SET(_mutex_lock,        osi_mutex_lock);
    OSI_SET(_mutex_unlock,      osi_mutex_unlock);

    /* ---- 事件组 ---- */
    OSI_SET(_event_group_create,     osi_event_group_create);
    OSI_SET(_event_group_delete,     osi_event_group_delete);
    OSI_SET(_event_group_set_bits,   osi_event_group_set_bits);
    OSI_SET(_event_group_clear_bits, osi_event_group_clear_bits);
    OSI_SET(_event_group_wait_bits,  osi_event_group_wait_bits);

    /* ---- 任务 ---- */
    OSI_SET(_task_create,            osi_task_create);
    OSI_SET(_task_create_pinned_to_core, osi_task_create);
    OSI_SET(_task_delete,            osi_task_delete);
    OSI_SET(_task_get_current_task,  osi_task_get_current_task);
    OSI_SET(_task_ms_to_tick,        osi_task_ms_to_tick);
    OSI_SET(_task_get_max_priority,  osi_task_get_max_priority);
    OSI_SET(_task_yield_from_isr,    osi_task_yield_from_isr);

    /* ---- 杂项 ---- */
    OSI_SET(_env_is_chip,   osi_env_is_chip);
    OSI_SET(_read_mac,      osi_read_mac);
    OSI_SET(_log_write,     osi_log_write);
    OSI_SET(_log_writev,    osi_log_writev);
    OSI_SET(_log_timestamp, osi_log_timestamp);
    OSI_SET(_esp_timer_get_time, osi_esp_timer_get_time);
    OSI_SET(_rand,          osi_rand);
    OSI_SET(_get_random,    osi_rand);
    OSI_SET(_random,        osi_rand);
    OSI_SET(_get_time,      osi_get_time);
    OSI_SET(_slowclk_cal_get, osi_slowclk_cal_get);
    OSI_SET(_spin_lock_create, osi_mutex_create);
    OSI_SET(_spin_lock_delete, osi_mutex_delete);
    OSI_SET(_is_from_isr,   glue_ret0);
}

/* WiFi 初始化配置块：放在 BSS 里，由 .c6 侧经桥取地址后填写。
 * 以前 .c6 里写死 0x3FCA2000，那个地址落在 .data 段内，填配置就把
 * 我们自己的全局变量覆盖了。 */
u32 g_wifi_cfg_blob[64];

void *s3_wifi_cfg_ptr(void) { return (void *)g_wifi_cfg_blob; }

/* ===================== WiFi 启动流程（全部在 C 侧） =====================
 * 之前这段是在 .c6 里写的，但 .c6 生成的代码在跨函数调用时会把局部变量
 * （配置块指针）弄丢，导致 esp_wifi_init_internal 收到字符串地址而不是 cfg。
 * wifi_glue.c 本身就是 windowed 编译，可以直接调闭源库，不需要绕桥。 */
extern int esp_wifi_init_internal(const void *cfg);

static void wcfg(u32 off, u32 v)
{
    *(volatile u32 *)((u8 *)g_wifi_cfg_blob + off) = v;
}

u32 s3_wifi_bringup(void)
{
    u32 i = 0u;
    /* 库的 wifi_log 先看它自己的 g_log_level，默认级别会把错误信息过滤掉，
     * 调高之后 init 失败的原因才会通过 _log_writev 送到串口。 */
    { extern int g_log_level; g_log_level = 4; }

    ets_printf("B0 osi0=%08x osi@%08x calls@%08x\n",
               (u32)g_osi_calls[0], (u32)(void *)g_wifi_osi_funcs._version,
               (u32)(void *)&g_osi_calls[0]);
    while (i < 40u) { *(volatile u32 *)((u8 *)g_wifi_cfg_blob + i * 4u) = 0u; i = i + 1u; }

    /* ---- 严格照 IDF 官方 WIFI_INIT_CONFIG_DEFAULT() 填，不再猜 --------------
     * 依据（官方源文件）：
     *   components/esp_wifi/include/esp_wifi.h     WIFI_INIT_CONFIG_DEFAULT()
     *   components/esp_wifi/Kconfig                各 CONFIG_ESP_WIFI_* 的 default
     *   components/esp_wifi/esp32s3/esp_adapter.c   官方 OSI 表
     * 偏移见 include/c6os/wifi_osi.h 里 wifi_init_config_t 的布局。          */
    wcfg(0u,   (u32)(void *)&g_wifi_osi_funcs);   /* osi_funcs */
    /* wpa_crypto_funcs @4..47：官方实现在
     * wpa_supplicant/src/crypto/crypto_ops.c 的 g_wifi_default_wpa_crypto_funcs，
     * 依赖 mbedtls；等接 TLS 时一起补。init 阶段库只校验 size/version。 */
    wcfg(4u,   44u);            /* wpa_crypto_funcs.size    = sizeof(...) */
    wcfg(8u,   1u);             /* wpa_crypto_funcs.version = ESP_WIFI_CRYPTO_VERSION */
    wcfg(48u,  10u);            /* static_rx_buf_num      Kconfig default 10  */
    wcfg(52u,  32u);            /* dynamic_rx_buf_num     default 32          */
    wcfg(56u,  1u);             /* tx_buf_type            default 1 = dynamic */
    wcfg(60u,  0u);             /* static_tx_buf_num      动态模式下为 0      */
    wcfg(64u,  32u);            /* dynamic_tx_buf_num     default 32          */
    wcfg(68u,  1u);             /* rx_mgmt_buf_type       DYNAMIC_RX_MGMT_BUF default 1 */
    wcfg(72u,  5u);             /* rx_mgmt_buf_num        default 5           */
    wcfg(76u,  0u);             /* cache_tx_buf_num       feature_caps 未开 CACHE_TX_BUFFER */
    wcfg(80u,  0u);             /* csi_enable             default n           */
    wcfg(84u,  1u);             /* ampdu_rx_enable        default y           */
    wcfg(88u,  1u);             /* ampdu_tx_enable        default y           */
    wcfg(92u,  0u);             /* amsdu_tx_enable        default n           */
    wcfg(96u,  0u);             /* nvs_enable             官方默认 y，但需要 NVS 子系统，先关 */
    wcfg(100u, 0u);             /* nano_enable            default n           */
    wcfg(104u, 6u);             /* rx_ba_win              default 6           */
    wcfg(108u, 0u);             /* wifi_task_core_id      单核跑，用 0 号核    */
    wcfg(112u, 752u);           /* beacon_max_len         default 752         */
    wcfg(116u, 32u);            /* mgmt_sbuf_num          default 32          */
    wcfg(120u, 0u);             /* feature_caps 低 32 位   先全关             */
    wcfg(124u, 0u);             /* feature_caps 高 32 位                       */
    wcfg(128u, 1u);             /* sta_disconnected_pm    default y           */
    wcfg(132u, 7u);             /* espnow_max_encrypt_num default 7           */
    wcfg(136u, 1u);             /* tx_hetb_queue_num      default 1           */
    wcfg(140u, 0u);             /* dump_hesigb_enable     default n           */
    wcfg(144u, 0x1F2F3F4Fu);    /* magic = WIFI_INIT_CONFIG_MAGIC             */

    s3_wifi_osi_init();
    ets_printf("B1 slot13=%08x slot18=%08x magic=%08x ver=%08x\n",
               (u32)(void *)g_wifi_osi_funcs._semphr_create,
               (u32)(void *)g_wifi_osi_funcs._mutex_create,
               (u32)(void *)g_wifi_osi_funcs._magic,
               (u32)(void *)g_wifi_osi_funcs._version);
    ets_printf("B2 cfg@%08x osi=%08x magic@144=%08x\n",
               (u32)(void *)g_wifi_cfg_blob,
               *(volatile u32 *)((u8 *)g_wifi_cfg_blob + 0u),
               *(volatile u32 *)((u8 *)g_wifi_cfg_blob + 144u));
    return (u32)(int)esp_wifi_init_internal((const void *)g_wifi_cfg_blob);
}
void *s3_wifi_calls_ptr(void) { return (void *)g_osi_calls; }

/* 诊断：把关键 OSI 函数的调用计数打到串口（供 .c6 侧经桥调用） */
static void pnm(const char *n, u32 v)
{
    ets_printf("%s=%08x ", n, v);
}

void s3_osi_dump(void)
{
    ets_printf("\n[osi] ");
    pnm("mutex",    g_osi_calls[18]);
    pnm("lock",     g_osi_calls[21]);
    pnm("qcreat",   g_osi_calls[23]);
    pnm("sem",      g_osi_calls[13]);
    pnm("task",     g_osi_calls[37]);
    pnm("evt",      g_osi_calls[31]);
    pnm("wmalloc",  g_osi_calls[90]);
    pnm("log",      g_osi_calls[83]);
    ets_printf("\n");
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
/* ---- 内存函数：必须由 windowed 编译单元提供 ------------------------------
 * 闭源库是 windowed ABI，它用 `call8 memset` 调过来。之前用 call0 编的版本
 * （rt/c6rt.c）会让返回地址带上 CALL 的窗口标签（bit31:30）并破坏窗口簿记，
 * 表现为 `retw` 处 IllegalInstruction / InstrPageTableErr。
 * 让编译器按 windowed ABI 生成 entry/retw 就一切正常 —— 同一个文件里的
 * strlen/strcpy 一直是这么工作的。                                          */
void *memset(void *dst, int c, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n != 0u) { *d = (unsigned char)c; d++; n--; }
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n != 0u) { *d = *s; d++; s++; n--; }
    return dst;
}

void *memmove(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n != 0u) { *d = *s; d++; s++; n--; }
    } else {
        d = d + n;
        s = s + n;
        while (n != 0u) { d--; s--; *d = *s; n--; }
    }
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n != 0u) {
        if (*x != *y) { return (int)*x - (int)*y; }
        x++; y++; n--;
    }
    return 0;
}

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
