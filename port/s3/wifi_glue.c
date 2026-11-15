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
volatile u32 g_osi_calls[128];
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
    u32 want = 0u;
    WhBlk *b = 0;
    u32 end = 0u;

    OSI_HIT(43);
    /* 防御：库可能请求一个离谱的大小，n+15 会回绕成 0（want 变 0），
     * 调用方再按原大小写下去就把整片 .bss 踩烂 —— 表现为内核全局、
     * ISR 静态计数被莫名改写（本轮就出现过 ISR 分段计数自相矛盾）。 */
    if (n == 0u || n > (S3_WIFI_HEAP_SIZE - (u32)sizeof(WhBlk) - 64u)) {
        *(volatile u32 *)0x50000740u = n;
        *(volatile u32 *)0x50000744u = *(volatile u32 *)0x50000744u + 1u;
        return 0;
    }
    want = (n + 15u) & ~15u;
    wh_ensure();
    b = (WhBlk *)S3_WIFI_HEAP_BASE;
    end = S3_WIFI_HEAP_BASE + S3_WIFI_HEAP_SIZE;
    /* 防御：节点必须整个落在堆数组内，否则会把数组外的邻居当成块头。 */
    while (((u32)b + (u32)sizeof(WhBlk) <= end) && b->magic == WH_MAGIC) {
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
        if ((u32)b + (u32)sizeof(WhBlk) + b->size > end) { break; }
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
static void *osi_calloc(u32 n, u32 sz)
{
    void *p = 0;
    u32 i = 0u;
    u32 total = 0u;
    u8 *q = 0;

    /* 防御：n*sz 溢出会让 osi_malloc 拿到一个很小的值、随后这里按原尺寸清零，
     * 直接把后面的 .bss 全写掉。 */
    if (n == 0u || sz == 0u || n > (0xFFFFFFFFu / sz)) { return 0; }
    total = n * sz;
    p = osi_malloc(total);
    if (p == 0) { return 0; }
    q = (u8 *)p;
    while (i < total) { q[i] = 0u; i = i + 1u; }
    return p;
}
static void *osi_zalloc(u32 n)            { return osi_calloc(1u, n); }


/* ---- 真 RTOS 对象 ------------------------------------------------------
 * 全部经 port/s3/w2c.S 里的 windowed→call0 薄壳调用内核：
 *   entry a1,32 / call0 os_xxx / retw
 * 这样既不会把 CALL 的窗口标签漏给内核，也不会破坏调用者的窗口簿记。
 *
 * 内核对象布局在这里按值内嵌（OsSem/OsQueue/OsEvent 都是纯 POD，字段与
 * include/c6os/{sem,queue,event}.h 完全一致）。                               */
typedef struct { u32 count; u32 max; u32 magic; u32 waiters; } WOsSem;
typedef struct { u8 *buf; u32 item_size; u32 cap; u32 head; u32 tail;
                 u32 count; u32 magic; u32 waiters_get; u32 waiters_put; } WOsQueue;
typedef struct { u32 bits; u32 magic; } WOsEvent;
typedef struct { u32 owner; u32 count; u32 magic; } WOsMutex;

extern int  s3_w2c_sem_init(void *s, u32 initial, u32 max);
extern int  s3_w2c_sem_destroy(void *s);
extern int  s3_w2c_sem_pend(void *s, u32 timeout);
extern int  s3_w2c_sem_post(void *s);
extern int  s3_w2c_mutex_init(void *m);
extern int  s3_w2c_mutex_destroy(void *m);
extern int  s3_w2c_mutex_pend(void *m, u32 timeout);
extern int  s3_w2c_mutex_post(void *m);
extern int  s3_w2c_queue_init(void *q, void *storage, u32 cap, u32 item_size);
extern int  s3_w2c_queue_put(void *q, const void *item, u32 timeout);
extern int  s3_w2c_queue_get(void *q, void *out, u32 timeout);
extern u32  s3_w2c_queue_count(void *q);
extern int  s3_w2c_queue_destroy(void *q);
extern int  s3_w2c_event_init(void *e);
extern int  s3_w2c_event_set(void *e, u32 bits);
extern int  s3_w2c_event_clear(void *e, u32 bits);
extern u32  s3_w2c_event_wait(void *e, u32 mask, u32 mode, u32 timeout);
extern int  s3_w2c_event_destroy(void *e);
extern int  s3_w2c_task_create(const char *name, void *entry, void *arg,
                               u32 prio, u32 stack);
extern int  s3_w2c_task_delete(u32 id);
extern void s3_w2c_task_delay(u32 ticks);
extern void s3_w2c_task_yield(void);
extern u64  s3_w2c_tick_get(void);
extern u32  s3_w2c_tick_hz(void);
extern u32  s3_w2c_current_task(void);
extern void *g_s3_cur[2];   /* port/s3/ctx.S 里的当前 ctx（按核下标） */

/* 阻塞原语进入/离开标记：0x50000370 = 最后进入的，0x50000374 = 最后离开的。
 * 两者不一致就说明卡在那个原语上。1=sem_pend 2=queue_get 3=event_wait 4=mutex_pend */
/* 按任务分开记（任务 id 取低 2 位做下标），避免两个任务互相覆盖：
 *   0x50000370 + 8*i = 任务 i 最后进入 / 最后离开 的阻塞原语
 *   1=sem_pend 2=queue_get 3=event_wait 4=mutex_pend */
#define BDIAG ((volatile u32 *)0x50000370u)
#define B_SLOT (((s3_w2c_current_task() & 3u) * 2u))
#define B_IN(id)   (BDIAG[B_SLOT] = (u32)(id))
#define B_OUT(id)  (BDIAG[B_SLOT + 1u] = (u32)(id))

#define C6_FOREVER 0xFFFFFFFFu

/* 内核 os_task_block_prepare 的约定是 **timeout 0 = 不设超时 = 永久等待**；
 * 直接把 IDF 的 portMAX_DELAY(0xFFFFFFFF) 当 tick 数传进去，会变成
 * "now + 0xFFFFFFFF"，立刻到期 → 每次等待都秒回超时，调用者就忙等。
 * 所有阻塞原语统一在这里翻译。 */
static u32 w_ticks(u32 t)
{
    return (t == C6_FOREVER) ? 0u : t;
}

/* ---- 信号量 ---- */
typedef struct { WOsSem s; u32 magic; } WSem;
#define WSEM_MAGIC 0x53454D31u

static WSem *wsem_new(u32 max, u32 init)
{
    WSem *s = (WSem *)osi_malloc((u32)sizeof(WSem));
    if (s == 0) { return 0; }
    if (s3_w2c_sem_init((void *)&s->s, init, max) != 0) { osi_free(s); return 0; }
    s->magic = WSEM_MAGIC;
    return s;
}

static void *osi_semphr_create(u32 max, u32 init)
{
    HIT_OF(_semphr_create);
    return (void *)wsem_new(max, init);
}
static void osi_semphr_delete(void *sem)
{
    WSem *s = (WSem *)sem;
    if (s != 0 && s->magic == WSEM_MAGIC) { (void)s3_w2c_sem_destroy((void *)&s->s); s->magic = 0u; osi_free(s); }
}
static int osi_semphr_take(void *sem, u32 ticks)
{
    WSem *s = (WSem *)sem;
    HIT_OF(_semphr_take);
    if (s == 0 || s->magic != WSEM_MAGIC) { return 0; }
    {
        int ok;
        B_IN(1u);
        ok = (s3_w2c_sem_pend((void *)&s->s, w_ticks(ticks)) == 0) ? 1 : 0;
        B_OUT(1u);
        return ok;
    }
}
static int osi_semphr_give(void *sem)
{
    WSem *s = (WSem *)sem;
    HIT_OF(_semphr_give);
    if (s == 0 || s->magic != WSEM_MAGIC) { return 0; }
    return (s3_w2c_sem_post((void *)&s->s) == 0) ? 1 : 0;
}

/* 当前任务用来阻塞的信号量。现在有真任务了，必须每个任务一个，
 * 否则两个任务会在同一个信号量上打架。用当前任务 token 做键。 */
#define WTHREAD_SLOTS 8
static WSem *g_thread_sems[WTHREAD_SLOTS];
static u32   g_thread_keys[WTHREAD_SLOTS];

static void *osi_thread_semphr_get(void)
{
    u32 key = s3_w2c_current_task();
    u32 i;
    HIT_OF(_wifi_thread_semphr_get);
    for (i = 0u; i < WTHREAD_SLOTS; i++) {
        if (g_thread_sems[i] != 0 && g_thread_keys[i] == key) { return (void *)g_thread_sems[i]; }
    }
    for (i = 0u; i < WTHREAD_SLOTS; i++) {
        if (g_thread_sems[i] == 0) {
            g_thread_sems[i] = wsem_new(1u, 0u);
            g_thread_keys[i] = key;
            return (void *)g_thread_sems[i];
        }
    }
    return 0;
}

/* ---- 互斥量 ------------------------------------------------------------
 * 必须用内核的 OsMutex（带 owner + 递归计数），不能用计数信号量冒充：
 * IDF 的 wifi_api_lock 是**递归**加锁的，用 max=1 的信号量会自己把自己锁死
 * （真板表现：init 走完 M1 之后彻底卡住，没有 TRAP 也没有返回）。            */
typedef struct { WOsMutex m; u32 magic; } WMtx;
#define WMTX_MAGIC 0x4D545831u

static void *wmux_new(void)
{
    WMtx *m = (WMtx *)osi_malloc((u32)sizeof(WMtx));
    if (m == 0) { return 0; }
    if (s3_w2c_mutex_init((void *)&m->m) != 0) { osi_free(m); return 0; }
    m->magic = WMTX_MAGIC;
    return (void *)m;
}

static void *osi_mutex_create(void)
{
    HIT_OF(_mutex_create);
    return wmux_new();
}
static void *osi_recursive_mutex_create(void)
{
    HIT_OF(_recursive_mutex_create);
    return wmux_new();
}
static void osi_mutex_delete(void *p)
{
    WMtx *m = (WMtx *)p;
    if (m != 0 && m->magic == WMTX_MAGIC) {
        (void)s3_w2c_mutex_destroy((void *)&m->m);
        m->magic = 0u;
        osi_free(m);
    }
}
static int osi_mutex_lock(void *p)
{
    WMtx *m = (WMtx *)p;
    HIT_OF(_mutex_lock);
    if (m == 0 || m->magic != WMTX_MAGIC) { return 0; }
    {
        int ok;
        B_IN(4u);
        ok = (s3_w2c_mutex_pend((void *)&m->m, 0u) == 0) ? 1 : 0;
        B_OUT(4u);
        return ok;
    }
}
static int osi_mutex_unlock(void *p)
{
    WMtx *m = (WMtx *)p;
    HIT_OF(_mutex_unlock);
    if (m == 0 || m->magic != WMTX_MAGIC) { return 0; }
    return (s3_w2c_mutex_post((void *)&m->m) == 0) ? 1 : 0;
}

/* ---- 事件组 ---- */
typedef struct { WOsEvent e; u32 magic; } WEvt;
#define WEVT_MAGIC 0x45565431u

static void *osi_event_group_create(void)
{
    WEvt *g = (WEvt *)osi_malloc((u32)sizeof(WEvt));
    HIT_OF(_event_group_create);
    if (g == 0) { return 0; }
    if (s3_w2c_event_init((void *)&g->e) != 0) { osi_free(g); return 0; }
    g->magic = WEVT_MAGIC;
    return (void *)g;
}
static void osi_event_group_delete(void *g)
{
    WEvt *e = (WEvt *)g;
    if (e != 0 && e->magic == WEVT_MAGIC) { (void)s3_w2c_event_destroy((void *)&e->e); e->magic = 0u; osi_free(e); }
}
static u32 osi_event_group_set_bits(void *g, u32 bits)
{
    WEvt *e = (WEvt *)g;
    HIT_OF(_event_group_set_bits);
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    (void)s3_w2c_event_set((void *)&e->e, bits);
    return e->e.bits;
}
static u32 osi_event_group_clear_bits(void *g, u32 bits)
{
    WEvt *e = (WEvt *)g;
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    (void)s3_w2c_event_clear((void *)&e->e, bits);
    return e->e.bits;
}
static u32 osi_event_group_wait_bits(void *g, u32 bits, int clear, int wait_all,
                                     u32 ticks)
{
    WEvt *e = (WEvt *)g;
    u32 got;
    HIT_OF(_event_group_wait_bits);
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    B_IN(3u);
    got = s3_w2c_event_wait((void *)&e->e, bits,
                            (wait_all != 0) ? 1u : 0u, w_ticks(ticks));
    B_OUT(3u);
    if (clear != 0 && got != 0u) { (void)s3_w2c_event_clear((void *)&e->e, got); }
    return got;
}

/* ---- 队列 ---- */
/* 注意第一个字段 self：闭源库把 `_wifi_create_queue` 的返回值当成 IDF 的
 * `wifi_static_queue_t { QueueHandle_t queue; void *items; }` 用 —— 它会读
 * **偏移 0 处的字**当内层队列句柄存下来（反汇编 pp_create_task 0x420343b8 里
 * 就是 `l32i a4, a10, 0`），之后又把这个值传给 `_queue_recv`。
 * 所以偏移 0 必须放"队列对象自己的地址"，库再拿回来时我们还能认出它。
 * （之前偏移 0 是数组成员的 buf 指针，于是库后来传进来的是 buf，
 *   magic 校验必然失败 → WiFi 任务在 _queue_recv 上死循环。） */
typedef struct { u32 self; WOsQueue q; u32 magic; } WQ;
#define WQ_MAGIC 0x51554531u

static u32 g_qn = 0u;
static u32 g_qbad = 0u;
static u32 g_ev_n = 0u;
static u32 g_sp_n = 0u;
static u32 g_rc_n = 0u;
static u32 g_ql_n = 0u;
static u32 g_ql_id[4];

/* 库有时候（实测）会把队列的 **buf 存储** 而不是队列对象本身传给
 * _queue_recv。这里按 magic 找不到就扫一遍 WiFi 堆，找出 buf == qh 的那个队列。
 * 扫描范围就是自己的 g_wifi_heap_mem，代价可控。 */
static WQ *wq_resolve(void *qh)
{
    WQ *q = (WQ *)qh;
    u32 p;
    if (q != 0 && q->magic == WQ_MAGIC) { return q; }
    if (qh == 0) { return 0; }
    for (p = S3_WIFI_HEAP_BASE; p + (u32)sizeof(WQ) <= S3_WIFI_HEAP_BASE + S3_WIFI_HEAP_SIZE; p += 16u) {
        WQ *c = (WQ *)p;
        if (c->magic == WQ_MAGIC && (u32)c->q.buf == (u32)qh) { return c; }
    }
    return 0;
}

static u32 osi_wifi_create_queue(u32 len, u32 size)
{
    WQ *q;
    u8 *buf;
    HIT_OF(_wifi_create_queue);
    if (len == 0u || size == 0u) { return 0u; }
    q = (WQ *)osi_malloc((u32)sizeof(WQ));
    if (q == 0) { return 0u; }
    buf = (u8 *)osi_malloc(len * size);
    if (buf == 0) { osi_free(q); return 0u; }
    if (s3_w2c_queue_init((void *)&q->q, (void *)buf, len, size) != 0) {
        osi_free(buf); osi_free(q); return 0u;
    }
    q->magic = WQ_MAGIC;
    q->self = (u32)q;
    *(volatile u32 *)0x50000310u = (u32)q;              /* 最近一次建出来的句柄 */
    *(volatile u32 *)0x50000364u = (u32)q->q.buf;       /* 顺便记下 buf，验证库传的是不是它 */
    if (g_qn < 4u) { *(volatile u32 *)(0x50000340u + 4u * g_qn) = (u32)q; }
    g_qn = g_qn + 1u;
    *(volatile u32 *)0x50000350u = g_qn;                /* 累计创建次数（BSS 计数，可信） */
    return (u32)q;
}
static void osi_wifi_delete_queue(u32 qh)
{
    WQ *q = (WQ *)qh;
    HIT_OF(_wifi_delete_queue);
    if (q == 0 || q->magic != WQ_MAGIC) { return; }
    (void)s3_w2c_queue_destroy((void *)&q->q);
    osi_free(q->q.buf);
    q->magic = 0u;
    osi_free(q);
}
static u32 osi_queue_create(u32 len, u32 size) { HIT_OF(_queue_create); return osi_wifi_create_queue(len, size); }
static void osi_queue_delete(void *qh) { osi_wifi_delete_queue((u32)qh); }
#define SDIAG ((volatile u32 *)0x50000320u)

static int osi_queue_send(void *qh, void *item, u32 ticks)
{
    WQ *q = wq_resolve(qh);
    int r;
    HIT_OF(_queue_send);
    if (q == 0 || q->magic != WQ_MAGIC) {
        SDIAG[0] = (u32)qh;                 /* 句柄不像队列 */
        SDIAG[1] = (u32)((WQ *)qh)->magic;
        SDIAG[2] = (u32)((WQ *)qh)->q.buf;
        SDIAG[3] = 0xBAD00000u;
        return 0;
    }
    r = s3_w2c_queue_put((void *)&q->q, item, w_ticks(ticks));
    if (r != 0) {
        SDIAG[3] = 0xBAD10000u | (u32)(-r);
        SDIAG[2] = ticks;
        SDIAG[1] = q->q.count;
        SDIAG[0] = (u32)item;
    }
    return (r == 0) ? 1 : 0;
}
/* 诊断记录区：0x50000300 放最近一次失败的线索，复位后 read-mem 可读 */
#define QDIAG ((volatile u32 *)0x50000300u)

static int osi_queue_recv(void *qh, void *item, u32 ticks)
{
    WQ *q = wq_resolve(qh);
    int r;
    HIT_OF(_queue_recv);
    if (g_qbad == 0u && (q == 0 || q->magic != WQ_MAGIC)) {
        g_qbad = 1u;
        *(volatile u32 *)0x50000360u = (u32)qh;   /* 第一个不过 magic 的 recv 句柄 */
    }
    if (q == 0 || q->magic != WQ_MAGIC) {
        QDIAG[0] = (u32)qh;                 /* 句柄不像我们的队列 */
        QDIAG[1] = (u32)((WQ *)qh)->magic;  /* 那里到底放的是什么 */
        QDIAG[2] = (u32)((WQ *)qh)->q.cap;
        QDIAG[3] = (u32)((WQ *)qh)->q.item_size;
        return 0;
    }
    B_IN(2u);
    {
        /* 每次 recv 记一条 (序号, 任务 id, 栈指针) 到 RTC 0x50000800 起的环，
         * 先占位再写，避免被抢占后串字段。 */
        u32 sp_;
        u32 k_ = g_rc_n;
        u32 id_;
        volatile u32 *rc = (volatile u32 *)0x50000800u;
        __asm__ __volatile__("mov %0, a1" : "=r"(sp_));
        g_rc_n = k_ + 1u;
        id_ = s3_w2c_current_task();
        rc[(k_ & 7u) * 3u] = k_;
        rc[(k_ & 7u) * 3u + 1u] = id_;
        rc[(k_ & 7u) * 3u + 2u] = sp_;
        rc[24] = g_rc_n;
        /* 关键对比：port 认为的当前 ctx（g_s3_cur[0]）vs 内核认为的当前任务 id */
        rc[(k_ & 7u) * 3u + 3u] = 0u;   /* 占位避免越界踩到下一项 */
        rc[(k_ & 7u)] = k_;
        rc[(k_ & 7u) + 8u] = (u32)(void *)g_s3_cur[0];
        rc[(k_ & 7u) + 16u] = id_;
    }
    r = s3_w2c_queue_get((void *)&q->q, item, w_ticks(ticks));
    {
        volatile u32 *ev = (volatile u32 *)0x50000400u;
        ev[g_ev_n & 7u] = (s3_w2c_current_task() << 8) | 2u;
        g_ev_n = g_ev_n + 1u;
    }
    B_OUT(2u);
    if (r != 0) {
        QDIAG[1] = (u32)(-r);               /* 1=没有当前任务 3=超时表满 … */
        QDIAG[2] = ticks;
        QDIAG[3] = q->q.count;
    }
    return (r == 0) ? 1 : 0;
}
static u32 osi_queue_msg_waiting(void *qh)
{
    WQ *q = (WQ *)qh;
    if (q == 0 || q->magic != WQ_MAGIC) { return 0u; }
    return s3_w2c_queue_count((void *)&q->q);
}

/* ---- 任务 ---- */
static int osi_task_create(void *fn, const char *name, u32 stack, void *arg,
                           u32 prio, void *handle)
{
    int id;
    u32 p = (prio > 31u) ? 0u : (31u - prio);   /* IDF 越大越高，我们越小越高 */
    HIT_OF(_task_create);
    id = s3_w2c_task_create(name, fn, arg, p, stack);
    /* 诊断：库传进来的入口函数、参数、栈、返回的任务 id，以及调用者 id */
    {
        volatile u32 *td = (volatile u32 *)0x500003c0u;
        td[0] = (u32)fn;
        td[1] = (u32)arg;
        td[2] = stack;
        td[3] = (u32)id;
        td[4] = s3_w2c_current_task();
        td[5] = prio;
    }
    if (id < 0) { return 0; }
    if (handle != 0) { *(u32 *)handle = (u32)id; }
    return 1;
}
static void osi_task_delete(void *h) { (void)s3_w2c_task_delete((u32)h); }
static u32 osi_task_get_current_task(void) { return s3_w2c_current_task(); }
static u32 osi_task_ms_to_tick(u32 ms)
{
    u32 hz = s3_w2c_tick_hz();
    if (hz == 0u) { return ms; }
    return (ms * hz) / 1000u;
}
static u32 osi_task_get_max_priority(void) { HIT_OF(_task_get_max_priority); return 31u; }
static void osi_task_yield_from_isr(void) { s3_w2c_task_yield(); }
static void osi_task_delay(u32 ticks) { s3_w2c_task_delay(ticks); }

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
    u32 hz;
    u64 t;
    HIT_OF(_esp_timer_get_time);
    hz = s3_w2c_tick_hz();          /* 时间也必须走 w2c 壳：内核是 call0 */
    t  = s3_w2c_tick_get();
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
