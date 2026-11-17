/* wifi_glue.c —— WiFi 闭源库的 OS 适配层（第一步：先能链接 + 能返回）。
 *
 * 现状：所有 119 个成员先指向同一个"返回 0"的桩，目的是让 esp_wifi_init()
 * 能链接通过；之后用 USB-JTAG（tools/dbg.sh）在 esp_wifi_init 里断点/单步，
 * 按崩溃顺序逐个换成真实现。优先级：
 *   内存 -> 队列 -> 信号量/互斥 -> 定时器 -> 线程 -> 中断 -> 日志/事件
 */
#include "c6os/types.h"

#include <stdarg.h>

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

/* port/s3/w2c.S 的 osi_queue_recv 壳用它做 4 槽环的下标。
 * 必须和函数体里的 g_recv_seq 分开：共用一个序号时两边的写入互相覆盖，
 * 真板上表现为"两次 recv 的库侧返回地址一模一样"，把 task1 的那条冲掉了。 */
u32 g_lra_seq = 0u;

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
    /* §297：堆耗尽——库很可能没检查返回值就直接解引用（真板上表现为
     * cause=0x1d、EXCVADDR=NULL+偏移）。这里记次数和最近一次的大小，并
     * 在第一次失败时打一行，方便判断要不要扩堆。 */
    *(volatile u32 *)0x50000748u = *(volatile u32 *)0x50000748u + 1u;
    *(volatile u32 *)0x5000074cu = n;
    if (*(volatile u32 *)0x50000748u == 1u) {
        extern int ets_printf(const char *fmt, ...);
        ets_printf("[heap] 分配失败 n=%u\n", (unsigned)n);
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
extern int  s3_w2c_queue_try_get(void *q, void *out);
extern int  s3_w2c_queue_try_put(void *q, const void *item);
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
extern int  s3_w2c_task_suspend(u32 id);
extern u32 g_next_task_windowed;
extern u32 s3_w2c_slot_of_id(u32 id);
extern int  s3_w2c_task_resume(u32 id);
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
/* 初始化任务的 id：它是唯一有权"不阻塞"的任务。
 * 实测（§120）单线程配置下 esp_wifi_init_internal 能返回 ESP_OK，
 * 而真 RTOS 配置下它会在 pp_create_task 内部与 WiFi 任务互等。
 * 这里先让初始化任务的所有阻塞调用变成"立即返回成功"，
 * 验证这是否足以让 init 走完（WiFi 任务照常阻塞等下一条消息）。 */
static u32 g_init_task = 0u;

static u32 g_nb_n = 0u;
static u32 s3_init_noblock(void)
{
    u32 me = s3_w2c_current_task();
    u32 hit = (g_init_task != 0u && me == g_init_task) ? 1u : 0u;
    if (g_nb_n < 8u) {
        volatile u32 *nd = (volatile u32 *)0x50001410u;
        u32 k = g_nb_n;
        g_nb_n = k + 1u;
        nd[k * 2u] = me;
        nd[k * 2u + 1u] = hit;
        nd[16] = g_init_task;
    }
    return hit;
}
#define INIT_NOBLOCK() (s3_init_noblock() != 0u)

static u32 w_ticks(u32 t)
{
    return (t == C6_FOREVER) ? 0u : t;
}

/* ---- 信号量 ---- */
/* 同步点追踪（§229）：只在 bringup 期间打开，打印信号量的建/给/取，
 * 用来定位"init 卡在等哪个同步对象"。 */
u32 g_dbg_sync = 0u;
static u32 g_sem_seq = 0u;
static u32 g_sem_id_of(void *p) { (void)p; return 0u; }

typedef struct { WOsSem s; u32 magic; u32 seq; } WSem;
#define WSEM_MAGIC 0x53454D31u

static WSem *wsem_new(u32 max, u32 init)
{
    WSem *s = (WSem *)osi_malloc((u32)sizeof(WSem));
    if (s == 0) { return 0; }
    if (s3_w2c_sem_init((void *)&s->s, init, max) != 0) { osi_free(s); return 0; }
    s->magic = WSEM_MAGIC;
    g_sem_seq = g_sem_seq + 1u;
    s->seq = g_sem_seq;
    if (g_dbg_sync != 0u) {
        ets_printf("[semnew] #%u p=%08x max=%u init=%u t=%u\n",
                   (unsigned)s->seq, (unsigned)(void *)s, (unsigned)max, (unsigned)init,
                   (unsigned)s3_w2c_current_task());
    }
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
/* 信号量操作环形追踪（RTC 0x50001C00，每项 5 字：
 * 序号 / 操作(1=take 2=give) / 对象 / ticks / 结果）。
 * 用来定位"谁在等、谁没给"。 */
static u32 g_sem_n = 0u;
static void sem_trace(u32 op, u32 obj, u32 ticks, u32 r)
{
    /* 诊断环暂时停用（§243）：它本身在真板上引发了 LoadProhibited
     * （inlined 之后 l32i.n a8, a6, 0 里 a6 是 0）。诊断不该主导故障，
     * 先把它变成空函数，等主流程通了再恢复。 */
    (void)op; (void)obj; (void)ticks; (void)r;
}
static int osi_semphr_take(void *sem, u32 ticks)
{
    WSem *s = (WSem *)sem;

    if (g_dbg_sync != 0u) {
        ets_printf("[take] t=%u sem#%u tk=%x\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)((s != 0) ? s->seq : 0u), (unsigned)ticks);
    }
    int ok;
    HIT_OF(_semphr_take);
    if (s == 0 || s->magic != WSEM_MAGIC) { sem_trace(1u, (u32)sem, ticks, 0xEEu); return 0; }
    B_IN(1u);
    ok = (s3_w2c_sem_pend((void *)&s->s, w_ticks(ticks)) == 0) ? 1 : 0;
    B_OUT(1u);
    sem_trace(1u, (u32)sem, ticks, (u32)ok);
    return ok;
}
static int osi_semphr_give(void *sem)
{
    WSem *s = (WSem *)sem;
    int r;
    HIT_OF(_semphr_give);
    if (g_dbg_sync != 0u) {
        ets_printf("[give] t=%u sem#%u\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)((s != 0) ? s->seq : 0u));
    }
    if (s == 0 || s->magic != WSEM_MAGIC) { sem_trace(2u, (u32)sem, 0u, 0xEEu); return 0; }
    r = (s3_w2c_sem_post((void *)&s->s) == 0) ? 1 : 0;
    sem_trace(2u, (u32)sem, 0u, (u32)r);
    return r;
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

static u32 g_mtx_seq = 0u;

static void *osi_mutex_create(void)
{
    void *m = wmux_new();

    HIT_OF(_mutex_create);
    if (m != 0) {
        g_mtx_seq = g_mtx_seq + 1u;
        *(u32 *)((u8 *)m + 8u) = g_mtx_seq;   /* WMtx 的第 3 个字当序号 */
        if (g_dbg_sync != 0u) {
            ets_printf("[mtxnew] #%u p=%08x t=%u\n", (unsigned)g_mtx_seq,
                       (unsigned)m, (unsigned)s3_w2c_current_task());
        }
    }
    return m;
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
    if (g_dbg_sync != 0u) {
        ets_printf("[lock] t=%u m#%u p=%08x\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)*(u32 *)((u8 *)m + 8u), (unsigned)(void *)m);
    }
    {
        int ok;
        B_IN(4u);
        int pr = s3_w2c_mutex_pend((void *)&m->m, 0u);

        ok = (pr == 0) ? 1 : 0;
        B_OUT(4u);
        if (g_dbg_sync != 0u) {
            ets_printf("[lockok] t=%u pr=%d ok=%d\n", (unsigned)s3_w2c_current_task(),
                       pr, ok);
        }
        return ok;
    }
}
static int osi_mutex_unlock(void *p)
{
    WMtx *m = (WMtx *)p;
    HIT_OF(_mutex_unlock);
    if (m == 0 || m->magic != WMTX_MAGIC) { return 0; }
    if (g_dbg_sync != 0u) {
        ets_printf("[unlock] t=%u m#%u p=%08x\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)*(u32 *)((u8 *)m + 8u), (unsigned)(void *)m);
    }
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

    ets_printf("[evwait] t=%u g=%x bits=%x all=%d tk=%x\n",
               (unsigned)s3_w2c_current_task(), (unsigned)(void *)g,
               (unsigned)bits, wait_all, (unsigned)ticks);
    u32 got;
    HIT_OF(_event_group_wait_bits);
    if (e == 0 || e->magic != WEVT_MAGIC) { return 0u; }
    B_IN(3u);
    if (0 && INIT_NOBLOCK()) { return 0u; }
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
static u32 g_fr_n = 0u;
static u32 g_gr_n = 0u;
volatile u32 g_recv_seq = 0u;
u32 g_wifi_noblock = 0u;   /* 实验开关：默认关闭 */
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
__attribute__((section(".flash.text")))
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
    if (g_dbg_sync != 0u) {
        ets_printf("[send] t=%u qh=%x tk=%x\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)qh, (unsigned)ticks);
    }
    if (q == 0 || q->magic != WQ_MAGIC) {
        SDIAG[0] = (u32)qh;                 /* 句柄不像队列 */
        SDIAG[1] = (u32)((WQ *)qh)->magic;
        SDIAG[2] = (u32)((WQ *)qh)->q.buf;
        SDIAG[3] = 0xBAD00000u;
        return 0;
    }
    if (ticks == 0u) {
        r = s3_w2c_queue_try_put((void *)&q->q, item);
    } else {
        r = s3_w2c_queue_put((void *)&q->q, item, w_ticks(ticks));
    }
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

/* 真正的 OSI 回调由 port/s3/w2c.S 的汇编壳提供（它会在入口记录库的返回地址），
 * 这里只声明；C 实现改名为 _body。 */
int osi_queue_recv(void *qh, void *item, u32 ticks);

extern void *port_cur_ctx(void);

int osi_queue_recv_body(void *qh, void *item, u32 ticks)
{
    {
        /* 诊断：记录"是谁"在调 _queue_recv —— 用 port 视角的当前上下文，
         * 这是唯一能区分初始化任务与 WiFi 任务的可靠标识。 */
        volatile u32 *rr = (volatile u32 *)0x50001540u;
        u32 k_ = g_recv_seq;
        g_recv_seq = k_ + 1u;
        /* 注意：这里必须用 windowed 的垫片，不能直接调 ctx.c 的 port_cur_ctx()。
         * ctx.c 是 -mabi=call0 编译的，被 windowed 代码用 call8 调时，
         * 它的 `ret` 会跳到**带 CALLINC 标记**的返回地址上 ->
         * InstrFetchProhibited（真板症状：pc==vaddr==a0==0x82xxxxxx）。 */
        rr[(k_ & 3u) * 3u]      = s3_w2c_current_task();
        rr[(k_ & 3u) * 3u + 1u] = (u32)qh;
        rr[(k_ & 3u) * 3u + 2u] = ticks;
    }
    WQ *q = wq_resolve(qh);
    int r;
    HIT_OF(_queue_recv);
    /* 假设验证：让 WiFi 任务不在深窗口调用链上阻塞。
     * 若崩溃（InstrFetchProhibited @ osi_queue_recv+N）因此消失，
     * 就印证"切走 windowed 任务时祖先窗口丢失"这个根因。 */
    if (g_wifi_noblock != 0u) { return 0; }

    /* 试过让初始化任务不阻塞（INIT_NOBLOCK）：recv 直接返回成功反而让
     * 调用方自旋（ISR 采样到的 PC 一直是 osi_queue_recv 内部），
     * 所以初始化任务也必须正常阻塞。保留这个宏仅供诊断开关使用。 */
    if (0 && INIT_NOBLOCK()) { return 0; }
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
        /* 疑点：ppTask 可能在 xphyQueue 尚未写入时就调 _queue_recv。
         * xphyQueue 是 ROM 里的全局（0x3fcef904），记下每次 recv 时的值。 */
        rc[25] = *(volatile u32 *)0x3fcef904u;
        rc[26] = (u32)qh;
        rc[27] = (u32)q;
        /* 独立区域：从窗口帧里取调用者返回地址。
         * entry 把调用者的 a0..a3 存在帧的高端（framesize-16 ..+0），
         * 这里把 framesize=32 的几种可能位置都记下来，事后对照哪份是代码地址。 */
        {
            /* 从自己的窗口帧里取调用者返回地址：
             * osi_queue_recv 是 `entry a1, 32`，entry 把调用者 a0..a3 存在
             * [a1+16..a1+28]。这里把 a1 与那几个字都记下来核对。 */
            u32 a1v = 0u;
            volatile u32 *fr = (volatile u32 *)0x50000D00u;
            u32 k2 = g_fr_n & 3u;
            g_fr_n = g_fr_n + 1u;
            __asm__ __volatile__("mov %0, a1" : "=r"(a1v));
            fr[k2 * 5u] = a1v;
            if (a1v >= 0x3FC80000u && a1v < 0x3FD00000u) {
                fr[k2 * 5u + 1u] = *(volatile u32 *)(a1v + 16u);
                fr[k2 * 5u + 2u] = *(volatile u32 *)(a1v + 20u);
                fr[k2 * 5u + 3u] = *(volatile u32 *)(a1v + 24u);
                fr[k2 * 5u + 4u] = id_;
            }
        }
    }
    /* FreeRTOS 语义：ticks==0 = 非阻塞；tick==portMAX_DELAY = 永久等待。
     * 我们内核里 timeout==0 表示永久等待，所以 0 必须走 try 版本，
     * 否则库里的"非阻塞收"会变成永久挂起（实测就是任务再也不动）。 */
    /* 关键诊断：在**阻塞发生的那一刻**记下是谁、收哪个队列、等多久。
     * RTC 环在真板上会互相覆盖、read-mem 又会先复位，只有这里直接打印
     * 才可靠（一次启动只有两三次）。 */
    {
        /* §320 实验：这条打印原来走 ets_printf + s3_w2c_current_task()（一个
         * windowed 壳）。真板上崩溃点正好是 ROM printf 内部那个 `retw.n`，
         * 所以先把它换成"只写 RTC、不调任何函数"的记法，看崩溃是否跟着消失。 */
        static u32 recv_n_;
        if (recv_n_ < 3u) {
            volatile u32 *d_ = (volatile u32 *)0x50000600u;
            u32 k_ = recv_n_ * 4u;
            d_[k_ + 0u] = (u32)(void *)qh;
            d_[k_ + 1u] = (u32)ticks;
            d_[k_ + 2u] = 0x52454356u;   /* 'RECV' */
            recv_n_ = recv_n_ + 1u;
            d_[12] = recv_n_;
        }
    }
    if (ticks == 0u) {
        r = s3_w2c_queue_try_get((void *)&q->q, item);
    } else {
        r = s3_w2c_queue_get((void *)&q->q, item, w_ticks(ticks));
    }
    {
        /* 诊断：每次收到的返回值 + 当前队列 count，判断第一次是"真收到"
         * 还是我们错误地返回了成功。 */
        volatile u32 *gd = (volatile u32 *)0x50001300u;
        u32 kk = g_gr_n;
        g_gr_n = kk + 1u;
        if (kk < 4u) {
            gd[kk * 4u]      = (u32)(int)r;
            gd[kk * 4u + 1u] = q->q.count;
            gd[kk * 4u + 2u] = s3_w2c_current_task();
            gd[kk * 4u + 3u] = *(volatile u32 *)0x3fcef904u;
        }
    }
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
/* ---- 隔离测试：只走"库调用 _queue_recv"这一条路径 ----------------------
 * 用和库完全一样的方式建队列（osi_wifi_create_queue），再经 osi_queue_recv
 * 垫片（w2c.S，windowed）→ callx8 osi_queue_recv_body → 内核队列 收一次。
 * 这样可以在完全不启动 WiFi 的情况下判断这条路径本身是否有问题。
 */
int osi_queue_recv(void *qh, void *item, u32 ticks);
__attribute__((section(".flash.text"), noinline))
int s3_wifi_test_recv(u32 rounds)
{
    u32 qh = osi_wifi_create_queue(4u, 4u);
    u32 item = 0u;
    u32 i;
    int ok = 0;

    if (qh == 0u) { return -1; }
    for (i = 0u; i < rounds; i = i + 1u) {
        if (osi_queue_recv((void *)qh, (void *)&item, 0u) != 0) { ok = ok + 1; }
    }
    return ok;
}

__attribute__((section(".flash.text")))
static u32 osi_queue_msg_waiting(void *qh)
{
    WQ *q = (WQ *)qh;
    if (q == 0 || q->magic != WQ_MAGIC) { return 0u; }
    return s3_w2c_queue_count((void *)&q->q);
}

/* ---- 任务 ---- */
static u32 g_wifi_task_ids[4];
static u32 g_wifi_task_n = 0u;

static int osi_task_create(void *fn, const char *name, u32 stack, void *arg,
                           u32 prio, void *handle)
{
    int id;
    u32 p = (prio > 31u) ? 0u : (31u - prio);   /* IDF 越大越高，我们越小越高 */
    HIT_OF(_task_create);
    /* 库请求的栈往往偏小（ppTask 只要 3584），而 WiFi 协议栈的调用链很深，
     * 所以要抬。但栈池只有 24KB（port/s3/ctx.c 的 S3_STACK_POOL，§219），
     * 抬到 8192 的话"bringup 任务 8KB + ppTask 8KB + lwIP tcpip 4KB"就快满了，
     * 所以这里取 6144：够深调用的余量，又给别的任务留位置。 */
    if (stack < 6144u) { stack = 6144u; }
    g_next_task_windowed = 1u;   /* 库的任务入口都是 windowed ABI */
#ifdef S3_NO_LIB_TASKS
    /* §366 排查：**不真的建库任务**，只给个假句柄。用来判定崩溃是
     * 发生在"库任务开始执行"之后，还是主流程自己继续走就会崩。 */
    id = 1;
    if (handle != 0) { *(u32 *)handle = 1u; }
    ets_printf("[task] 跳过库任务 fn=%08x\n", (unsigned)(u32)fn);
    return 1;
#endif
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
    ets_printf("[task] 库建任务 fn=%08x id=%d stack=%u prio=%u(->%u)\n",
               (unsigned)(u32)fn, id, (unsigned)stack, (unsigned)prio, (unsigned)p);
    if (id < 0) { return 0; }
    /* 句柄用稳定的表下标，和 _task_get_current_task() 同一口径 */
    if (handle != 0) { *(u32 *)handle = s3_w2c_slot_of_id((u32)id); }
    /* 试过"建完立即挂起、init 返回后再恢复"：不行 —— 挂起后
     * pp_create_task 里紧接着的 _semphr_take 握手永远等不到 _semphr_give，
     * init 会更早卡住。所以任务必须照常运行。 */
    if (g_wifi_task_n < 4u) { g_wifi_task_ids[g_wifi_task_n] = (u32)id; g_wifi_task_n = g_wifi_task_n + 1u; }
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
static void osi_task_delay(u32 ticks)
{
    if (g_dbg_sync != 0u) {
        ets_printf("[delay] t=%u ticks=%u\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)ticks);
    }
    s3_w2c_task_delay(ticks);
}

/* ---- PHY 使能/关闭（§228）------------------------------------------------
 * IDF 的 OSI 表把这两个槽位接到 esp_phy_enable/disable(PHY_MODEM_WIFI)，也就是
 * "WiFi 库自己在合适时机回调我们做 PHY 初始化"。我们之前把这槽位留空、改成自己
 * 在 bringup 之前手动调 register_chipv7_phy —— 实测那条路会在库里忙等死转
 * （卡在 set_chan_freq_hw_init 之后的 RF 等待里）。
 * 现在改回 IDF 的语义：库回调时再做。 */
extern int s3_win_phy_enable_hook(void);

static int osi_phy_enable(void)
{
    HIT_OF(_phy_enable);
    return (s3_win_phy_enable_hook() == 0) ? 0 : -1;
}

static int osi_phy_disable(void)
{
    HIT_OF(_phy_disable);
    return 0;
}

static void osi_wifi_rtc_enable_iso(void)
{
    volatile u32 *iso = (volatile u32 *)0x60008094u;   /* RTC_CNTL_DIG_ISO */

    HIT_OF(_wifi_rtc_enable_iso);
    *iso = *iso | (1u << 28);                           /* WIFI_FORCE_ISO */
}

static void osi_wifi_rtc_disable_iso(void)
{
    volatile u32 *iso = (volatile u32 *)0x60008094u;

    HIT_OF(_wifi_rtc_disable_iso);
    *iso = *iso & ~(1u << 28);
}


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

__attribute__((section(".flash.text")))
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

/* 真实 MAC：DHCP / WPA / 扫描都要用，返回全零 MAC 会让上层行为莫名其妙
 * （有些 AP 直接拒绝，DHCP 客户端身份也不合法）。IDF 的 esp_read_mac() 语义：
 * type 0 = STA/BASE、1 = AP、2 = BT，按最后 1 字节 +type 派生。
 * 这块板 efuse 里的 base MAC 是 cc:8d:a2:ec:99:c4（§192 从 IDF 启动日志抄下来的）。
 * TODO: 改成从 efuse 读，而不是写死。 */
static const u8 s3_base_mac[6] = { 0xccu, 0x8du, 0xa2u, 0xecu, 0x99u, 0xc4u };

static u32 osi_read_mac(u8 *mac, u32 type)
{
    u32 i;

    HIT_OF(_read_mac);
    if (mac == 0) { return 1u; }
    i = 0u;
    while (i < 6u) { mac[i] = s3_base_mac[i]; i = i + 1u; }
    mac[5] = (u8)(mac[5] + (u8)type);
    return 0u;
}

/* ---- NVS 槽位：**必须返回错误码，不能返回 0** -------------------------
 *
 * 这一条是静态统计（tools/osislots.py）挖出来的隐患：libnet80211 的
 * `wifi_nvs_load`、libcore 的 `misc_nvs_load`、libmesh 的 `esp_mesh_nvs_*`
 * 都会走 `_nvs_open` / `_nvs_get_blob` 这条链。写成"一律返回 0"的话：
 *   1) IDF 的 esp_err_t 里 0 就是 ESP_OK —— 库以为配置打开成功了；
 *   2) 接着它拿一个**没初始化过**的 handle 去 `_nvs_get_blob`，还是返回 0（成功），
 *      于是把调用方 buffer 里的**未初始化内容**当成"上次保存的校准数据/配置"用下去。
 * 正确做法是返回 nvs 的真实错误码，让库走"没存过 → 用默认值"的正常分支。 */
#define OSI_NVS_ERR_NOT_INITIALIZED 0x1101
#define OSI_NVS_ERR_NOT_FOUND       0x1102

static int osi_nvs_open(const char *name, u32 mode, u32 *out)
{
    HIT_OF(_nvs_open);
    (void)name; (void)mode;
    if (out != 0) { *out = 0u; }
    return OSI_NVS_ERR_NOT_INITIALIZED;
}
static void osi_nvs_close(u32 h) { HIT_OF(_nvs_close); (void)h; }
static int osi_nvs_get(const char *tag) { (void)tag; return OSI_NVS_ERR_NOT_FOUND; }
static int osi_nvs_put(const char *tag) { (void)tag; return OSI_NVS_ERR_NOT_INITIALIZED; }

#define OSI_NVS_STUB_GET(name, slot) \
    static int name(u32 h, const char *k, void *out) \
    { HIT_OF(slot); (void)h; (void)k; (void)out; return OSI_NVS_ERR_NOT_FOUND; }
#define OSI_NVS_STUB_SET(name, slot) \
    static int name(u32 h, const char *k, void *val) \
    { HIT_OF(slot); (void)h; (void)k; (void)val; return OSI_NVS_ERR_NOT_INITIALIZED; }

OSI_NVS_STUB_GET(osi_nvs_get_i8,   _nvs_get_i8)
OSI_NVS_STUB_GET(osi_nvs_get_u8,   _nvs_get_u8)
OSI_NVS_STUB_GET(osi_nvs_get_u16,  _nvs_get_u16)
OSI_NVS_STUB_SET(osi_nvs_set_i8,   _nvs_set_i8)
OSI_NVS_STUB_SET(osi_nvs_set_u8,   _nvs_set_u8)
OSI_NVS_STUB_SET(osi_nvs_set_u16,  _nvs_set_u16)

static int osi_nvs_get_blob(u32 h, const char *k, void *out, u32 *len)
{
    HIT_OF(_nvs_get_blob);
    (void)h; (void)k; (void)out;
    if (len != 0) { *len = 0u; }
    return OSI_NVS_ERR_NOT_FOUND;
}
static int osi_nvs_set_blob(u32 h, const char *k, const void *val, u32 len)
{
    HIT_OF(_nvs_set_blob);
    (void)h; (void)k; (void)val; (void)len;
    return OSI_NVS_ERR_NOT_INITIALIZED;
}
static int osi_nvs_commit(u32 h) { HIT_OF(_nvs_commit); (void)h; return OSI_NVS_ERR_NOT_INITIALIZED; }
static int osi_nvs_erase_key(u32 h, const char *k) { HIT_OF(_nvs_erase_key); (void)h; (void)k; return OSI_NVS_ERR_NOT_FOUND; }


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

/* ---- 中断相关槽位：先做成"记录 + 空实现"，用来判断库到底用不用（§216）----
 *
 * 静态统计（tools/osislots.py）显示 libnet80211/libpp/libcore **没有**直接读过
 * `_set_isr`/`_ints_on`/`_set_intr` 这些槽位，但为了确认（万一它们通过别的间接
 * 路径用到了），这里把它们从"纯 glue_ret0"换成会记账的实现：真上板时如果库确实
 * 调了，RTC 慢存里就有参数可查，我们据此再决定要不要接内核的中断表。
 *
 * RTC 慢存布局（和 §202 里其它信标错开）：
 *   0x50000B00  set_isr 环：8 项 × 2 字（中断号, 处理函数）
 *   0x50000B40  ints_on 次数 / 最后一次 mask
 *   0x50000B48  ints_off 次数 / 最后一次 mask
 *   0x50000B50  set_intr 环：4 项 × 4 字（cpu, source, num, prio）
 *   0x50000B90  event_post 环：16 项 × 2 字（event_base, event_id）+ 计数
 */
#define IRQ_RING_BASE  0x50000B00u
#define EVT_RING_BASE  0x50000B90u

static void rec2(u32 base, u32 idx, u32 a, u32 b)
{
    volatile u32 *p = (volatile u32 *)base;

    p[(idx & 7u) * 2u] = a;
    p[(idx & 7u) * 2u + 1u] = b;
    p[16] = p[16] + 1u;             /* 第 16 个字当计数器 */
}

static int osi_set_intr(u32 cpu_no, u32 source, u32 intr_num, u32 prio)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x50u);

    p[(p[16] & 3u) * 4u] = cpu_no;
    p[(p[16] & 3u) * 4u + 1u] = source;
    p[(p[16] & 3u) * 4u + 2u] = intr_num;
    p[(p[16] & 3u) * 4u + 3u] = prio;
    p[16] = p[16] + 1u;
    return 0;
}

static int osi_clear_intr(u32 source, u32 intr_num) { (void)source; (void)intr_num; return 0; }

static int osi_set_isr(int n, void *fn, void *arg)
{
    volatile u32 *p = (volatile u32 *)IRQ_RING_BASE;

    rec2(IRQ_RING_BASE, p[16], (u32)n, (u32)fn);
    (void)arg;
    return 0;
}

static int osi_ints_on(u32 mask)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x40u);

    p[0] = p[0] + 1u;               /* 次数 */
    p[1] = mask;                    /* 最后一次 mask */
    return 0;
}

static int osi_ints_off(u32 mask)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x48u);

    p[0] = p[0] + 1u;
    p[1] = mask;
    return 0;
}

/* 真的关中断（§226）：IDF 这两个槽位就是 portENTER_CRITICAL/portEXIT_CRITICAL。
 * 只记账的空实现会让库以为自己在临界区里，实际上并发完全没保护（队列/链表
 * 可能被 ISR 和任务同时改）。库在临界区里**不会阻塞**（IDF 下同样如此），
 * 所以用内核的 rsil/wsr ps 是安全的。 */
extern u32 s3_w2c_irq_save(void);
extern void s3_w2c_irq_restore(u32 state);

static u32 osi_wifi_int_disable(void)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x40u);

    p[2] = p[2] + 1u;
    return s3_w2c_irq_save();
}

static void osi_wifi_int_restore(u32 state)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x40u);

    p[3] = p[3] + 1u;
    s3_w2c_irq_restore(state);
}

static int osi_is_from_isr(void) { return 0; }

/* 事件：库通过它上报 WIFI_EVENT/IP_EVENT。我们没接事件系统，但**记下来**：
 * 上板时 RTC 里能看到 event_base/event_id，就知道关联到哪一步了。 */
static int osi_event_post(const char *base, int id, void *data, u32 size, u32 ticks)
{
    volatile u32 *p = (volatile u32 *)EVT_RING_BASE;
    u32 n = p[32];

    (void)data; (void)size; (void)ticks;
    p[(n & 15u) * 2u] = (u32)(void *)base;
    p[(n & 15u) * 2u + 1u] = (u32)id;
    p[32] = n + 1u;
    return 0;
}

/* ---- 表装配：用 __builtin_offsetof，彻底避免手数下标 ---- */
#define OSI_SET(slot, fn) \
    (((void **)(void *)&g_wifi_osi_funcs)[__builtin_offsetof(wifi_osi_funcs_t, slot) / 4u] \
        = (void *)(fn))

/* 编译期断言：_magic 必须在偏移 0x1DC（库硬读这个位置） */
typedef char s3_osi_magic_offset_check[
    (__builtin_offsetof(wifi_osi_funcs_t, _magic) == 0x1DCu) ? 1 : -1];
typedef char s3_osi_size_check[(sizeof(wifi_osi_funcs_t) == 480u) ? 1 : -1];

/* windowed 世界的窗口 spill 开关（定义在 port/s3/ctx.c，w2c.S 的壳读它）。
 * 闭源库的回调马上要跑起来了，从这里开始每条 w2c 壳都必须把祖先窗口 spill
 * 干净，否则库里一阻塞我们就没法把它恢复回来（§193/§197）。 */
extern u32 g_s3_win_spill_en;

__attribute__((section(".flash.text")))
void s3_wifi_osi_init(void)
{
    void **tab = (void **)(void *)&g_wifi_osi_funcs;
    u32 n = (u32)(sizeof(wifi_osi_funcs_t) / 4u);
    u32 i = 0;

    while (i < n) { tab[i] = (void *)glue_ret0; i = i + 1u; }
#ifdef S3_NO_WIN_SPILL
    g_s3_win_spill_en = 0u;    /* 对照实验：不 spill 祖先窗口（§230） */
#else
    g_s3_win_spill_en = 1u;
#endif

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
    OSI_SET(_is_from_isr,   osi_is_from_isr);
    /* 中断相关：记账 + 空实现（§216，判断库到底用不用） */
    OSI_SET(_set_intr,        osi_set_intr);
    OSI_SET(_clear_intr,      osi_clear_intr);
    OSI_SET(_set_isr,         osi_set_isr);
    OSI_SET(_ints_on,         osi_ints_on);
    OSI_SET(_ints_off,        osi_ints_off);
    OSI_SET(_wifi_int_disable, osi_wifi_int_disable);
    OSI_SET(_wifi_int_restore, osi_wifi_int_restore);
    OSI_SET(_event_post,      osi_event_post);
    OSI_SET(_task_delay,      osi_task_delay);
    OSI_SET(_phy_enable,      osi_phy_enable);
    OSI_SET(_phy_disable,     osi_phy_disable);
    OSI_SET(_wifi_rtc_enable_iso,  osi_wifi_rtc_enable_iso);
    OSI_SET(_wifi_rtc_disable_iso, osi_wifi_rtc_disable_iso);

    /* ---- NVS：必须返回错误码，别让库把未初始化内存当有效数据（见上面的说明）---- */
    OSI_SET(_nvs_open,        osi_nvs_open);
    OSI_SET(_nvs_close,       osi_nvs_close);
    OSI_SET(_nvs_commit,      osi_nvs_commit);
    OSI_SET(_nvs_erase_key,   osi_nvs_erase_key);
    OSI_SET(_nvs_set_i8,      osi_nvs_set_i8);
    OSI_SET(_nvs_get_i8,      osi_nvs_get_i8);
    OSI_SET(_nvs_set_u8,      osi_nvs_set_u8);
    OSI_SET(_nvs_get_u8,      osi_nvs_get_u8);
    OSI_SET(_nvs_set_u16,     osi_nvs_set_u16);
    OSI_SET(_nvs_get_u16,     osi_nvs_get_u16);
    OSI_SET(_nvs_set_blob,    osi_nvs_set_blob);
    OSI_SET(_nvs_get_blob,    osi_nvs_get_blob);
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

/* ---- 事件环：库通过 _event_post 上报的 WiFi/IP 事件（§216 记的账）-------
 * 打印 RTC 慢存 0x50000B90 起的 16 项（event_base, event_id）。
 * 常用 event_id（IDF 的 esp_wifi_types）：
 *   1=SCAN_DONE 2=STA_START 4=STA_CONNECTED 5=STA_DISCONNECTED 6=AUTHMODE_CHANGE
 * 有了这一行，关联卡在哪一步就一目了然（比"关联超时"四个字强多了）。 */
extern const char *WIFI_EVENT;      /* 定义在文件后面（"WIFI_EVENT" 的指针） */

void s3_wifi_dump_events(const char *tag)
{
    volatile u32 *p = (volatile u32 *)0x50000B90u;
    u32 n = p[32];
    u32 i = 0u;
    u32 show = (n > 8u) ? 8u : n;

    ets_printf("[evt] %s: 共 %u 条", tag, (unsigned)n);
    for (i = 0u; i < show; i++) {
        u32 base = p[i * 2u];
        u32 id = p[i * 2u + 1u];
        const char *kind = "?";

        if (base == (u32)(void *)WIFI_EVENT) {
            kind = "wifi";
        } else if (base != 0u) {
            kind = "ip/other";
        }
        ets_printf(" [%s:%u]", kind, (unsigned)id);
    }
    ets_printf("\n");
}

/* ---- OSI 调用统计：每个阶段结束打一遍，方便判断"库到底走到哪一步" --------
 * g_osi_calls[i] 是第 i 个 OSI 槽位被调用的次数。板子上没有断点，靠这个就能看出
 * 例如 "_semphr_create 有、_queue_recv 没有" 这种早期失败（§223）。 */
void s3_wifi_dump_osi(const char *tag)
{
    volatile u32 *c = (volatile u32 *)g_osi_calls;
    u32 i = 0u;
    u32 printed = 0u;

    ets_printf("[osi] %s:", tag);
    for (i = 0u; i < 128u; i++) {
        if (c[i] != 0u) {
            ets_printf(" %u=%u", (unsigned)i, (unsigned)c[i]);
            printed = printed + 1u;
            if (printed >= 16u) {
                break;              /* 一行最多 16 项，别把串口刷爆 */
            }
        }
    }
    if (printed == 0u) {
        ets_printf(" (还没有任何回调)");
    }
    ets_printf("\n");
}

/* ---- 剩下几个"库会调但我们还没实现"的槽位（§225）----------------------
 *
 * 逐个说明（对照 tools/osislots.py 的静态统计）：
 *   _task_delay            IDF 映射到 vTaskDelay —— 必须真延迟，否则库里的
 *                          "等一会儿再重试"变成空转（我们的调度是抢占的，所以
 *                          不会彻底卡死，但时序完全不对）。
 *   _wifi_rtc_enable_iso / _disable_iso
 *                          IDF 就是置/清 RTC_CNTL_DIG_ISO 的 WIFI_FORCE_ISO(bit28)，
 *                          照抄即可（我们已关省电，但库在 hw_start/hw_stop 会调）。
 *   其余（_phy_*、_timer_*、_wifi_clock_*、_coex_*、apb80m、dport_stall）
 *                          静态统计显示闭源库**从不直接调用**；coex 那几个即使被
 *                          调用，返回 0 也表示"没有 BT 活动/无共存需求"，是正确
 *                          答案（我们的 BT 是关的）。所以继续保持空实现。
 */
/* ---- 关联（associate）：init 之后把 STA 真正连上 AP ---------------------
 *
 * 之前只做了 esp_wifi_init_internal()，**没有** set_mode/set_config/start/connect，
 * 所以就算 PHY 起来了也永远不会关联。这里按 IDF 的顺序补齐：
 *   esp_wifi_set_storage(RAM)   —— 先关掉 NVS 存储（我们没接 NVS 子系统）
 *   esp_wifi_set_mode(STA)
 *   esp_wifi_set_config(STA, cfg) —— cfg 是 192 字节全零缓冲，只填 ssid/password
 *                                   （wifi_sta_config_t 的这两个字段在最前面，
 *                                     偏移 0 / 32，跨 IDF 版本都稳定）
 *   esp_wifi_start()
 *   esp_wifi_connect_internal()
 *
 * 事件系统我们没接（OSI 的 _event_post 是空实现），所以"连上没有"靠轮询
 * esp_wifi_sta_get_ap_info()，"拿到 IP 没有"靠轮询 lwIP 的 netif（见
 * c6_lwip_port.c 的 c6_lwip_wait_ip）。
 */
/* wpa_crypto.c 总是会链进来（wifi 构建和基线构建都有），不受 S3_WITH_WIFI 影响 */
extern void c6_wpa_crypto_init(void);
extern const void *c6_wpa_crypto_table(void);

#ifdef S3_WITH_WIFI
extern int esp_wifi_set_storage(int storage);
extern int esp_wifi_set_mode(int mode);
extern int esp_wifi_set_config(int iface, void *conf);
extern int esp_wifi_start(void);
extern int esp_wifi_connect_internal(void);
extern int esp_wifi_sta_get_ap_info(void *conf);
extern int esp_wifi_set_ps(int type);
extern int esp_wifi_scan_start(const void *cfg, int block);
extern int esp_wifi_scan_get_ap_num(unsigned short *num);
extern int esp_wifi_scan_get_ap_records(unsigned short *num, void *records);
int s3_wifi_scan(void);         /* 定义在下面（先扫一遍做射频体检） */
#endif

#ifdef S3_WITH_WIFI
u32 s3_wifi_associate(const char *ssid, const char *pass)
{
    /* 320 字节全零：wifi_sta_config_t 现在约 184 字节，但不同 IDF 版本会变长。
     * 库是按**它自己编译时的 sizeof** 读这块内存的，缓冲给窄了它就会读到我们
     * 后面的 bss 垃圾（比如 bssid_set 变成 1、channel 变成随便一个值），
     * 表现就是"配置明明对却连不上"。多给一些零是最便宜的保险（§221）。 */
    static u32 cfg[80];
    u32 i = 0u;
    int r1 = 0;
    int r2 = 0;
    int r3 = 0;
    int r4 = 0;
    int r5 = 0;
    int n = 0;

    for (i = 0u; i < 80u; i++) { cfg[i] = 0u; }
    if (ssid == 0 || ssid[0] == 0) {
        /* 没给账号：仍然把接口拉起来扫一遍，至少验证射频与收发链路 */
        ets_printf("[wifi] 没给 SSID：只做 set_mode/start/scan（射频体检）\n");
        (void)esp_wifi_set_storage(1);
        (void)esp_wifi_set_mode(1);
        ets_printf("[wifi] start=%d\n", esp_wifi_start());
        (void)s3_wifi_scan();
        return 0xFFFFFFFFu;
    }
    while (ssid[n] != 0 && n < 32) { ((u8 *)cfg)[n] = (u8)ssid[n]; n++; }
    if (pass != 0) {
        n = 0;
        while (pass[n] != 0 && n < 64) { ((u8 *)cfg)[32 + n] = (u8)pass[n]; n++; }
    }
    ets_printf("[wifi] associate ssid=%s\n", ssid);
    /* wifi_storage_t: 0 = WIFI_STORAGE_FLASH, 1 = WIFI_STORAGE_RAM */
    r1 = esp_wifi_set_storage(1);
    ets_printf("[wifi] set_storage(RAM)=%d\n", r1);
    /* wifi_mode_t: 1 = WIFI_MODE_STA */
    r2 = esp_wifi_set_mode(1);
    ets_printf("[wifi] set_mode(STA)=%d\n", r2);
    /* wifi_interface_t: 0 = WIFI_IF_STA */
    r3 = esp_wifi_set_config(0, (void *)cfg);
    ets_printf("[wifi] set_config=%d\n", r3);
    r4 = esp_wifi_start();
    ets_printf("[wifi] start=%d\n", r4);
    /* 关掉省电：modem sleep 需要 OSI 的 `_wifi_rtc_enable/disable_iso` 和定时器
     * （我们那两个是空实现），睡眠一开 RX 就可能收不到包。取一个页面不需要省电，
     * 所以显式 WIFI_PS_NONE(0)（§221）。 */
    ets_printf("[wifi] set_ps(NONE)=%d\n", esp_wifi_set_ps(0));
    /* 先扫一遍：不需要密码就能验证射频链路（也方便判断"连不上"是 AP 没扫到
     * 还是认证失败）。 */
    (void)s3_wifi_scan();
    r5 = esp_wifi_connect_internal();
    ets_printf("[wifi] connect=%d\n", r5);
    return (u32)r5;
}

/* 扫描：这一步是**不需要 WiFi 密码**就能做的射频体检 ——
 * 只要扫到 AP，就证明 PHY + MAC + 收发链路都活着（§215）。
 * 返回扫到的 AP 数；<0 表示失败。只打印第一个 AP 的 SSID/信道/RSSI
 * （wifi_ap_record_t 的结构体大小依赖 IDF 版本，只取第一个记录最稳）。 */
int s3_wifi_scan(void)
{
    /* wifi_scan_config_t 约 20~24 字节，给 64 字节零缓冲（同样避免库读到 bss 垃圾） */
    u8 cfg[64];
    u8 rec[256];                /* 只放一个 wifi_ap_record_t，足够大 */
    unsigned short num = 0;
    unsigned short one = 1;
    int r = 0;
    int i = 0;

    for (i = 0; i < 64; i++) { cfg[i] = 0; }
    for (i = 0; i < 256; i++) { rec[i] = 0; }
    ets_printf("[wifi] scan start（阻塞）\n");
    r = esp_wifi_scan_start((const void *)cfg, 1);
    ets_printf("[wifi] scan_start=%d\n", r);
    if (r != 0) {
        return -1;
    }
    r = esp_wifi_scan_get_ap_num(&num);
    ets_printf("[wifi] scan_get_ap_num=%d num=%u\n", r, (unsigned)num);
    if (r != 0 || num == 0) {
        return (int)num;
    }
    r = esp_wifi_scan_get_ap_records(&one, (void *)rec);
    if (r == 0) {
        /* 偏移按 IDF 的 wifi_ap_record_t：bssid[6]@0, ssid[33]@6,
         * primary(u8)@39, second@40, rssi(int8)@41 */
        ets_printf("[wifi] AP0 ssid=%.32s ch=%u rssi=%d\n",
                   (const char *)(rec + 6), (unsigned)rec[39], (int)(signed char)rec[41]);
    }
    return (int)num;
}

/* 轮询"连上 AP 了吗"：返回 1 = 已关联。ap_info 至少 64 字节（我们只用前几个字节）。 */
int s3_wifi_wait_connected(int ms)
{
    u8 ap[96];
    int waited = 0;
    int i = 0;
    int r = 0;

    while (waited < ms) {
        for (i = 0; i < 96; i++) { ap[i] = 0; }
        r = esp_wifi_sta_get_ap_info((void *)ap);
        if (r == 0) {
            ets_printf("[wifi] 已关联，ssid=%s channel=%u\n",
                       (const char *)ap, (unsigned)ap[40]);
            return 1;
        }
        s3_w2c_task_delay(1u);
        waited = waited + 10;
    }
    ets_printf("[wifi] 关联超时（%d ms），sta_get_ap_info=%d\n", ms, r);
    return 0;
}
#else
u32 s3_wifi_associate(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;
    return 0xFFFFFFFFu;         /* 没链 WiFi 库：跳过关联 */
}

int s3_wifi_wait_connected(int ms)
{
    (void)ms;
    return 0;
}
#endif

#ifdef S3_WIFI_ASSOC_GATE
/* 调试闸门（docs/wifi-plan.md §265）（docs/wifi-plan.md §265）。为什么需要它：
 * 这块板子的 USB-Serial-JTAG 和串口是同一个 USB —— esptool 一复位，OpenOCD 的
 * 连接就断，所以"先下断点再复位"走不通；而 OpenOCD 自己的 reset 起不来我们的
 * app（只到 port_idle）。唯一可行的是：esptool 真复位 → 立刻起 OpenOCD →
 * halt → 写 ibreaka0/ibreakenable → resume。但 OpenOCD 起来要 3~4 秒，等我们
 * 架好断点，app 早就跑过 associate 崩掉了。
 * 所以加一个**只在调试构建里存在**的闸门：app 在这里自旋等 JTAG 把
 * g_s3_dbg_gate 写 1。默认不编译（没有 S3_WIFI_ASSOC_GATE 时这段完全不存在，
 * 不会影响时序 —— 要知道这条路径对时序极其敏感，见 §259）。 */
volatile u32 g_s3_dbg_gate = 0u;
#endif

u32 s3_wifi_bringup(void)
{
    u32 i = 0u;
#ifdef S3_WIFI_ASSOC_GATE
    { extern volatile u32 g_s3_dbg_gate; while (g_s3_dbg_gate == 0u) { } }
#endif
    /* 库的 wifi_log 先看它自己的 g_log_level，默认级别会把错误信息过滤掉，
     * 调高之后 init 失败的原因才会通过 _log_writev 送到串口。 */
    { extern int g_log_level; g_log_level = 4; }
    /* §425 ★ 清 ROM 变量 `s_wifi_queue`（`rom/esp32s3.rom.ld` 里的**绝对
     * 地址**定义：0x3fcef918）。
     *
     * 这个地址不在 link.ld 的 DRAM 区域里（上界 0x3FCEF000），也**不在
     * `.bss`**，所以复位后**保留上一轮的值**。真板实测：复位后它还是上一轮
     * 的堆指针 0x3fcc45b0 ⇒ libpp 打出
     *   `create wifi task: queue not null`
     * 然后拿这个**已失效**的指针去 delete/复用 ⇒
     *   `TRAP cause=0x1c(StoreProhibited) vaddr=0x18 a4=0`
     * 清成 0 之后库会走"创建新队列"的正常路径。
     *
     * 注意：**不能一次性清整片 0x3FCEF800..0x3FCF0000**（试过，直接退化成
     * `TRAP pc=0x4004eabc`，ROM 自己的状态也在那 2KB 里）。只能一个一个来，
     * 只清我们确认需要的。 */
    { extern void *s_wifi_queue; s_wifi_queue = 0; }

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
    /* wpa_crypto_funcs：44 字节的**函数指针表**（§218）。以前这里只填了
     * size/version，九个指针全是 NULL —— WPA2-PSK 关联时要靠它做
     * "口令 → PSK"（PBKDF2-SHA1）和 MIC 计算，NULL 就是直接跑飞。
     * 表由 port/s3/wpa_crypto.c 提供（里面 PBKDF2-SHA1/SHA256/HMAC-SHA256
     * 是真实现，AES 那几项先记账+返回 -1）。 */
    c6_wpa_crypto_init();
    {
        const u32 *tab = (const u32 *)c6_wpa_crypto_table();
        u32 k = 0u;

        for (k = 0u; k < 11u; k++) {
            /* wcfg 收的是**字节偏移**，所以每项要 +4（之前写成 4u+k，把整张表
             * 挤进了 4..14 这 11 个字节里，库读到 size=0/version=0，
             * 直接返回 ESP_ERR_INVALID_ARG(258) —— 真板日志：
             *   wpa crypto funcs expected size=0 version=0, actual size=0 version=0 */
            wcfg(4u + k * 4u, tab[k]);
        }
    }
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

    g_dbg_sync = 1u;               /* 打开同步点追踪（§229）。注意：关掉它反而走得更短
                                    * ——串口打印带来的延迟会改变时序，见 §254 */
    s3_wifi_osi_init();
    ets_printf("B1 slot13=%08x slot18=%08x magic=%08x ver=%08x\n",
               (u32)(void *)g_wifi_osi_funcs._semphr_create,
               (u32)(void *)g_wifi_osi_funcs._mutex_create,
               (u32)(void *)g_wifi_osi_funcs._magic,
               (u32)(void *)g_wifi_osi_funcs._version);
    g_init_task = s3_w2c_current_task();
    *(volatile u32 *)0x50001400u = g_init_task;
    ets_printf("B2 cfg@%08x osi=%08x magic@144=%08x\n",
               (u32)(void *)g_wifi_cfg_blob,
               *(volatile u32 *)((u8 *)g_wifi_cfg_blob + 0u),
               *(volatile u32 *)((u8 *)g_wifi_cfg_blob + 144u));
    {
        u32 irc;
        /* §421 诊断：库的 init 状态机就在 `g_wifi_osi_funcs_md5 + 4`
         * （镜像初值 1 = 未初始化，2 = 正在初始化，3/4 = 其它）。
         * 如果进来时不是 1，说明之前有一次 init 没走完就返回了 ——
         * 那样这次必然拿到 0x3013。用符号而不是硬编码地址：加一行打印
         * 就会让 .data 布局整体挪位（§421 第一次就是踩了这个）。 */
        extern unsigned char g_wifi_osi_funcs_md5[];
        volatile u32 *wst = (volatile u32 *)(void *)(g_wifi_osi_funcs_md5 + 4);
        ets_printf("[wifi] init 前的库状态 @%08x = %u\n",
                   (unsigned)(u32)(void *)wst, (unsigned)*wst);
        /* §421 实验：我们手动做了 PHY enable，库的状态机残留成 2
         * （"正在初始化"），于是 init 直接返回 0x3013。复位成 1 再进去。 */
        if (*wst != 1u) { *wst = 1u; }
        irc = (u32)(int)esp_wifi_init_internal((const void *)g_wifi_cfg_blob);
        ets_printf("[wifi] init 后的库状态 @%08x = %u\n",
                   (unsigned)(u32)(void *)wst, (unsigned)*wst);

        ets_printf("[wifi] esp_wifi_init_internal -> %u\n", (unsigned)irc);
        g_dbg_sync = 0u;
        return irc;
    }
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

int esp_supplicant_init(void) { return 0; }
int esp_supplicant_deinit(void) { return 0; }

/* 我们 -nostdlib，newlib 的这几个函数自己给最小实现（free/puts 见文件末尾） */
void *malloc(u32 n);
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

/* IDF 的 wifi_init.c / mesh_event.c 里的薄封装（闭源库反调它们）。
 * 只有链接了闭源库的构建（S3_WITH_WIFI）才有 *_internal，普通构建里必须整段
 * 关掉，否则会对着不存在的符号报 undefined reference。 */
#ifdef S3_WITH_WIFI
extern int esp_wifi_connect_internal(void);
int esp_wifi_connect(void) { return esp_wifi_connect_internal(); }
extern int esp_wifi_disconnect_internal(void);
int esp_wifi_disconnect(void) { return esp_wifi_disconnect_internal(); }
/* mesh_event.c: ESP_EVENT_DEFINE_BASE(MESH_EVENT) + post。我们没有事件循环，直接返回。 */
const char *MESH_EVENT = "MESH_EVENT";
int esp_mesh_send_event_internal(int event_id, void *event_data, u32 event_data_size)
{
    (void)event_id; (void)event_data; (void)event_data_size;
    return 0;
}
#endif

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

/* ---- 诊断：用 --wrap 给 wifi_init_in_caller_task 走到的库函数各包一层 ----
 * 每个包装函数把自己的调用计数写进 RTC（BSS 计数，RTC 只做副本，
 * 因为 RTC 初值是随机的）。一次上板就能看出初始化到底走到哪个函数。 */
static u32 g_wrap_n[8];

static void s3_wrap_bump(u32 i)
{
    volatile u32 *d = (volatile u32 *)0x50000E00u;
    if (i < 8u) {
        g_wrap_n[i] = g_wrap_n[i] + 1u;
        d[i] = g_wrap_n[i];
    }
}

extern int __real_wifi_menuconfig_init(void *cfg);
int __wrap_wifi_menuconfig_init(void *cfg)
{
    s3_wrap_bump(0u);
    return __real_wifi_menuconfig_init(cfg);
}

extern int __real_misc_nvs_init(void);
int __wrap_misc_nvs_init(void)
{
    s3_wrap_bump(1u);
    return __real_misc_nvs_init();
}

extern int __real_ic_create_wifi_task(void);
int __wrap_ic_create_wifi_task(void)
{
    s3_wrap_bump(2u);
    return __real_ic_create_wifi_task();
}

extern int __real_ieee80211_ioctl_init(void);
int __wrap_ieee80211_ioctl_init(void)
{
    s3_wrap_bump(3u);
    return __real_ieee80211_ioctl_init();
}

extern int __real_pp_create_task(void);
int __wrap_pp_create_task(void)
{
    s3_wrap_bump(4u);
    return __real_pp_create_task();
}

extern int __real_pp_post(unsigned int ppq, unsigned int data);
int __wrap_pp_post(unsigned int ppq, unsigned int data)
{
    s3_wrap_bump(7u);
    return __real_pp_post(ppq, data);
}

extern int __real_wifi_init_in_caller_task(void);
int __wrap_wifi_init_in_caller_task(void)
{
    int r;
    s3_wrap_bump(5u);
    r = __real_wifi_init_in_caller_task();
    s3_wrap_bump(6u);          /* 只有它返回了才会 +1 */
    return r;
}

/* ---- 闭源库的日志/工具函数出口（IDF 里由开源组件提供，我们没人提供）----
 *
 * IDF 的 `esp_wifi/src/lib_printf.c` 导出 7 个 `xxx_printf(fmt, ...)`，闭源 .a
 * 直接用它们打日志。此前我们只吞掉 net80211_printf；现在按 §202 把整套补齐，
 * 并且**真的打出来**（§192 的 `_log_write=0` 说明库自己的日志通道不走 OSI，
 * 这条 printf 才是唯一能看到库内部诊断的路）。
 *
 * 输出走 ROM 的 ets_printf（windowed，本文件也是 windowed，ABI 一致）。
 * 只需要支持很小一部分格式：%s %d %u %x %c %% 和宽度。够读 md5/自检失败。 */

extern int ets_printf(const char *fmt, ...);
extern void s3_win_log(const char *s);

static char *wp_utoa(char *end, u32 v, u32 base)
{
    const char *digits = "0123456789abcdef";
    char *p = end;
    do {
        *p = digits[v % base];
        p--;
        v = v / base;
    } while (v != 0u);
    return p + 1;
}

/* 极简格式化：%s %c %d %i %u %x %X %p %%，支持数字宽度与 '0' 填充、'l'/'z' 长度前缀。
 * 用标准 va_list（Xtensa 上它是结构体/数组，不是指针），所以必须靠 va_arg。 */
static int wp_vfmt(char *out, u32 cap, const char *fmt, va_list ap)
{
    u32 n = 0u;
    char tmp[16];

    while (*fmt != '\0' && n + 1u < cap) {
        if (*fmt != '%') { out[n] = *fmt; n++; fmt++; continue; }
        fmt++;
        if (*fmt == '\0') { break; }
        {
            u32 width = 0u;
            int zero = 0;
            char conv;

            if (*fmt == '0') { zero = 1; fmt++; }
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10u + (u32)(*fmt - '0'); fmt++; }
            while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') { fmt++; }
            conv = *fmt;
            fmt++;

            if (conv == 's') {
                const char *s = va_arg(ap, const char *);
                u32 put = 0u;
                if (s == 0) { s = "(null)"; }
                while (s[put] != '\0' && n + 1u < cap) { out[n] = s[put]; n++; put++; }
                while (put < width) { if (n + 1u < cap) { out[n] = ' '; n++; } put++; }
                continue;
            }
            if (conv == 'c') {
                out[n] = (char)va_arg(ap, int); n++;
                continue;
            }
            if (conv == 'p') {
                u32 v = (u32)va_arg(ap, void *);
                char *p;
                u32 len;
                tmp[15] = '\0';
                p = wp_utoa(&tmp[14], v, 16u);
                len = (u32)(&tmp[15] - p);
                while (len < 8u) { p--; *p = '0'; len++; }
                if (n + 1u < cap) { out[n] = '0'; n++; }
                if (n + 1u < cap) { out[n] = 'x'; n++; }
                while (*p != '\0' && n + 1u < cap) { out[n] = *p; n++; p++; }
                continue;
            }
            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
                u32 v = va_arg(ap, u32);
                u32 base = (conv == 'x' || conv == 'X') ? 16u : 10u;
                int neg = 0;
                char *p;
                u32 len;
                if ((conv == 'd' || conv == 'i') && (v & 0x80000000u) != 0u) { neg = 1; v = (u32)(-(int)v); }
                tmp[15] = '\0';
                p = wp_utoa(&tmp[14], v, base);
                if (neg) { p--; *p = '-'; }
                len = (u32)(&tmp[15] - p);
                while (len < width) { if (n + 1u < cap) { out[n] = zero ? '0' : ' '; n++; } width--; }
                while (*p != '\0' && n + 1u < cap) { out[n] = *p; n++; p++; }
                continue;
            }
            if (conv == '%') { out[n] = '%'; n++; continue; }
            /* 不认识的转换：原样吐出 */
            if (n + 1u < cap) { out[n] = '%'; n++; }
            if (n + 1u < cap) { out[n] = conv; n++; }
        }
    }
    out[n] = '\0';
    return (int)n;
}

static int lib_printf_line(const char *tag, const char *fmt, va_list ap)
{
    char buf[160];
    int n = wp_vfmt(buf, sizeof(buf), fmt, ap);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        n--;
        buf[n] = '\0';
    }
    if (n > 0) {
        ets_printf("[%s] %s\n", tag, buf);
    }
    return n;
}

#define LIB_PRINTF(name, tag)                     \
    int name(const char *fmt, ...)                \
    {                                             \
        va_list ap;                               \
        int r;                                    \
        va_start(ap, fmt);                        \
        r = lib_printf_line(tag, fmt, ap);        \
        va_end(ap);                               \
        return r;                                 \
    }

LIB_PRINTF(net80211_printf, "net80211")
LIB_PRINTF(pp_printf, "pp")
LIB_PRINTF(core_printf, "core")
LIB_PRINTF(sc_printf, "smartcfg")
LIB_PRINTF(mesh_printf, "mesh")
LIB_PRINTF(wapi_printf, "wapi")

/* ---- 其余闭源库要的裸符号 ----
 * free(): libpp 的 hal_crypto 直接调 libc 的 free。我们的 osi_malloc 才是分配器，
 *          所以 free 必须落回同一个堆（osi_free 会校验块头 magic，外来指针安全返回）。
 * puts():  net80211_api 里的自检/断言用它。
 * hexstr2bin(): wpa 的 "hex 字符串 -> 字节串"（ieee80211_ioctl / wapi 用）。 */
void free(void *p)
{
    osi_free(p);
}

/* ---- 闭源库 + lwIP 需要的几个裸 libc 符号（§210）----------------------
 * lwIP 用 MEM_LIBC_MALLOC/MEMP_MEM_MALLOC，会直接引用 malloc/calloc/realloc；
 * netif.c 用 atoi()；ip4_addr.c 的 inet_aton 用 isdigit()（= 查 _ctype_ 表）。
 * 这些都必须在**同一个堆**上：free() 已经落回 osi_free，所以 malloc 也必须是
 * osi_malloc，否则"谁分配、谁释放"就对不上了。 */
void *malloc(u32 n)
{
    return osi_malloc(n);
}

void *calloc(u32 n, u32 sz)
{
    void *p = 0;

    if (n == 0u || sz == 0u) {
        return 0;
    }
    if (n > 0x00FFFFFFu / (sz == 0u ? 1u : sz)) {
        return 0;                    /* 溢出保护：库可能给离谱的值 */
    }
    p = osi_malloc(n * sz);
    if (p != 0) {
        (void)memset(p, 0, n * sz);
    }
    return p;
}

void *realloc(void *p, u32 n)
{
    return osi_realloc(p, n);
}

int atoi(const char *s)
{
    int v = 0;
    int neg = 0;

    if (s == 0) {
        return 0;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (int)(*s - '0');
        s++;
    }
    return neg ? -v : v;
}

/* newlib 布局：_ctype_[0] 对应 EOF，字符 c 的属性在 _ctype_[c+1]。
 * 位定义：_U 01 _L 02 _N 04 _S 08 _P 10 _C 20 _X 40 _B 80 */
const char _ctype_[257] = {
    0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x08, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

/* errno：lwIP 的 sockets.c 会引用它（lwip/errno.h 里是 extern int errno）。
 * freestanding 镜像里没有 libc 提供这个变量，只能自己给一个（windowed 侧）。 */
int errno;

/* printf：windowed 侧的实现（§214；snprintf 本文件里早就有）。
 * TLS 客户端（net/*.c6 编成 windowed 对象）和我们自己的板级用例都要打日志，
 * 而 freestanding 镜像里没有 libc 的 printf；这里用现有的 wp_vfmt() 顶。 */
int printf(const char *fmt, ...)
{
    char buf[256] = { 0 };
    va_list ap;
    int n = 0;

    va_start(ap, fmt);
    n = wp_vfmt(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s3_win_log(buf);
    }
    return n;
}

int puts(const char *s)
{
    if (s != 0) { s3_win_log(s); s3_win_log("\n"); }
    return 0;
}

static int wh_hexval(char c)
{
    if (c >= '0' && c <= '9') { return (int)(c - '0'); }
    if (c >= 'a' && c <= 'f') { return (int)(c - 'a') + 10; }
    if (c >= 'A' && c <= 'F') { return (int)(c - 'A') + 10; }
    return -1;
}

int hexstr2bin(const char *hex, u8 *buf, u32 len)
{
    u32 i;
    if (hex == 0 || buf == 0) { return -1; }
    for (i = 0u; i < len; i++) {
        int hi = wh_hexval(hex[2u * i]);
        int lo = wh_hexval(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) { return -1; }
        buf[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

/* WIFI_EVENT：IDF 用 ESP_EVENT_DEFINE_BASE(WIFI_EVENT) 定义的事件基址，
 * 实际就是 `const char *WIFI_EVENT = "WIFI_EVENT";` 的 4 字节指针。
 * 闭源库把它当指针比较/透传，我们的事件桩不解引用，给个合法指针即可。 */
const char *WIFI_EVENT = "WIFI_EVENT";
