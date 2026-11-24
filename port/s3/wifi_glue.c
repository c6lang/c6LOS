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

void s3_fix_ic16(void);
void s3_fix_menuconfig(void);
void *g_osi_funcs_p = (void *)&g_wifi_osi_funcs;

extern volatile u32 g_osi_calls[128];
volatile u32 g_osi_calls[128];

/* §1113：RTC 自初始化计数。RTC 未清零，读到的值 >1000 就当垃圾重置。
 * 本项目里这是唯一可靠的观测手段（GDB 读 DRAM 读到 0/垃圾、flash 断点不命中）。 */
__attribute__((section(".flash.text"))) static void s3_cnt(u32 addr)
{
    volatile u32 *cp = (volatile u32 *)addr;
    u32 v = *cp;
    if (v > 1000u) { v = 0u; }
    *cp = v + 1u;
}
extern int s3_ets_printf(const char *fmt, ...);

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

/* §674: 库自己构造的 scan-start 消息（b0=0x305），由 __wrap_pp_post 截获。
 * §629 的手工消息布局不对（msg[0]=5、m[4]=0），FSM 立刻 cancel/end；
 * 这里保存库构造的真消息指针，交给直接调用路径用。 */
u32 g_s3_real_scan_msg = 0u;

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
/* §432：**从 64KB 降到 32KB**。
 *
 * 原来 64KB 把 `.bss` 顶到 `_bss_end = 0x3fcd8c6e`，而 https 目标的
 * `_stack_top = 0x3FCD9500`、`_stack_limit = _bss_end + 0x800`，
 * 于是**主栈只剩 146 字节**。真板实测 `port_task_switch` 的外出 sp 是
 * `0x3fcd9390` —— 已经低于 `_stack_limit`，从头就在往 `.bss` 里写。
 * 全局变量被自己踩烂，什么怪现象都出得来。
 *
 * 32KB 之后 `_bss_end = 0x3fcd0c6e`，主栈有 ~32KB，恢复正常。 */
/* §437：32KB **不够** —— 真板崩在 `ram_chip_i2c_readReg`：
 *   `l32i a8, a8, 0x168` 且 vaddr=0x168，即 a8 是 0
 * （某个 i2c 函数表指针没分配出来）。库在 PHY 阶段就要这么多堆。
 * 48KB 是"能装下库"与"给主栈留够"之间的折中。 */
static u32 g_wifi_heap_mem[12288];      /* 48KB */
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
        extern int s3_ets_printf(const char *fmt, ...);
        s3_ets_printf("[heap] 分配失败 n=%u\n", (unsigned)n);
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
        s3_ets_printf("[semnew] #%u p=%08x max=%u init=%u t=%u\n",
                   (unsigned)s->seq, (unsigned)(void *)s, (unsigned)max, (unsigned)init,
                   (unsigned)s3_w2c_current_task());
    }
    return s;
}

static void *osi_semphr_create(u32 max, u32 init)
{
    /* ★★★ §1109：**去掉 HIT_OF**！
     * 真板计数（用 RTC 自初始化计数器做的）：
     * `_semphr_create` 被调 4 次，但 `wsem_new` 只进了 2 次 ——
     * 另外两次卡在 `HIT_OF` 里（它写 `g_osi_calls[]` 和 RTC 0x50000204 的环）。
     * 诊断代码不该主导故障（§243 已经因为同类原因把 sem_trace 停用过）。 */
    return (void *)wsem_new(max, init);
}
static void osi_semphr_delete(void *sem)
{
    s3_ets_printf("[sdel] sem=%08x t=%u\n", (unsigned)(u32)sem, (unsigned)s3_w2c_current_task());
    s3_cnt(0x50000E70u);
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
        s3_ets_printf("[take] t=%u sem#%u tk=%x\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)((s != 0) ? s->seq : 0u), (unsigned)ticks);
    }
    int ok;
    if (ticks == 0xFFFFFFFFu) {
        s3_ets_printf("[take!] sem=%08x t=%u\n", (unsigned)(u32)sem, (unsigned)s3_w2c_current_task());
    }
    s3_cnt(0x50000E64u);
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
    s3_ets_printf("[give] sem=%08x t=%u\n", (unsigned)(u32)sem, (unsigned)s3_w2c_current_task());
    s3_cnt(0x50000E68u);
    HIT_OF(_semphr_give);
    if (g_dbg_sync != 0u) {
        s3_ets_printf("[give] t=%u sem#%u\n", (unsigned)s3_w2c_current_task(),
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
    void *m;
    s3_ets_printf("[mux] in\n");
    m = wmux_new();
    s3_ets_printf("[mux] -> %08x\n", (unsigned)(u32)m);

    HIT_OF(_mutex_create);
    if (m != 0) {
        g_mtx_seq = g_mtx_seq + 1u;
        *(u32 *)((u8 *)m + 8u) = g_mtx_seq;   /* WMtx 的第 3 个字当序号 */
        if (g_dbg_sync != 0u) {
            s3_ets_printf("[mtxnew] #%u p=%08x t=%u\n", (unsigned)g_mtx_seq,
                       (unsigned)m, (unsigned)s3_w2c_current_task());
        }
    }
    return m;
}
static void *osi_recursive_mutex_create(void)
{
    void *r;
    s3_ets_printf("[rmux] in\n");
    HIT_OF(_recursive_mutex_create);
    r = wmux_new();
    s3_ets_printf("[rmux] -> %08x\n", (unsigned)(u32)r);
    return r;
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
        s3_ets_printf("[lock] t=%u m#%u p=%08x\n", (unsigned)s3_w2c_current_task(),
                   (unsigned)*(u32 *)((u8 *)m + 8u), (unsigned)(void *)m);
    }
    {
        int ok;
        B_IN(4u);
        int pr = s3_w2c_mutex_pend((void *)&m->m, 0u);

        ok = (pr == 0) ? 1 : 0;
        B_OUT(4u);
        if (g_dbg_sync != 0u) {
            s3_ets_printf("[lockok] t=%u pr=%d ok=%d\n", (unsigned)s3_w2c_current_task(),
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
        s3_ets_printf("[unlock] t=%u m#%u p=%08x\n", (unsigned)s3_w2c_current_task(),
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

    s3_ets_printf("[evwait] t=%u g=%x bits=%x all=%d tk=%x\n",
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
#ifdef S3_WIFI_NOBLOCK
u32 g_wifi_noblock = 1u;   /* §916: 编译期打开，测试"windowed 深调用链阻塞即崩"假说 */
#else
u32 g_wifi_noblock = 0u;   /* 实验开关：默认关闭 */
#endif
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
    /* §§1127：临时无条件打印，确认 pp_post 是否真的进到 _queue_send。 */
    s3_ets_printf("[send] t=%u qh=%x tk=%x q=%x\n", (unsigned)s3_w2c_current_task(),
               (unsigned)qh, (unsigned)ticks, (unsigned)q);
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
        /* §§1123：**最顶端计数**。+0=进入次数，+1=qh，+2=item，+3=ticks。 */
        volatile u32 *c = (volatile u32 *)0x50000F40u;
        u32 v = c[0]; if (v > 1000000u) { v = 0u; } c[0] = v + 1u;
        c[1] = (u32)qh;
        c[2] = (u32)item;
        c[3] = ticks;
    }
    /* §§1120 ★★★★：**每次 recv 都推进一次时间**。
     *
     * `S3_TICK_DEFER` 下时间只能由任务调 `s3_tick_consume()` 推进。
     * ppTask 手上有好几条库消息（pp_post 调了 7 次），它在 recv
     * 循环里一直有消息可取、从不阻塞；而内核又是无抢占的
     * ⇒ tick 永远无人消费 ⇒ 初始化任务的 `os_task_delay(1)` 永不到期。
     * 真板现场：`[delay] ticks=1 t=1` → `[give] ...` 之后再无进展。 */
    {
        extern void s3_tick_consume(void);
        extern void os_task_yield(void);
        static u32 s_rn = 0u;
        s3_tick_consume();
        /* §§1121：周期性让出。ppTask 手上有消息时会在 recv 里一直转，
         * 无抢占内核下初始化任务即使已被唤醒也拿不到 CPU。
         * 注意：这里直接调 windowed 的 `os_task_yield()`（普通 call8），
         * **不能**用 `s3_w2c_task_yield`（那是另一层 W2C_SPILL 壳，嵌套进来会把 a1 弄成 0）。 */
        /* §§1130：周期性 `os_task_yield()` 已**移除**。
         * 它在 recv 的 W2C_SPILL 壳深处调 `port_core_id()`，而那时 `a1` 已被
         * 壳子从 RTC 槽里恢复成了坏值（真板：`a1=8`、
         * `exccause=0x1d`、`excvaddr=0xfffffff0`、`epc1=0x40378dfb`（`port_core_id` 的 `entry`））。
         * 库任务已降到优先级 20，不再需要抢占式让出。 */
        (void)s_rn;
    }
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
    /* ★★★ §1086：qh 为 NULL 时直接返回失败。
     *
     * §1085 真板实测：ppTask 首次被切进去时，库传给 `_queue_recv` 的
     * 队列句柄就是 **NULL**（崩在 `l32i.n a9, a7, 40`，a7=0、EXCVADDR=0x28）。
     * 让 body 去解引用空指针只会把整个系统打死；按"没收到"返回，
     * 库自己会重试或走别的路径。 */
    if (qh == 0) {
        /* §§§1110：**必须让出 CPU**！
         * ppTask 的主循环是 `_semphr_take(sem,0)` → `_queue_recv(xphyQueue,&msg,-1)`。
         * 库的 xphyQueue 全局是 0（§1095/§1096），所以 recv 永远走这条早返回。
         * 在 S3_NO_PREEMPT=1 的内核里，ppTask 于是一个**不让出的空转**，
         * 初始化任务永远回不来（切换环 0x50001500 明确显示：切到 id=2 之后再没切回）。
         * 让出 CPU 后，init 任务可以继续把 s_pp_task_create_sem 等资源建好。 */
        return 0;
    }
    /* §1087：库有时会传进来一个**垃圾小值**（真板实测 qh=0x05，
     * EXCVADDR=0x2d = 5+40，崩在 wq_resolve 的 l32i a8,a2,40）。
     * DRAM/flash 上的合法指针都 ≥ 0x3f000000，小值一律当"没收到"处理。 */
    if ((u32)qh < 0x3f000000u) { return 0; }
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
        /* §320 实验：这条打印原来走 s3_ets_printf + s3_w2c_current_task()（一个
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
    {
        volatile u32 *c = (volatile u32 *)0x50000EE0u;
        u32 v = c[0]; if (v > 1000000u) { v = 0u; } c[0] = v + 1u;
        c[1] = ticks;
        c[2] = w_ticks(ticks);
        c[3] = (u32)(void *)q;
        c[4] = q->q.count;
        c[5] = (u32)item;
    }
    if (ticks == 0u) {
        r = s3_w2c_queue_try_get((void *)&q->q, item);
    } else {
        r = s3_w2c_queue_get((void *)&q->q, item, w_ticks(ticks));
    }
    {
        volatile u32 *c = (volatile u32 *)0x50000EE0u;
        u32 v = c[6]; if (v > 1000000u) { v = 0u; } c[6] = v + 1u;
        c[7] = (u32)(int)r;
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
#if 0  /* §676: ppTask(windowed) 里的 printf 会把 ppTask 卡在 port_putc；
        * 第 6 条消息后的 [recvq] 本身就是干扰源，先关掉看 FSM 是否继续。 */
    {
        u32 tid = s3_w2c_current_task();
        if (tid == 2u && (r == 0u || ticks != 0xFFFFFFFFu)) {
            s3_ets_printf("[recvq] id=%u ticks=%x r=%d it=%x\n",
                          tid, ticks, r, (u32)item);
        }
    }
#endif
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
    /* §§1126 ★★★★ 库任务必须比初始化任务**低**优先级。
     * 原来 `31 - prio`：ppTask 的 prio=29 ⇒ p=2（极高）。内核是无抢占的，
     * ppTask 一旦得到 CPU 就在 recv 循环里不让出；即使初始化任务已被
     * `unblock()` 置为 READY，`port_task_yield` 也永远挑 ppTask（优先级更高）。
     * 真板现场：`[delay] ticks=1 t=1` → `[give]` 之后再无进展，而超时计数
     * 0x50001600 已经是 1（说明超时确实到期并解除了阻塞）。
     * 改成固定低优先级 20（数字越大越低），保证初始化任务先跑完。 */
    u32 p = 20u;
    (void)prio;
    HIT_OF(_task_create);
    /* 库请求的栈往往偏小（ppTask 只要 3584），而 WiFi 协议栈的调用链很深，
     * 所以要抬。但栈池只有 24KB（port/s3/ctx.c 的 S3_STACK_POOL，§219），
     * 抬到 8192 的话"bringup 任务 8KB + ppTask 8KB + lwIP tcpip 4KB"就快满了，
     * 所以这里取 6144：够深调用的余量，又给别的任务留位置。
     *
     * §469：**改成 8192**。§468 抓到闭源 `pp` 任务崩溃时 `a7 = 0xc4b00000`
     * （windowed ABI 的 callee-saved 寄存器，只会存在栈上），
     * 而 `excvaddr = 0xc4b0002c` 恰好等于 `a7 + 44`，
     * 那个 `+44` 出现在 `ppTxFragmentProc` 里 —— 也就是这个任务自己。
     * callee-saved 被写坏 = 自己的栈被覆盖，而库里 `ppTask` 只要 3584、
     * 我们抬到 6144 仍嫌紧。`S3_STACK_POOL` 在 §449 已从 24KB 提到 40KB
     * （`bringup 8K + pp 8K + lwIP 4K = 20K`，还剩 20K），够抬。 */
    /* §727: 真板 JTAG 实测 ppTask(ctx[1]) 在扫描期间 sp=0x3fcbf150，
     * 低于其栈基 0x3fcbf670 达 0x520 字节 —— 8192 栈溢出，压进了 wifi 主任务
     * 的栈区，导致 ppTask 在 os_task_delay 里状态损坏、xphyQueue 第 6 条之后
     * 再也不消费。栈池 40960（§449），当前仅用 16KB，抬到 16384 安全。 */
    /* §959 ★ 24576 → 16384。
     *
     * lwIP 的 tcpip 线程（lwipopts NO_SYS=0）要 8192（sys_arch.c 的下限），
     * 而栈池 S3_STACK_POOL 只有 40960：
     *     wifi 主任务 16384 + ppTask + tcpip 8192 ≤ 40960  ⇒  ppTask ≤ 16384
     * 原来 clamp 到 24576 时：16384 + 24576 = 40960 **正好用光**，
     * tcpip 线程分不到栈 → os_task_create 返回 0 → lwip_socket/getaddrinfo
     * 没有线程可等，DHCP 也起不来，验收的整条后半段全断。
     * §727 的 JTAG 实测结论本来就是"8192 会溢出，抬到 16384 安全"，
     * 24576 是后来额外加的余量 —— 那时候 tcpip 线程还不存在，所以够用。 */
    if (stack < 16384u) { stack = 16384u; }
    g_next_task_windowed = 1u;   /* 库的任务入口都是 windowed ABI */
#ifdef S3_NO_LIB_TASKS
    /* §366 排查：**不真的建库任务**，只给个假句柄。用来判定崩溃是
     * 发生在"库任务开始执行"之后，还是主流程自己继续走就会崩。 */
    id = 1;
    if (handle != 0) { *(u32 *)handle = 1u; }
    s3_ets_printf("[task] 跳过库任务 fn=%08x\n", (unsigned)(u32)fn);
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
    s3_ets_printf("[task] 库建任务 fn=%08x id=%d stack=%u prio=%u(->%u)\n",
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
    s3_ets_printf("[delay] ticks=%u t=%u\n", (unsigned)ticks, (unsigned)s3_w2c_current_task());
    if (g_dbg_sync != 0u) {
        s3_ets_printf("[delay] t=%u ticks=%u\n", (unsigned)s3_w2c_current_task(),
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

/* §520：IDF 在 wifi_hw_start() 里的顺序是
 *   _wifi_apb80m_request → _wifi_clock_enable → _phy_enable → _wifi_rtc_disable_iso
 *   → _wifi_reset_mac → hal_init。
 * _wifi_reset_mac 映射到 periph_module_reset(PERIPH_WIFI_MODULE)，即脉冲
 * SYSTEM_WIFIMAC_RST (0x60026018 bit2)。我们以前把这个 OSI 槽位留成 glue_ret0，
 * MAC 从没在 PHY enable **之后**复位过 —— 而 hal_init 等的 0x60033d14 bit0
 * 很可能就是"MAC 复位释放/与 BB 同步"的硬件应答。补上它。 */
static void osi_wifi_reset_mac(void)
{
    volatile u32 *rst = (volatile u32 *)0x60026018u;
    volatile u32 k;

    HIT_OF(_wifi_reset_mac);
    *rst = *rst | (1u << 2);            /* SYSTEM_WIFIMAC_RST assert */
    for (k = 0u; k < 400u; k++) { }
    *rst = *rst & ~(1u << 2);           /* release */
    s3_ets_printf("[wifi] reset_mac pulse done\n");
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
 * 返回地址被 OR 上 bit31）。ROM 的 s3_ets_printf 从 windowed 调是安全的
 * （s3_win_log 一直这么用）。 */
extern int s3_ets_printf(const char *fmt, ...);

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
    s3_ets_printf("%s", out);
}

__attribute__((section(".flash.text")))
static void osi_log_write(u32 level, const char *tag, const char *fmt, const char *arg)
{
    OSI_HIT(83);
    /* §§1192 ★★★：**必须展开格式串**。
     *
     * 原来这里直接 `s3_ets_printf("%s", fmt)` 把格式串原样打出来，
     * 真板上就只看到 `[w1]wifi|%s`、`[w1]wifi|%c (%d) %s:` 这种
     * **根本看不出错误内容**的行——而扫描失败的真正
     * 原因就藏在这些消息里。
     * 库调 `_log_write(level, tag, fmt, arg)` 只带**一个**参数，
     * 这里把它装成一个临时参数数组交给 `c6_log_fmt`。 */
    if (level < 2u) {
        s3_ets_printf("[w%u]%s|", level, c6_ok_str((u32)(void *)tag) ? tag : "?");
        if (c6_ok_str((u32)(void *)fmt)) {
            s3_ets_printf(fmt, arg);
        }
        s3_ets_printf("\n");
    }
    (void)arg;
}

__attribute__((section(".flash.text")))
static void osi_log_writev(u32 level, const char *tag, const char *fmt,
                           void *ap, char *buf, u32 len)
{
    OSI_HIT(84);
    /* §641: 临时放开全部级别，看库自己的 scan FSM 日志 */
    if (level > 6u) { return; }
    s3_ets_printf("[v%u]%s|", level, c6_ok_str((u32)(void *)tag) ? tag : "?");
    if (!c6_ok_str((u32)(void *)fmt)) {
        /* fmt 不是字符串：把 va_list 里的东西直接摊开看 */
        u32 p = (ap != 0) ? *(u32 *)ap : 0u;
        s3_ets_printf("(fmt=%08x)", (u32)(void *)fmt);
        if (p >= 0x3F000000u && p < 0x3FE00000u) {
            u32 i;
            u32 *w = (u32 *)p;
            s3_ets_printf(" args@%08x:", p);
            for (i = 0u; i < 6u; i++) { s3_ets_printf(" %08x", w[i]); }
        }
        s3_ets_printf("\n");
        return;
    }
    c6_log_fmt(fmt, ap);
    s3_ets_printf("\n");
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
    /* §589: 模拟一个已初始化但**全空**的 NVS 分区：open 必须成功，
     * 后续 get_* 返回 NOT_FOUND 让库走默认值分支。 */
    if (out != 0) { *out = 1u; }
    return 0;
}
static void osi_nvs_close(u32 h) { HIT_OF(_nvs_close); (void)h; }
static int osi_nvs_get(const char *tag) { (void)tag; return OSI_NVS_ERR_NOT_FOUND; }
static int osi_nvs_put(const char *tag) { (void)tag; return OSI_NVS_ERR_NOT_INITIALIZED; }

#define OSI_NVS_STUB_GET(name, slot) \
    static int name(u32 h, const char *k, void *out) \
    { HIT_OF(slot); (void)h; (void)k; (void)out; return OSI_NVS_ERR_NOT_FOUND; }
#define OSI_NVS_STUB_SET(name, slot) \
    static int name(u32 h, const char *k, void *val) \
    { HIT_OF(slot); (void)h; (void)k; (void)val; return 0; }

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
    return 0;
}
static int osi_nvs_commit(u32 h) { HIT_OF(_nvs_commit); (void)h; return 0; }
static int osi_nvs_erase_key(u32 h, const char *k) { HIT_OF(_nvs_erase_key); (void)h; (void)k; return 0; }


/* 库对同一份实现会用不同槽位调用（内部堆 / wifi 专用堆），分开包装
 * 才能从 RTC 追踪里看出到底是哪个槽位在失败。 */
static void *osi_malloc_internal(u32 n)  { OSI_HIT(86); return osi_malloc(n); }
static void *osi_realloc_internal(void *p, u32 n) { OSI_HIT(87); return osi_realloc(p, n); }
static void *osi_calloc_internal(u32 n, u32 sz)   { OSI_HIT(88); return osi_calloc(n, sz); }
static void *osi_zalloc_internal(u32 n)  { OSI_HIT(89); return osi_calloc(1u, n); }
static void *osi_wifi_malloc(u32 n)      { OSI_HIT(90); return osi_malloc(n); }
static void *osi_wifi_realloc(void *p, u32 n)     { OSI_HIT(91); return osi_realloc(p, n); }
static void *osi_wifi_calloc(u32 n, u32 sz)       { OSI_HIT(92); return osi_calloc(n, sz); }
static void *osi_wifi_zalloc(u32 n)
{
    /* §§1117：esp_wifi_init_internal 在 wifi_init_in_caller_task 之后
     * 调 osi[93]=_wifi_zalloc(24)，之后才调 ieee80211_ioctl。
     * 打印能分辨它是不是卡在这里。 */
    s3_ets_printf("[zalloc] n=%u\n", (unsigned)n);
    OSI_HIT(93);
    {
        void *r = osi_calloc(1u, n);
        s3_ets_printf("[zalloc] -> %08x\n", (unsigned)(u32)r);
        return r;
    }
}

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

/* §642: 真实中断接线：保存库注册的 ISR，接进内核 level-1 中断分派 */
extern void intr_matrix_set(u32 cpu, u32 src, u32 intr);

static void (*s_wifi_isr_fn[8])(void *arg);
static void *s_wifi_isr_arg[8];

extern volatile u32 g_s3_diag[12];   /* §699: 前置声明（定义在文件末尾） */
/* §715: PWR 事件寄存器（0x60035118）从未测过。wDev_ProcessFiq 的 TBTT/TSF
 * 定时器事件从 PWR 取（a5=0x1e → wdev_process_tbtt；a6=0x1e0 → tsf_timer）。
 * 扫描 dwells 靠这些定时器推进；如果 PWR 一直为 0，就是"定时器没跑"。 */
volatile u32 g_s3_pwr_or = 0u;
#ifdef S3_NOOP_DATA_RX
/* §868: 数据帧 RX-success 的空处理（丢弃数据帧，避免 ROM TRAP）。 */
__attribute__((section(".flash.text")))
void s3_noop_data_rx(void *a) { (void)a; }
#endif

void s3_wifi_isr_dispatch(void)
{
    void (*fn)(void *) = s_wifi_isr_fn[0];
    g_s3_diag[8]++;   /* §699: BSS 计数器：WiFi ISR 实际触发次数（RTC 区被踩坏，不可用） */
    /* §709: 在 wDev_ProcessFiq 清掉事件之前抓取 MAC/PWR 事件寄存器，
     * 判断中断到底是 RX_DONE(真收到帧) 还是只有 TX/TBTT 之类的伪事件。
     * §711: 改为 OR 累积，看清整个扫描期间出现过的所有事件位。 */
    g_s3_diag[11] |= *(volatile u32 *)0x60033c3cu;
    g_s3_pwr_or |= *(volatile u32 *)0x60035118u;
    if (fn != 0) { fn(s_wifi_isr_arg[0]); }
}

static int osi_set_intr(u32 cpu_no, u32 source, u32 intr_num, u32 prio)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x50u);

    s3_ets_printf("[setintr] cpu=%u src=%u num=%u prio=%u\n", cpu_no, source, intr_num, prio);
    intr_matrix_set(cpu_no, source, intr_num);   /* §642: 真接中断矩阵 */
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

    s3_ets_printf("[setisr] n=%d fn=%x arg=%x\n", n, (u32)fn, (u32)arg);
    rec2(IRQ_RING_BASE, p[16], (u32)n, (u32)fn);
    if (n >= 0 && n < 8) { s_wifi_isr_fn[n] = (void (*)(void *))fn; s_wifi_isr_arg[n] = arg; }
    return 0;
}

static int osi_ints_on(u32 mask)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x40u);

    /* ★★★★ §1089：**绝不能在硬件上真的打开库的中断**。
     *
     * 我们用自己的 `__xt_vectors`（irq.S）接管了向量表，而 `_vec_lvl2..6`
     * 是"闪背光 N 下后停住"的调试处理器。库通过 `_set_isr` 装的 ISR
     * 只被记录、从未真正装进向量表（见 osi_set_isr），所以库的中断一旦
     * 在硬件上被使能，触发时只会落进我们的停机处理器 —— 真板 halt 读到
     * `PS=0x60225`（INTLEVEL=5）正是 `_vec_lvl5`。
     *
     * 内核本来就走轮询派发：`s3_timer_irq` 里置 `g_wifi_isr_pending`，
     * 任务上下文的 `s3_wifi_scan` 再调 `s3_wifi_isr_dispatch()`。
     * 所以这里只记账，不动 intenable。 */
    s3_ets_printf("[ints_on] mask=%x\n", mask);
    p[0] = p[0] + 1u;               /* 次数 */
    p[1] = mask;                    /* 最后一次 mask */
    return 0;
}

static int osi_ints_off(u32 mask)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x48u);

    { u32 v; __asm__ __volatile__("rsr %0, intenable" : "=r"(v));
      __asm__ __volatile__("wsr %0, intenable" : : "r"(v & ~mask));
      __asm__ __volatile__("rsync"); }
    s3_ets_printf("[ints_off] mask=%x\n", mask);
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
    /* §§1169：**单核 + 无抢占** 下不需要真的关中断。
     * `pp_post` 在这个临界区里做 `pp_sig_cnt` 的读-改-写；
     * 我们原来走 `s3_w2c_irq_save()`（= `port_irq_save`，要写 PS 寄存器），
     * 开销大且会把 `__real_pp_post` 推进一个极窄的时序窗口（真板：
     * `__wrap_pp_post` 里多一条指令就从 195 行掉到 90 行卡 `[pq]`）。
     * 改成空操作后这个窗口应该消失。 */
    return 0u;
}

static void osi_wifi_int_restore(u32 state)
{
    volatile u32 *p = (volatile u32 *)(IRQ_RING_BASE + 0x40u);

    (void)state;
    p[3] = p[3] + 1u;
}

static int osi_is_from_isr(void) { return 0; }

/* 事件：库通过它上报 WIFI_EVENT/IP_EVENT。我们没接事件系统，但**记下来**：
 * 上板时 RTC 里能看到 event_base/event_id，就知道关联到哪一步了。 */
static int osi_event_post(const char *base, int id, void *data, u32 size, u32 ticks)
{
    volatile u32 *p = (volatile u32 *)EVT_RING_BASE;
    u32 n = p[32];

    s3_ets_printf("[post] base=%08x id=%d\n", (u32)(void *)base, id);
    (void)data; (void)size; (void)ticks;
    p[(n & 15u) * 2u] = (u32)(void *)base;
    p[(n & 15u) * 2u + 1u] = (u32)id;
    p[32] = n + 1u;
    return 0;
}

/* ---- §687: ETS 软件定时器适配（OSI slot 56..60）-------------------------
 * 之前这 5 个槽位全是 glue_ret0：库调用 _timer_arm 后没有任何定时器会触发，
 * scan 状态机因此停在第一信道（[v4]wifi|first chan=0 / num=0）。
 * 这里用内核 tick（TICK_HZ=100，10ms/格）实现最小软件定时器；
 * 回调由 s3_ets_timer_dispatch() 在 windowed 上下文触发。
 * ETSTimer 布局（rom/ets_sys.h）：next@0 expire@4 period@8 func@12 arg@16。 */
#define S3_ETS_TIMER_MAX 4u
typedef struct {
    void *t;
    void (*fn)(void *);
    void *arg;
    u32 expire;      /* tick 绝对截止；0=未 armed */
    u32 period;      /* tick 周期；0=一次性 */
    u8 active;
} S3EtsTimer;
static S3EtsTimer s3_ets_timers[S3_ETS_TIMER_MAX];
/* §698: 0x50001100 是被踩坏的 RTC 区（读到 1242037968 垃圾），
 * 导致 `<=24` 守卫永远不成立、[tarm] 从不打印。改用 BSS 计数器。 */
extern volatile u32 g_s3_diag[12];   /* 定义在文件末尾 */
#define s3_ets_arm_n (g_s3_diag[6])
extern u64 s3_w2c_tick_get(void);

__attribute__((section(".flash.text")))
static S3EtsTimer *s3_ets_find(void *t, int create)
{
    u32 i = 0;
    while (i < S3_ETS_TIMER_MAX) {
        if (s3_ets_timers[i].active != 0u && s3_ets_timers[i].t == t) {
            return &s3_ets_timers[i];
        }
        i++;
    }
    if (create == 0) { return (S3EtsTimer *)0; }
    i = 0;
    while (i < S3_ETS_TIMER_MAX) {
        if (s3_ets_timers[i].active == 0u) {
            S3EtsTimer *e = &s3_ets_timers[i];
            e->t = t; e->fn = (void (*)(void *))0; e->arg = (void *)0;
            e->expire = 0u; e->period = 0u; e->active = 1u;
            return e;
        }
        i++;
    }
#ifdef S3_TIMER_RECLAIM
    /* §837: 库 init 阶段 setfn 占满 4 槽且都不 arm(expire=0)。AUTH 定时器
     * 注册新 t 时无空槽 → arm 失败。回收一个未 arm 的槽给新 t。 */
    i = 0;
    while (i < S3_ETS_TIMER_MAX) {
        S3EtsTimer *e = &s3_ets_timers[i];
        if (e->expire == 0u && e->period == 0u) {
            e->t = t; e->fn = (void (*)(void *))0; e->arg = (void *)0;
            e->expire = 0u; e->period = 0u; e->active = 1u;
            return e;
        }
        i++;
    }
#endif
    return (S3EtsTimer *)0;
}

__attribute__((section(".flash.text")))
static void osi_timer_setfn(void *t, void *fn, void *arg)
{
    S3EtsTimer *e = s3_ets_find(t, 1);
    if (e != 0) { e->fn = (void (*)(void *))fn; e->arg = arg; }
}

__attribute__((section(".flash.text")))
static void osi_timer_disarm(void *t)
{
    S3EtsTimer *e = s3_ets_find(t, 0);
    if (e != 0) { e->expire = 0u; e->period = 0u; }
}

__attribute__((section(".flash.text")))
static void osi_timer_done(void *t)
{
    S3EtsTimer *e = s3_ets_find(t, 0);
    if (e != 0) { e->active = 0u; e->fn = (void (*)(void *))0; e->arg = (void *)0; e->t = (void *)0; }
}

__attribute__((section(".flash.text")))
static void osi_timer_arm_ms(void *t, u32 ms, int repeat)
{
    S3EtsTimer *e = s3_ets_find(t, 1);
    u32 ticks = (ms + 9u) / 10u;             /* 1 tick = 10ms */
    if (ticks == 0u) { ticks = 1u; }
    if (e == 0) { return; }
    s3_ets_arm_n = s3_ets_arm_n + 1u;
    if (s3_ets_arm_n <= 24u) {
        s3_ets_printf("[tarm] t=%x ms=%u rep=%d ticks=%u fn=%x\n",
                      (u32)t, ms, repeat, ticks, (u32)e->fn);
    }
    e->expire = (u32)s3_w2c_tick_get() + ticks;
    e->period = (repeat != 0) ? ticks : 0u;
}

__attribute__((section(".flash.text")))
static void osi_timer_arm_us(void *t, u32 us, int repeat)
{
    osi_timer_arm_ms(t, (us + 999u) / 1000u, repeat);
}

__attribute__((section(".flash.text")))
void s3_ets_timer_dispatch(void)
{
    u32 now = (u32)s3_w2c_tick_get();
    u32 i = 0;
    while (i < S3_ETS_TIMER_MAX) {
        S3EtsTimer *e = &s3_ets_timers[i];
        if (e->active != 0u && e->expire != 0u && (i32)(now - e->expire) >= 0) {
            void (*fn)(void *) = e->fn;
            g_s3_diag[7]++;   /* §698: 记录 timer 实际触发次数 */
            void *arg = e->arg;
            if (e->period != 0u) { e->expire = e->expire + e->period; }
            else { e->expire = 0u; }
            if (fn != 0) { fn(arg); }
        }
        i++;
    }
}

/* §§1132：通用安全桩——不做任何事，返回 0（成功）。 */
__attribute__((section(".flash.text")))
int osi_stub_ok(void) { return 0; }

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
    /* §447：**默认关闭**祖先窗口 spill（原先要 `S3_NO_WIN_SPILL` 才关，默认是开）。
     * 理由见 w2c.S 里 WIN_SPILL 宏的注释：`s3_win_spill` 是 IDF 官方实现，靠把
     * WINDOWBASE 推满一整圈来触发硬件溢出，而溢出处理体要用 `[a1-12]` 帧链 ——
     * call0 内核的帧里没有这条链，于是拿脏 a9 写飞（§446 现场：StoreProhibited
     * excvaddr=0x0005ffe0、epc1=s3_win_spill+0x40 的 call12、windowbase=14）。
     *
     * 关掉是安全的：W2C 壳自己已经把 a0..a15 完整存进帧（0..60 那 16 条 s32i），
     * 窗口快照完整，wb 稳定在 0..4 不会绕圈。
     * 需要开启做对照实验时用 `-DS3_WIN_SPILL`。 */
/* ★★★ §1016（**根因修法**）：全 windowed 构建下也**必须**打开祖先窗口 spill。
     *
     * 原来这里只允许 `S3_WIN_SPILL && !S3_ONE_ABI`。当时关掉的理由是
     * "W2C 壳已经把 a0..a15 完整存进帧，窗口快照完整，wb 稳定在 0..4 不绕圈，
     *  窗口由硬件维护"。
     *
     * **这条前提是错的。** 硬件只保证"同一个执行流内"窗口账目正确；
     * 一旦任务在阻塞点被切走，另一个任务的 `call8` 链会把 16 个窗口的环
     * 推着转过一整圈，**把本任务还活着的祖先窗口在物理寄存器里覆盖掉**。
     * 壳把寄存器存进自己的帧救不了这件事 —— 被覆盖的是**祖先**的窗口，
     * 而祖先的帧没人去写。切回来时祖先的 `a1` 就变成了别的任务留下的值。
     *
     * `irq.c:410` 的注释其实早就把这个机制写对了，并且明确说"阻塞点靠壳里的
     * WIN_SPILL 保平安"—— 但在 `S3_ONE_ABI` 下那段 WIN_SPILL 是空宏（§1000/§1015），
     * 于是那句保证落空了三十多轮。
     *
     * 现在打开它是安全的：内核已全 windowed，每个帧都有 `[sp-12]` 帧链
     * （§934/§955 修复，壳入口也显式写了），`s3_win_spill` 的溢出处理体
     * 因此能算对落点 —— 当年用脏 a9 写飞的前提（call0 内核无帧链）已经不存在。 */
#if defined(S3_WIN_SPILL) && !defined(S3_ONE_ABI)
    g_s3_win_spill_en = 1u;
#else
    g_s3_win_spill_en = 0u;
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
    /* §1019 最终判别实验的结论（接线已恢复）：把 `_queue_recv` 指向
     * 纯 `return 0` 的 windowed 函数 `glue_ret0`，故障记录 `0x500018c0`
     * **一模一样**（a1=0 a0=0 epc1=0x403822df wb=4 ws=0x14）。
     * ⇒ **ppTask 调用任何 windowed 内核函数，返回后 a1 都是 0**，
     *   与具体实现、窗口层数、是否阻塞全部无关 ⇒ 靶子是 ppTask 的
     *   初始帧 / 窗口账目本身。 */
    /* §1033 判别结论（接线已恢复）：§1032 让壳能完整返回后，把
     * `_queue_recv` 指向纯 `return 0` 的 windowed 函数（**完全不经壳**），
     * 故障记录仍然一模一样（a1=3 / epc1=0x403822ef / wb=2）
     * ⇒ **壳已完全洗清**，破坏发生在"ppTask 做任何 windowed 调用"本身。*/
    /* §1044 直接推论实测（接线已恢复）：去掉壳那一层（`_queue_recv` 直连
     * `osi_queue_recv_body`），故障记录**逐字节不变**（a1=3 / epc1=0x403822ef）
     * ⇒ 少一层不够；链本身就有 7 层，而 16 窗口从 WB=2 起只够 6 层（§1043）。*/
    /* §1045 最浅链路判别结论（接线已恢复）：`_queue_recv` 直连内核队列函数
     * （ppTask→内核，只有 2 层窗口、根本不可能绕环），故障记录**逐字节不变**
     * （a1=3 / epc1=0x403822ef）⇒ **§1043 的深度/绕环理论也被推翻**：
     * 任何 windowed 调用返回后 ppTask 的 a1 都会被写成小常数，与深度无关。*/
    /* §1084：改走带 spill 的薄壳（阻塞点必须先在自由栈上 spill 祖先窗口） */
    { extern int s3_w2c_queue_recv(void *qh, void *item, u32 ticks);
      OSI_SET(_queue_recv, s3_w2c_queue_recv); }
    OSI_SET(_queue_msg_waiting, osi_queue_msg_waiting);
    /* §§1132 ★★★：补上之前漏登记的回调。
     *
     * 库里 `wifi_start_process`（真正建 `g_ic` 的那个函数）会调
     * `_wifi_clock_enable` / `_wifi_clock_disable` 来开关 WiFi 外设时钟，
     * 还会调 `_phy_update_country_info` 和一堆 `_coex_*`。
     * 我们只注册了 91/120 个槽位，这 30 个是 **NULL**——
     * 一旦被调就是跳到 0 或者把栈指针弄坏。真板现场正是
     * `wifi_start_process` 首次执行时 `a1=8` / `exccause=0x1d` 崩掉。
     * 这里全部装成"空操作但返回成功"的安全桩。 */
    OSI_SET(_wifi_clock_enable,        osi_stub_ok);
    OSI_SET(_wifi_clock_disable,       osi_stub_ok);
    OSI_SET(_phy_update_country_info,  osi_stub_ok);
    OSI_SET(_coex_init,                osi_stub_ok);
    OSI_SET(_coex_deinit,              osi_stub_ok);
    OSI_SET(_coex_enable,              osi_stub_ok);
    OSI_SET(_coex_disable,             osi_stub_ok);
    OSI_SET(_coex_status_get,          osi_stub_ok);
    OSI_SET(_coex_condition_set,       osi_stub_ok);
    OSI_SET(_coex_wifi_request,        osi_stub_ok);
    OSI_SET(_coex_wifi_release,        osi_stub_ok);
    OSI_SET(_coex_wifi_channel_set,    osi_stub_ok);
    OSI_SET(_coex_event_duration_get,  osi_stub_ok);
    OSI_SET(_coex_pti_get,             osi_stub_ok);
    OSI_SET(_coex_schm_status_bit_clear, osi_stub_ok);
    OSI_SET(_coex_schm_status_bit_set,   osi_stub_ok);
    OSI_SET(_coex_schm_interval_set,     osi_stub_ok);
    OSI_SET(_coex_schm_interval_get,     osi_stub_ok);
    OSI_SET(_coex_schm_curr_period_get,  osi_stub_ok);
    OSI_SET(_coex_schm_curr_phase_get,   osi_stub_ok);
    OSI_SET(_coex_schm_process_restart,  osi_stub_ok);
    OSI_SET(_coex_schm_register_cb,      osi_stub_ok);
    OSI_SET(_coex_register_start_cb,     osi_stub_ok);
    OSI_SET(_coex_schm_flexible_period_set, osi_stub_ok);
    OSI_SET(_coex_schm_flexible_period_get, osi_stub_ok);
    OSI_SET(_coex_schm_get_phase_by_idx, osi_stub_ok);
    OSI_SET(_dport_access_stall_other_cpu_start_wrap, osi_stub_ok);
    OSI_SET(_dport_access_stall_other_cpu_end_wrap,   osi_stub_ok);
    OSI_SET(_wifi_apb80m_request,      osi_stub_ok);
    OSI_SET(_wifi_apb80m_release,      osi_stub_ok);

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
    /* §687: ETS 定时器槽位 —— 之前全是 glue_ret0（空转），库的 scan/channel
     * dwell 计时器永远不触发。 */
    OSI_SET(_timer_arm,     osi_timer_arm_ms);
    OSI_SET(_timer_disarm,  osi_timer_disarm);
    OSI_SET(_timer_done,    osi_timer_done);
    OSI_SET(_timer_setfn,   osi_timer_setfn);
    OSI_SET(_timer_arm_us,  osi_timer_arm_us);
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
    OSI_SET(_wifi_reset_mac,  osi_wifi_reset_mac);
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

    s3_ets_printf("[evt] %s: 共 %u 条", tag, (unsigned)n);
    for (i = 0u; i < show; i++) {
        u32 base = p[i * 2u];
        u32 id = p[i * 2u + 1u];
        const char *kind = "?";

        if (base == (u32)(void *)WIFI_EVENT) {
            kind = "wifi";
        } else if (base != 0u) {
            kind = "ip/other";
        }
        s3_ets_printf(" [%s:%u]", kind, (unsigned)id);
    }
    s3_ets_printf("\n");
}

/* ---- OSI 调用统计：每个阶段结束打一遍，方便判断"库到底走到哪一步" --------
 * g_osi_calls[i] 是第 i 个 OSI 槽位被调用的次数。板子上没有断点，靠这个就能看出
 * 例如 "_semphr_create 有、_queue_recv 没有" 这种早期失败（§223）。 */
void s3_wifi_dump_osi(const char *tag)
{
    volatile u32 *c = (volatile u32 *)g_osi_calls;
    u32 i = 0u;
    u32 printed = 0u;

    s3_ets_printf("[osi] %s:", tag);
    for (i = 0u; i < 128u; i++) {
        if (c[i] != 0u) {
            s3_ets_printf(" %u=%u", (unsigned)i, (unsigned)c[i]);
            printed = printed + 1u;
            if (printed >= 16u) {
                break;              /* 一行最多 16 项，别把串口刷爆 */
            }
        }
    }
    if (printed == 0u) {
        s3_ets_printf(" (还没有任何回调)");
    }
    s3_ets_printf("\n");
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
extern int esp_wifi_set_country(const void *country);
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
        s3_ets_printf("[wifi] 没给 SSID：只做 set_mode/start/scan（射频体检）\n");
        (void)esp_wifi_set_storage(1);
        (void)esp_wifi_set_mode(1);
        s3_ets_printf("[wifi] start=%d\n", esp_wifi_start());
        (void)s3_wifi_scan();
        return 0xFFFFFFFFu;
    }
    while (ssid[n] != 0 && n < 32) { ((u8 *)cfg)[n] = (u8)ssid[n]; n++; }
    if (pass != 0) {
        n = 0;
        while (pass[n] != 0 && n < 64) { ((u8 *)cfg)[32 + n] = (u8)pass[n]; n++; }
    }
#ifdef S3_STA_CHANNEL6
    /* §818: connect 内部 scan 日志 "first chan=0"（§690 我们自己的 scan 也有过
     * 这个病，靠 cfg[8]=6 修好）。这里给 STA config 的 channel 字段（偏移 107）
     * 写 6，让 connect 的内部 scan 从 ch6 起扫，而不是空信道表。
     * wifi_sta_config_t: ssid@0,pass@32,scan_method@96,bssid_set@100,
     * bssid@101,channel@107。bssid_set 保持 0。 */
    ((u8 *)cfg)[96] = 0u;   /* scan_method = WIFI_FAST_SCAN */
    ((u8 *)cfg)[100] = 0u;  /* bssid_set = 0 */
    ((u8 *)cfg)[107] = 6u;  /* channel = 6 */
#endif
#ifdef S3_BSSID_DIRECT2
    /* §816: bssid_set=1 + 目标 BSSID + ch6。这次配合 S3_FORCE_SYNC_CONNECT
     * （上次 §814 缺这个，connect 走异步没读到 config）。字段偏移按
     * IDF 6.0.1 wifi_sta_config_t: bssid_set@100, bssid@101, channel@107。 */
    ((u8 *)cfg)[96] = 0u;                        /* scan_method = WIFI_FAST_SCAN */
    ((u8 *)cfg)[100] = 1u;                       /* bssid_set */
    ((u8 *)cfg)[101] = 0xdcu; ((u8 *)cfg)[102] = 0xd8u;
    ((u8 *)cfg)[103] = 0x7cu; ((u8 *)cfg)[104] = 0x5au;
    ((u8 *)cfg)[105] = 0x7au; ((u8 *)cfg)[106] = 0x46u;
    ((u8 *)cfg)[107] = 6u;                       /* channel */
#endif
    s3_ets_printf("[wifi] associate ssid=%s\n", ssid);
    /* wifi_storage_t: 0 = WIFI_STORAGE_FLASH, 1 = WIFI_STORAGE_RAM */
    r1 = esp_wifi_set_storage(1);
    s3_ets_printf("[wifi] set_storage(RAM)=%d\n", r1);
    /* wifi_mode_t: 1 = WIFI_MODE_STA */
    r2 = esp_wifi_set_mode(1);
    s3_ets_printf("[wifi] set_mode(STA)=%d\n", r2);
    /* wifi_interface_t: 0 = WIFI_IF_STA */
    r3 = esp_wifi_set_config(0, (void *)cfg);
    s3_ets_printf("[wifi] set_config=%d\n", r3);
    /* §§1165 ★★★★ **直接调用** `wifi_init_process` / `wifi_start_process`，
     * 绕开 ppTask。真板已证实 `g_ic` 已分配（非 0）但内容全 0——
     * 因为 `wifi_init_process` 是 ppTask 的消息处理函数，而 ppTask 拿不到 CPU。
     * 这两个函数本身不依赖任务上下文（只读参数 + 操作 `g_ic`），
     * 所以可以在初始化任务里直接调。 */
    {
        extern int wifi_init_process(void *msg);
        extern int wifi_start_process(void);
        extern unsigned char g_ic[];
        u32 m[8];
        u32 i2;
        int ri;
        int rs;
        for (i2 = 0u; i2 < 8u; i2++) { m[i2] = 0u; }
        m[3] = *(volatile u32 *)(void *)g_ic;      /* msg->[12] = IC 指针 */
        ri = wifi_init_process((void *)m);
        /* §§1206 ★★★：直接调 `pp_attach()`。真板取证：
         * `esf_buf_setup` 初始化的 `eb_txdesc_space @0x3fcbbf00` **全为 0** ⇒ 它从未执行。
         * 而 `esf_buf_setup` 只在 `pp_attach` 里被调，`pp_attach` 只在 `ic_init` 里被调，
         * 但 `ic_init` 返回 0x101（已初始化）⇒ 它那次没把 pp 部分做成。 */
        {
            extern int pp_attach(void);
            /* §§1208 ★★★：`esf_buf_setup` 会读 `g_wifi_menuconfig + 4`
             * 当作缓冲个数，但真板上 `g_wifi_menuconfig @0x3fcd5660`
             * **是 NULL** ⇒ 它读地址 4 ⇒ 必崩。这里先把它补上。 */
            s3_fix_menuconfig();
            {
                extern void s3_seed_tx_buffers(void);
                int rp = pp_attach();
                s3_ets_printf("[ppattach] r=%d\n", rp);
            }
            /* §§1210 ★★★：`pp_attach` 只能执行一次（真板返回 0x101），
             * 而它里面的 `esf_buf_setup`（建 TX 缓冲池）那次没成功。
             * 真板证据：`eb_txdesc_space @0x3fcbbf00` 全 0 ⇒ 池子没建。
             * 直接再调一次 `esf_buf_setup()`。 */
            {
                extern int esf_buf_setup(void);
                extern void *esf_buf_alloc(u32 a, u32 b);
                /* §§1258 ★★★★ **最内层原因**：`esf_buf_alloc_dynamic` 读 `desc+16`（u16）当计数，
                 * 为 0 就直接返回 NULL。idx=9 时 `desc` = `g_eb_list_desc + 9*4*4` = `+144`，
                 * 所以计数字段在 **`g_eb_list_desc + 160`**。
                 * 真板实测那里是 `0x0000`。之前§1226/§1250 写的 +20/+32 都是错的。
                 * 先把它填成 32。 */
                {
                    extern unsigned char g_eb_list_desc[];
                    volatile unsigned char *d9 = (volatile unsigned char *)g_eb_list_desc + 144u;
                    if (*(volatile unsigned short *)(void *)(d9 + 16u) == 0u) {
                        *(volatile unsigned short *)(void *)(d9 + 16u) = 32u;
                    }
                }
                int rb = esf_buf_setup();
                s3_ets_printf("[esfsetup] r=%d\n", rb);
                /* §§1212：验证池子可用性——直接分一个试试。 */
                {
                    void *eb = esf_buf_alloc(1u, 0u);
                    s3_ets_printf("[esfalloc] -> %x\n", (unsigned)eb);
                }
            }
        }
        rs = wifi_start_process();
        s3_ets_printf("[direct] init=%d start=%d state=%u\n",
                      ri, rs,
                      (unsigned)*(volatile unsigned char *)(void *)(g_ic + 0x1E9));
    }
    r4 = esp_wifi_start();
    s3_ets_printf("[wifi] start=%d\n", r4);
    /* §§1128 ★★★★：`esp_wifi_start()` 只是把 `wifi_start_process`
     * 投进 wifi 任务的队列，真正建 `g_ic` 的是**那个任务**。
     * 我们把库任务降到优先级 20（低于初始化任务）后，
     * 初始化任务不主动阻塞，ppTask 就永远抢不到 CPU，
     * `wifi_start_process` 从未执行 ⇒ `g_ic` 恒为 0 ⇒ `esp_wifi_scan_start`
     * 在 `*(u8*)(g_ic+0x1E9) < 2` 处返回 0x3002 (NOT_STARTED)。
     * 这里主动睡一下：初始化任务 BLOCKED 后 ppTask 就能跑，
     * 它在 recv 里消费 tick 推进超时，睡满后我们自然醒来。 */
    /* §§1136：**泵已移除**。三种实现都试过：
     *   - `os_task_delay(1)` 直调内核：绕过薄壳的安全边界 ⇒ 窗口溢出崩溃
     *   - `s3_w2c_task_delay(1)` 走薄壳：不崩了，但 tick 只在 ppTask 的 recv 里推进，
     *     而 ppTask 正在跑 `wifi_start_process`（不进 recv）⇒ 超时永不到期 ⇒ 卡住
     *   - 长延时：同样卡住
     * 结论：**tick 推进依赖 ppTask 回到 recv** 是根本矛盾。下一步应该把 tick
     * 推进与超时处理挪到不依赖任务上下文的地方（如 ISR 里用
     * call0 实现的轻量超时扫描）。 */
    /* §1059：ESP_ERR_WIFI_NOT_STARTED 说明库内部没进 started 状态。
     * 读同一个状态机（§421 定位的 `g_wifi_osi_funcs_md5 + 4`），
     * 看 esp_wifi_start 到底有没有推进它。 */
    {
        extern unsigned char g_wifi_osi_funcs_md5[];
        volatile u32 *wst2 = (volatile u32 *)(void *)(g_wifi_osi_funcs_md5 + 4);
        s3_ets_printf("[wifi] start 后的库状态 = %u\n", (unsigned)*wst2);
    }
#ifdef S3_HOME_EARLY
    /* §857: esp_wifi_start 后 chm 已建好但 home 默认 0；立即设 home=current=6，
     * 从源头避免 mgmt_output 的 chm_is_at_home_channel() 假 → AUTH 帧入延迟队列。
     * g_chm=*(0x3fcef850)，home@+80、current@+82（u8）。 */
    {
        u32 chm0 = *(volatile u32 *)0x3fcef850u;
        if (chm0 != 0u) {
            *(volatile u8 *)(chm0 + 80u) = 6u;
            *(volatile u8 *)(chm0 + 82u) = 6u;
        }
    }
#endif
#ifdef S3_WDEV_DATA_INIT
    /* §822: JTAG 实测 wDevCtrl_ptr@0x3fcef93c=NULL（应为 0x3fcbb548），
     * 而 wdev_data_init()@42022908 就是把它和 pTxRx/lmacConfMib/... 一堆
     * ROM 全局指针初始化的函数。正常由 wdev_funcs_init() 调用，但我们的
     * init 路径似乎没走到 → 数据帧 RX 走 wdevProcessRxSucDataAll 时
     * NULL 解引用 TRAP，EAPOL/4-way handshake 永远收不到。
     * 这里显式补调一次（幂等：只是把全局指针指向静态结构）。 */
    {
        extern void wdev_data_init(void);
        wdev_data_init();
    }
#endif
#ifdef S3_RXCB_EARLY
    /* §821: IDF 参考在 esp_wifi_connect 之前就通过 esp_netif 注册好 rxcb；
     * c6LOS 之前推迟到关联成功后（c6_lwip_start_dhcp），导致 connect 期间
     * 收到数据帧时 wdevProcessRxSucDataAll 经 ppq=25 走 sta_rxcb=NULL →
     * TRAP(vaddr=0)。这里在 start 之后、connect 之前提前注册 windowed 的
     * c6_lwip_rx，避免 NULL 解引用。 */
    {
        extern int esp_wifi_internal_reg_rxcb(int iface, void *cb);
        extern int c6_lwip_rx(void *buffer, unsigned short len, void *eb);
        (void)esp_wifi_internal_reg_rxcb(0, (void *)&c6_lwip_rx);
    }
#endif
    /* §598: 与 IDF 参考 app 的 hs("after-wifi-start") 完全相同的寄存器快照，
     * 用来定位两边从"after-wifi-init 都是 d14=0 ca0=0"之后在哪里分叉。 */
#ifndef S3_NO_HS_PRINT
    s3_ets_printf("[hs] d14=%x ca0=%x PWC=%x CLK=%x\n",
        *(volatile u32 *)0x60033d14u, *(volatile u32 *)0x60033ca0u,
        *(volatile u32 *)0x60008090u, *(volatile u32 *)0x60026014u);
#endif
    /* 关掉省电：modem sleep 需要 OSI 的 `_wifi_rtc_enable/disable_iso` 和定时器
     * （我们那两个是空实现），睡眠一开 RX 就可能收不到包。取一个页面不需要省电，
     * 所以显式 WIFI_PS_NONE(0)（§221）。 */
    s3_ets_printf("[wifi] set_ps(NONE)=%d\n", esp_wifi_set_ps(0));
#ifdef S3_CNX_DIRECT
    /* §817: arm connect FSM（清 AP 列表+起 re-scan 卡住），然后用我们
     * 能用的 forced-sync scan 重新灌满 AP 列表，最后直接调
     * cnx_connect_next_ap() 从列表里挑 weforpay.com 发起关联，
     * 绕开 connect 内部那个永远完不成的 re-scan。 */
    {
        extern void cnx_connect_next_ap(void);
        volatile u32 *wth_c = (volatile u32 *)0x3fcd633cu;
        u32 saved_c = *wth_c;
        *wth_c = (u32)s3_w2c_current_task();
        r5 = esp_wifi_connect_internal();
        *wth_c = saved_c;
        s3_ets_printf("[wifi] connect=%d\n", r5);
        (void)s3_wifi_scan();
        cnx_connect_next_ap();
    }
#elif defined(S3_CONNECT_FIRST)
    /* §813: 先 arm connect FSM（它内部 cancel+pp_post(0,0) 起 re-scan，
     * 异步不会完成），再用 forced-sync scan 把 AP 列表灌进去，
     * 让 connect 的 scan-done 回调能拿到我们找到的 weforpay.com。 */
    {
        volatile u32 *wth_c = (volatile u32 *)0x3fcd633cu;
        u32 saved_c = *wth_c;
        *wth_c = (u32)s3_w2c_current_task();
        r5 = esp_wifi_connect_internal();
        *wth_c = saved_c;
    }
    s3_ets_printf("[wifi] connect=%d\n", r5);
    (void)s3_wifi_scan();
#else
    (void)s3_wifi_scan();
#ifdef S3_SET_HOME_CH
    /* §853: JTAG 实测 g_chm home=0 但 current=6 → chm_is_at_home_channel()
     * 返回假 → mgmt_output 把 AUTH 帧塞进 g_ic+0x1a8 延迟队列永不发送
     * （g_s3_diag[5]=1 只有 scan probe；g_lwip_stage[3]=1 表示 auth 尝试过）。
     * 正常 connect 由 wifi_set_channel_process 设 home；我们的 RC-seed handoff
     * 绕过了它。这里显式把 home channel 设成 6（=current），让 AUTH 帧直接发出。 */
    {
        extern void chm_set_home_channel(void *chan);
        u8 hc[2];
        hc[0] = 6u; hc[1] = 0u;
        chm_set_home_channel((void *)hc);
    }
#endif
#ifdef S3_RC_DRIVE
    /* §832: scan 返回后 g_ic+116 已清 0，用 S3_RC_SEED 灌的 RC 驱动 FSM 到 auth。 */
    {
#ifdef S3_SET_HOME_CH
        /* §855: AUTH 帧在 handoff 里构造，必须在那之前让 home==current。
         * 直接写 g_chm->home(+80)=6，避开 chm_set_home_channel 的状态检查。 */
        {
            u32 chmp1 = *(volatile u32 *)0x3fcef850u;
            if (chmp1 != 0u) { *(volatile u8 *)(chmp1 + 80u) = 6u; }
        }
#endif
#ifdef S3_RC_HANDOFF
        /* §834: cnx_connect_next_ap 的 timer gate 不通；直接 handoff，
         * 传 slot+4(0x3fcd6b4c，全零 BSSID) 让 cnx_rc_search 命中 seed 槽。
         * seed 已把 slot+0 设成自指针，避免 420470ad 的 *(bss+0) NULL 解引用。 */
        extern void cnx_do_handoff_internal(void *bssid);
        cnx_do_handoff_internal((void *)0x3fcd6b4cu);
#else
        extern void cnx_connect_next_ap(void);
        cnx_connect_next_ap();
#endif
    }
#endif
#ifdef S3_RC_DIAG
    /* §829: scan 之后立刻读 RC count，看 scan_parse_beacon 是否真灌了 RC list。
     * 复用现有 "B2 cfg@.." 格式串，避免新增字符串触发 BSS collision。 */
    s3_ets_printf("B2 cfg@%08x osi=%08x magic@144=%08x\n",
                  *(volatile u32 *)(0x3fcd72d8u),
                  (u32)*(volatile u8 *)(0x3fcd72eeu),
                  *(volatile u32 *)(0x3fcd560cu));
#endif
#ifdef S3_NO_CONNECT
    /* §825: 诊断 —— 完全不调 connect，只看 forced-sync scan 是否
     * 通过 scan_parse_beacon→cnx_add_rc 灌满 cnx RC list。
     * 之后 JTAG 读 g_cnxMgr+0xf00+62 的 RC count。 */
    r5 = 0;
#elif defined(S3_FORCE_SYNC_CONNECT) || defined(S3_NVS_CH6)
    /* §811: connect 内部会 cancel+重启 scan(pp_post 0,0)，异步派发不通。
     * 只在 connect 期间让 current_task_is_wifi_task 返回真，走同步路径。 */
    {
#ifdef S3_NVS_CH6
        /* §826: ieee80211_sta_connect 从 g_wifi_nvs->data[144] 读 channel，
         * 现在是 0xff → 校验失败 → 传 0 给 cnx_sta_connect_cmd（全信道扫描，
         * 需要 timer 驱动的 dwell，而我们没有）。强制成 6，让 connect 的
         * 内部 scan 锁在目标信道，单信道即可完成，不必切信道。 */
        extern u32 g_wifi_nvs;
        u32 nv = *(volatile u32 *)&g_wifi_nvs;
        *(volatile u8 *)(nv + 144u) = 6u;
#endif
        volatile u32 *wth_c = (volatile u32 *)0x3fcd633cu;
        u32 saved_c = *wth_c;
        *wth_c = (u32)s3_w2c_current_task();
        r5 = esp_wifi_connect_internal();
        *wth_c = saved_c;
    }
#else
    r5 = esp_wifi_connect_internal();
#endif
#ifndef S3_NO_CONNECT
    s3_ets_printf("[wifi] connect=%d\n", r5);
#endif
#ifdef S3_DRAIN_AFTER_CONNECT
    /* §869: connect 之后用 s3_wifi_scan 里那套已验证的 windowed ISR+timer
     * 驱动序列，在 associate 本函数（已证扫描能收帧的上下文）里把 AUTH response
     * 从 MAC drain 进 802.11 层。wait_connected 里做同样的事会 TRAP，这里试同上下文。 */
    {
        extern volatile u32 g_wifi_isr_pending;
        extern void s3_wifi_isr_dispatch(void);
        int k;
        for (k = 0; k < 40; k++) {
            u32 ie = 0u;
            g_wifi_isr_pending = 0u;
            s3_wifi_isr_dispatch();
            __asm__ __volatile__("rsr %0, intenable" : "=r"(ie));
            ie |= 1u;
            __asm__ __volatile__("wsr %0, intenable; rsync" : : "r"(ie));
            s3_ets_timer_dispatch();
            s3_w2c_task_delay(100u);
        }
    }
#endif
#endif
#ifdef S3_SCAN_AFTER_CONNECT
    /* §817: connect 内部清掉 AP 列表并起自己的 re-scan（pp_post 0,0，
     * 异步永远完不成）。connect 返回后再跑一次我们能用的 forced-sync scan，
     * 靠它的 ISR+timer 轮询循环把 AP 列表重新灌满，让 cnx FSM 的 scan-done
     * 回调拿得到 weforpay.com。 */
    (void)s3_wifi_scan();
#endif
#ifdef S3_HANDOFF_DIRECT
    /* §823: connect 自己的 scan 永远不发 scan-done，cnx FSM 卡在 scan 态。
     * 我们已用 forced-sync scan 找到 weforpay.com，AP 记录已在库里。
     * 直接调 scan-done 回调 cnx_start_handoff_cb(0) 模拟"扫描成功"，
     * 让它经 cnx_remove_rc 挑 AP、推进到 auth。a2=0 = 扫描成功。 */
    {
        extern void cnx_start_handoff_cb(void *arg);
#ifdef S3_SCAN_CANCEL_RESCAN
        /* §824: connect 留下一个卡住的 scan（第二次 scan_start=12294），
         * cnx RC list 空。先 scan_cancel 清掉它，再跑一次 forced-sync scan
         * 重新解析 beacon 灌满 RC list，最后才 fire scan-done。 */
        extern void scan_cancel(void *ic);
        extern void *g_ic;
        scan_cancel(g_ic);
        (void)s3_wifi_scan();
#endif
        cnx_start_handoff_cb((void *)0);
    }
#endif
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
#ifdef S3_MESH_ROOT_HACK
    /* §831: scan_parse_beacon 里 cnx_add_rc 被 gate 跳过（a2!=0,
     * mesh_is_root==0, [bss+84]==0）。强制 mesh_is_root=1 让 gate 走
     * cnx_add_rc，用库自己造的完整 bss_info（+0 指针等字段都正确）。 */
    *(volatile u8 *)0x3fcbbc9cu = 1u;
#endif

    for (i = 0; i < 64; i++) { cfg[i] = 0; }
    for (i = 0; i < 256; i++) { rec[i] = 0; }
#ifdef S3_SSID_SCAN
    /* §845: 定向扫描只找目标 SSID，让 rec[0] 天然就是 weforpay.com，
     * 库的 BSS list 也一致（§844 硬编码 BSSID 被库校验拒绝）。
     * wifi_scan_config_t.ssid @0 是指针，指向 flash 里的 "weforpay.com"。 */
    *(u32 *)cfg = 0x42079458u;
#endif
#ifdef S3_SET_COUNTRY
    /* §685: 空 NVS 桩下 regulatory domain 可能无效（[v4]wifi|first chan=0）。
     * 显式设置中国国家码（1..13 信道，MANUAL 策略），排除"空信道表"假设。
     * wifi_country_t: cc[3]="CN", schan=1, nchan=13, max_tx_power=0, policy=1(MANUAL) */
    {
        u8 ctry[16];
        for (i = 0; i < 16; i++) { ctry[i] = 0; }
        ctry[0] = 'C'; ctry[1] = 'N'; ctry[2] = 0;
        ctry[3] = 1;    /* schan */
        ctry[4] = 13;   /* nchan */
        ctry[5] = 0;    /* max_tx_power */
        /* ctry[8..11] = policy (enum, 4 bytes) = 1 = MANUAL */
        ctry[8] = 1;
        s3_ets_printf("[country] set=%d\n", esp_wifi_set_country((const void *)ctry));
    }
#endif
#ifdef S3_SCAN_CHAN39
    /* §690 根因修正：cfg[39] 其实是 wifi_scan_config_t.ghz_5_channels 的
     * 第 4 字节（channel_bitmap@32: ghz_2 u16@32, ghz_5 u32@36）。
     * 写 cfg[39]=6 把 5G bitmap 变成 0x06000000，而 2G bitmap 仍为 0，
     * 于是 cnx_sta_scan_cmd 调到 scan_bitmap_to_channel_list(0, 0x06000000)
     * → "2G channel bitmap is 0 in 2G band" → 返回 -1，扫描被拒。
     * 正确做法：channel 字段在偏移 8（l8ui a2,8 → scan_set_desChan）。
     * 这里显式锁信道 6；cfg[32..39] 全部保持 0。 */
    /* §§1188：恢复锁定信道 6。§1184 把它改成 0 后行为完全一样，
     * 说明 `channel=0` 是**日志参数错位**的产物（见 §1186），不是配置真的为 0。
     * 锁定目标 AP 的信道对后续关联更有利。 */
    cfg[8] = 6;
#endif
#ifdef S3_PM_WAKE
    /* §691: ppProcessTxQ (40380963) 在发 probe 前调 pm_is_waked(ROM 4003c364)，
     * 它读 g_pm_ptr(0x3fcef8e4) 指向结构的 byte1：byte1==0 → 返回 1（awake），
     * 非 0 → 返回 0（sleeping）→ ppProcessTxQ 直接 return -1，probe 永远不发。
     * 这里打印并强制 byte1=0，验证"PM 睡眠导致零 AP"假设。 */
    {
        volatile u32 *pmp = (volatile u32 *)0x3fcef8e4u;
        u32 pm = *pmp;
        s3_ets_printf("[pm] ptr=%x b1=%u\n", pm,
                      pm != 0u ? (u32)*(volatile u8 *)(pm + 1u) : 0xffu);
        if (pm != 0u) { *(volatile u8 *)(pm + 1u) = 0u; }
        s3_ets_printf("[pm] forced b1=%u\n",
                      pm != 0u ? (u32)*(volatile u8 *)(pm + 1u) : 0xffu);
    }
#endif
#ifdef S3_CHM_RESET
    /* §693: 根因！chm_start_op(4202d45c) 要求 g_chm->op_state(+4)==0xff，
     * 否则 movi a9,3 直接返回 3，channel-enter 回调 4204abe0 永不执行，
     * probe 帧根本不生成 → ppProcessTxQ 返回 -2、零 AP。
     * JTAG 实测 g_chm[4]=0x1（残留的旧 op）。扫描前强制复位为 0xff。 */
    {
        volatile u32 *chmp = (volatile u32 *)0x3fcef850u;
        u32 chm = *chmp;
        s3_ets_printf("[chm] ptr=%x st=%u\n", chm,
                      chm != 0u ? (u32)*(volatile u8 *)(chm + 4u) : 0xffu);
        if (chm != 0u) { *(volatile u8 *)(chm + 4u) = 0xffu; }
        s3_ets_printf("[chm] forced st=%u\n",
                      chm != 0u ? (u32)*(volatile u8 *)(chm + 4u) : 0xffu);
    }
#endif
    /* §511 诊断：阻塞 scan 一直 12300 超时，改成异步 + 轮询结果，
     * 区分"射频没扫到 AP"和"scan-done 事件没送到等待者"。 */
    s3_ets_printf("[wifi] scan start（异步）\n");
    /* §§1288：在扫描之前才播种 TX 缓冲（此时库已完成初始化）。 */
    { extern void s3_seed_tx_buffers(void); s3_seed_tx_buffers(); }
#ifdef S3_FORCE_SYNC_SCAN
    /* §809: 让 current_task_is_wifi_task() 对本次调用返回真，强制
     * ieee80211_ioctl 走同步路径 (4203f620 -> ieee80211_ioctl_process -> b1)。
     * s_wifi_task_hdl=2(ppTask)，我们当前 slot=1，临时改成 1 后调用，再还原。 */
    {
        volatile u32 *wth = (volatile u32 *)0x3fcd633cu;
        u32 saved = *wth;
        u32 cur = (u32)s3_w2c_current_task();
        *wth = cur;
        r = esp_wifi_scan_start((const void *)cfg, 0);
        *wth = saved;
    }
#else
    r = esp_wifi_scan_start((const void *)cfg, 0);
#endif
    s3_ets_printf("[wifi] scan_start=%d\n", r);
    /* §§1170：直接处理扫描消息（绕开 ppTask）。
     * `g_s3_real_scan_msg` 是 `__wrap_pp_post` 里既有的捕获（`m[0]==0x305`）。
     * §1169 把 `_wifi_int_disable/restore` 改成空操作后，`__real_pp_post` 的时序窗口
     * 应已消失，这里加代码不应再把真板打回 90 行。 */
    {
        extern u32 g_s3_real_scan_msg;
        u32 sm = g_s3_real_scan_msg;
        if (r == 0 && sm != 0u) {
            extern int __real_wifi_scan_start_process(void *msg);
            extern unsigned char g_wifi_nvs[];
            int sr;
            /* §§1178：`wifi_scan_start_process` 的第一个检查是
             * `(*(u8*)(*g_wifi_nvs) & 0xFD) == 1`（存储类型必须是 RAM），
             * 否则直接 `return -1`。真板实测该字节是 **0x80**，
             * 所以直调永远返回 -1。这里在调用前强制修正为 1。 */
            {
                u32 nv = *(volatile u32 *)(void *)g_wifi_nvs;
                if (nv != 0u) { *(volatile unsigned char *)(void *)nv = 1u; }
            }
            s3_fix_ic16();   /* §1182（实现在 flash，避 IRAM 溢出） */
            sr = __real_wifi_scan_start_process((void *)sm);
            s3_ets_printf("[scanp2] direct r=%d\n", sr);
        }
    }
#ifdef S3_DIRECT_SCAN_CALL
    /* §675: init 时 patch 的 g_timer_func 被库覆盖回真函数；扫描前再 patch 一次，
     * 判定 pp_post(0,0) 是否真的驱动 ieee80211_timer_do_process。 */
    {
        extern int s3_wrap_timer_func(void *);
        *(volatile u32 *)0x3fcef8b8u = (u32)(void *)&s3_wrap_timer_func;
    }
    /* §629 判别实验：绕过 ppTask/队列，直接调用 scan 处理函数。
     * 消息布局照 esp_wifi_scan_start 的反汇编：b0=5(type), b4=回调,
     * b8=0, b10=0, b12=0, b20..=cfg(44 字节)。 */
    {
        u8 msg[80];
        int k;
        for (k = 0; k < 80; k++) { msg[k] = 0; }
        msg[0] = 5;                       /* ioctl type: scan */
        /* 回调指针填真函数地址；这里只用真实处理函数，不依赖 wrap */
        {
            extern int __real_wifi_scan_start_process(void *);
            *(u32 *)(void *)(msg + 4) = (u32)(void *)&__real_wifi_scan_start_process;
        }
        /* b8..b12 留 0（见反汇编 4203e5bb） */
        for (k = 0; k < 44; k++) { msg[20 + k] = cfg[k]; }
        {
            extern int __real_wifi_scan_start_process(void *);
            extern u32 g_s3_real_scan_msg;
            u32 sm = g_s3_real_scan_msg ? g_s3_real_scan_msg : (u32)msg;
            int rr;
            /* §674: 优先用库造的 scan-start 消息（b0=0x305、m[4]=cfg 指针），
             * §629 手工 msg 布局不对导致 FSM 立刻 cancel/end。 */
            s3_ets_printf("[dirscan] msg=%x\n", sm);
            rr = __real_wifi_scan_start_process((void *)sm);
            s3_ets_printf("[dirscan] r=%d\n", rr);
        }
    }
#endif
    if (r != 0) {
#ifdef S3_DRIVE_BUSY_SCAN
        /* §817: connect 已经起了一个 scan 卡在 vIf 状态里（本函数第二次
         * 调用会拿到 12294=ESP_ERR_WIFI_STATE），此时不能 early-return，
         * 要继续跑下面的 ISR+timer 轮询循环替 connect 把那个 scan 跑完。 */
        if (r != 12294) { return -1; }
#else
        return -1;
#endif
    }
    for (i = 0; i < 60; i++) {
        /* §705: WiFi MAC 中断在 call0 的 ISR 里只置标志；窗口化的库 ISR
         * 必须在本任务（windowed）上下文调用 —— ISR 里 call8 会在任意
         * WINDOWBASE 上旋转窗口，retw 与 CALLINC 不匹配，真板实测 a0 被
         * 写成垃圾掉进 _vec_default。本文件是 windowed 编译，直接 call8
         * 进 s3_wifi_isr_dispatch() ABI 正确。 */
        {
            extern volatile u32 g_wifi_isr_pending;
            {
                extern void s3_wifi_isr_dispatch(void);
                u32 ie = 0;
                g_wifi_isr_pending = 0u;
                s3_wifi_isr_dispatch();
                /* §708: wDev_ProcessFiq 已清掉 MAC/PWR 事件寄存器，
                 * 现在重新使能 intenable 的 WiFi 位，否则中断永久关闭。 */
                __asm__ __volatile__("rsr %0, intenable" : "=r"(ie));
                ie |= 1u;
                __asm__ __volatile__("wsr %0, intenable; rsync" : : "r"(ie));
            }
        }
#ifdef S3_ETS_TIMER_DISPATCH
        /* §687: 驱动 ETS 软件定时器 —— scan 的 channel-dwell 回调靠它推进 */
        s3_ets_timer_dispatch();
#endif
#ifdef S3_POLL_FIQ
        /* §635: 库注册的 wDev_ProcessFiq 被我们的空 set_isr 吞掉了。
         * 这里从 windowed 上下文直接周期调用它，手动 drain MAC 事件。 */
        {
            /* §678: 无日志版本，省 IRAM */
            extern void wDev_ProcessFiq(void *arg);
            wDev_ProcessFiq(0);
        }
#endif
        s3_w2c_task_delay(100u);
#ifdef S3_POLL_SCANH
        /* §684: pm_offchan_ready_or_defer 返回 0（deferred），它把 scan_start_handler
         * 存进 g_scan 等待 PM 稍后回调；我们的 PM stub 不会触发，所以真射频扫描
         * 从没启动。直接从轮询循环调用 scan_start_handler，驱动信道扫描。 */
        {
            extern void scan_start_handler(void *arg);
            if (i < 40u) { scan_start_handler(0); }
        }
#endif
#ifdef S3_POLL_TXQ
        /* §682: ppTask 没处理 pp_post(0,0) 的消息；jump-table[0] 的 handler
         * 是 ppProcessTxQ(4)，它才真正把 probe request 发出去。手动驱动它。
         * §692: 打印返回值 —— -1=lmacIsIdle/pm_is_waked 拒绝, -2=TX 队列无帧,
         * 0=真的处理了。一次分清"probe 没入队"与"MAC 没放行"。 */
        {
            extern int ppProcessTxQ(int arg);
            if (i < 40u) {
                int txr = ppProcessTxQ(4);
                if (i < 3u) {
                    typedef int (*fn0)(void);
                    typedef int (*fn1)(int);
                    fn0 pmw = (fn0)0x4000549cu;      /* ROM pm_is_waked thunk */
                    fn1 lid = (fn1)0x40005334u;      /* ROM lmacIsIdle thunk */
                    s3_ets_printf("[txq] i=%d r=%d pmw=%d idle=%d\n",
                                  i, txr, pmw(), lid(4));
                }
            }
        }
#endif
#ifdef S3_POLL_TIMER
        /* §677: pp_post(0,0) 没能把 scan 状态机推进；直接从轮询循环调
         * ieee80211_timer_do_process，看扫描是否会往下走（真射频扫描）。 */
        {
            extern int s3_wrap_timer_func(void *);
            if (i < 40u) { (void)s3_wrap_timer_func(0); }
        }
#endif
        r = esp_wifi_scan_get_ap_num(&num);
        if (i % 10u == 0u) {
            {
                volatile u32 *bd = (volatile u32 *)0x50000370u;
                volatile u32 *rc = (volatile u32 *)0x50000800u;
                {
                    extern volatile u32 g_s3_txret, g_s3_txarg;
                    s3_ets_printf("[scand] i=%d r=%d num=%u yld=%u pm=%u txr=%u txa=%x\n",
                                  i, r, (unsigned)num, *(volatile u32 *)0x50001008u,
                                  *(volatile u32 *)0x3fcef8e4u != 0u ?
                                  (u32)*(volatile u8 *)(*(volatile u32 *)0x3fcef8e4u + 1u) : 0xffu,
                                  g_s3_txret, g_s3_txarg);
                }
                s3_ets_printf("[recv] n=%u t0=%u q0=%x tk0=%x t1=%u q1=%x tk1=%x phyq=%x\n",
                              rc[24], rc[0], rc[1], rc[2], rc[3], rc[4], rc[5],
                              *(volatile u32 *)0x3fcef904u);
                {
                    /* §654: 用已知 ABI 查 ppTask(id=2) 的阻塞状态 */
                    extern u32 s3_w2c_task_probe(u32 id, u32 what);
                    s3_ets_printf("[tsk2] st=%u wk=%u wo=%x wr=%d\n",
                                  s3_w2c_task_probe(2u, 0u), s3_w2c_task_probe(2u, 1u),
                                  s3_w2c_task_probe(2u, 2u), (int)s3_w2c_task_probe(2u, 3u));
                }
            }
        }
        if (r == 0 && num != 0u) {
#ifdef S3_CONNECT_IN_LOOP
            /* §818: connect 内部 re-scan 需要 ISR+timer 驱动才能完成；
             * 我们自己的 scan 靠本循环驱动才成功。一旦扫到 AP，就在本循环
             * 还活着时调 connect，然后**不 break**，继续驱动若干轮让 connect
             * 的 re-scan/auth 也能被同样的 ISR+timer 推进。 */
            {
                extern int esp_wifi_connect_internal(void);
                static int conn_done = 0;
                if (!conn_done) {
                    conn_done = 1;
                    (void)esp_wifi_connect_internal();
                }
                if (i >= 40u) { break; }   /* 再驱动 ~40 轮 */
            }
#else
            break;
#endif
        }
    }
#ifndef S3_NO_SCAN_NUM
#ifndef S3_NO_SCAN_NUM
    s3_ets_printf("[wifi] scan_get_ap_num=%d num=%u\n", r, (unsigned)num);
#endif
#endif
#ifdef S3_RC_DIAG
    /* §829: 在 get_ap_records 清空 BSS 链表之前读 RC count + BSS head。 */
    s3_ets_printf("B2 cfg@%08x osi=%08x magic@144=%08x\n",
                  *(volatile u32 *)(0x3fcd72d8u),
                  (u32)*(volatile u8 *)(0x3fcd72eeu),
                  *(volatile u32 *)(0x3fcd560cu));
#endif
    if (r != 0 || num == 0) {
        return (int)num;
    }
    r = esp_wifi_scan_get_ap_records(&one, (void *)rec);
    if (r == 0) {
        /* 偏移按 IDF 6.0.1 wifi_ap_record_t：bssid[6]@0, ssid[33]@6,
         * primary(u8)@39, second(enum/s32)@40, rssi(int8)@44（反汇编
         * wifi_get_ap_info_process 4203cb99 s8i a2,a3,44 确认）。 */
        s3_ets_printf("[wifi] AP0 ssid=%s ch=%u rssi=%d\n",
                   (const char *)(rec + 6), (unsigned)rec[39], (int)(signed char)rec[44]);
    }
#ifdef S3_RC_SEED
    /* §830: cnx_add_rc 从没被调用 → RC list 空 → cnx FSM 无候选 AP。
     * 手工用 scan 到的 AP 造一个 cnx bss_info（0x3c8 字节，放静态池 slot），
     * 灌进 RC list，再调 cnx_connect_next_ap 看 FSM 是否会推进到 auth。
     * rec[] 是 wifi_ap_record_t: bssid@0, ssid@6, ch@39, rssi@44。 */
    if (r == 0) {
        extern void cnx_add_rc(void *bss);
        u8 *slot = (u8 *)0x3fcd6b48u;   /* g_cnxMgr+8 + 2*0x3c8 静态池 slot */
        u32 *w = (u32 *)slot;
        u32 k;
        for (k = 0; k < 0x3c8u / 4u; k++) { w[k] = 0; }
        /* §835: 420470bc 的 fatal 检查要求 *(bss+0) == *(g_ic+16)（当前 cnx node 指针）。
         * 之前填自指针导致不等 → 落入 420470e4 无限循环（卡死无 auth TX）。 */
        *(u32 *)slot = *(volatile u32 *)0x3fcd54ecu;
        *(u32 *)(slot + 4) = *(u32 *)rec;                          /* BSSID[0..3] */
        cnx_add_rc(slot);
    }
#endif
    return (int)num;
}

/* 轮询"连上 AP 了吗"：返回 1 = 已关联。ap_info 至少 64 字节（我们只用前几个字节）。 */
/* §851: 挪到 flash 腾出 IRAM，让 S3_CONNECT_DRIVE2 的 ISR 驱动块能链接进 IRAM。
 * 调用方在 win_island 主流程（非 ISR），scan 在它之前已完成，挪 flash 不影响扫描时序。 */
__attribute__((section(".flash.text")))
int s3_wifi_wait_connected(int ms)
{
    u8 ap[96];
    int waited = 0;
    int i = 0;
    int r = 0;

    while (waited < ms) {
        for (i = 0; i < 96; i++) { ap[i] = 0; }
#ifdef S3_DRIVE_TOP
        /* §865: 早退判定之前先驱 ISR+timer。之前驱动块在 early-return 之后，
         * sta_get_ap_info 假阳性首轮就 return 1，驱动永远不执行（diag[8]=2）。
         * AUTH 请求已修好发出（§864），现在需要驱动把 AP 的 response 帧收进来。 */
        {
            extern volatile u32 g_wifi_isr_pending;
            extern void s3_wifi_isr_dispatch(void);
            u32 ie0 = 0u;
#ifdef S3_NOOP_DATA_RX
            /* §868: ppTask 的 idx25 数据帧 RX 处理 wdevProcessRxSucDataAll 在
             * call0 内核下 TRAP。直接改 ppTask thunk 的字面量 0x40382338
             * （=0x40005928）指向 no-op，让数据帧 RX 被丢弃而不 TRAP。 */
            *(volatile u32 *)0x40382334u = (u32)(void *)&s3_noop_data_rx;
#endif
#ifdef S3_RESTORE_WDECTRL
            *(volatile u32 *)0x3fcef93cu = 0x3fcbb548u;
#endif
            g_wifi_isr_pending = 0u;
            s3_wifi_isr_dispatch();
            __asm__ __volatile__("rsr %0, intenable" : "=r"(ie0));
            ie0 |= 1u;
            __asm__ __volatile__("wsr %0, intenable; rsync" : : "r"(ie0));
            s3_ets_timer_dispatch();
        }
#endif
#ifdef S3_SET_HOME_CH
        /* §854: 在早退判定之前就设 home=current=6。ROM chm_is_at_home_channel
         * 只比较 g_chm->home(+80) 与 current(+82)；之前把它放在驱动块里，
         * 但 sta_get_ap_info 的假阳性首轮就 return 1 了，根本没走到。 */
        {
            u32 chmp0 = *(volatile u32 *)0x3fcef850u;
            if (chmp0 != 0u) { *(volatile u8 *)(chmp0 + 80u) = 6u; }
        }
#endif
#ifdef S3_CONN_DIAG
        /* §816: 检查库到底有没有真连上 —— "Haven't to connect" 只是我们
         * 自己 sta_get_ap_info 的打印，可能 sta_get_ap_info 坏了但已关联。
         * 用 RTC 写避免新增 printf 字符串触发 BSS/stack collision。 */
        {
            extern int ieee80211_sta_is_connected(void);
            extern u8 g_sta_connected_flag;
            volatile u32 *cd = (volatile u32 *)0x50001700u;
            cd[0] = (u32)g_sta_connected_flag;
            cd[1] = (u32)ieee80211_sta_is_connected();
            cd[2]++;
        }
#endif
        r = esp_wifi_sta_get_ap_info((void *)ap);
#ifdef S3_REAL_ASSOC_GATE
        /* §856: sta_get_ap_info()==0 只是 ioctl 入队成功的假阳性。只有库自己的
         * g_sta_connected_flag==1 才是真关联。真关联前继续跑 ISR+timer 驱动，
         * 让延迟在 g_ic+0x1a8 的 AUTH 帧有机会被冲出去。 */
        {
            extern u8 g_sta_connected_flag;
            if (r == 0 && g_sta_connected_flag == 1u) {
                s3_ets_printf("[wifi] 已关联，ssid=%s channel=%u\n",
                           (const char *)(ap + 6), (unsigned)ap[39]);
                return 1;
            }
        }
#else
        if (r == 0) {
            s3_ets_printf("[wifi] 已关联，ssid=%s channel=%u\n",
                       (const char *)(ap + 6), (unsigned)ap[39]);
            return 1;
        }
#endif
#ifdef S3_CONNECT_DRIVE
        /* §812/§815: connect 内部 re-scan 靠 ISR+timer+TxQ 推进，但从
         * wait_connected 里调 s3_wifi_isr_dispatch()/ppProcessTxQ() 会在
         * ROM pc=400414a3 TRAP（call0/windowed 混调）。仅 timer 驱动无效。
         * 本宏保留为文档占位，实验结论见 §815。 */
        s3_ets_timer_dispatch();
#endif
#ifdef S3_CONNECT_DRIVE2
        /* §828: 用 s3_wifi_scan 里那套**经过验证的** ISR+timer 驱动序列
         * （含 §708 的 intenable 重新使能）来推进 connect 卡住的内部 re-scan。
         * §815 的 TRAP 是裸调 ISR、没有重开 intenable 也没有清 pending；
         * 这里严格复刻 scan 循环的每个动作。 */
        {
            extern volatile u32 g_wifi_isr_pending;
            extern void s3_wifi_isr_dispatch(void);
            u32 ie2 = 0u;
#ifdef S3_SET_HOME_CH
            /* §853: associate 里设的 home 被 esp_wifi_connect 重置；这里在
             * connect 之后的 wait 循环里每轮重设 home=current=6，让
             * chm_is_at_home_channel() 返回真 → AUTH 帧不再被延迟入队。
             * g_chm=*(0x3fcef850)，home channel 在 struct+80（u8）。 */
            {
                u32 chmp = *(volatile u32 *)0x3fcef850u;
                if (chmp != 0u) { *(volatile u8 *)(chmp + 80u) = 6u; }
            }
#endif
            g_wifi_isr_pending = 0u;
            s3_wifi_isr_dispatch();
            __asm__ __volatile__("rsr %0, intenable" : "=r"(ie2));
            ie2 |= 1u;
            __asm__ __volatile__("wsr %0, intenable; rsync" : : "r"(ie2));
            s3_ets_timer_dispatch();
        }
#endif
        s3_w2c_task_delay(100u);
        waited = waited + 10;
    }
#ifdef S3_CONN_DIAG2
    /* §817: 复用已有 printf 调用点（只加一个 %d 参数，不新增栈帧/字符串），
     * 打印库自己的 is_connected()，判定"Haven't to connect"是否只是
     * sta_get_ap_info 误报、真板其实已经关联。 */
    {
        extern int ieee80211_sta_is_connected(void);
        s3_ets_printf("[wifi] timeout(%d) apinfo=%d isconn=%d\n",
                      ms, r, ieee80211_sta_is_connected());
    }
#else
    s3_ets_printf("[wifi] 关联超时（%d ms），sta_get_ap_info=%d\n", ms, r);
#endif
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

    s3_ets_printf("B0 osi0=%08x osi@%08x calls@%08x\n",
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
    wcfg(96u,  1u);             /* §589: nvs_enable 打开（NVS 槽位已改成空分区语义） */
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

    /* §441：**这里必须保持 0**。
     *
     * 原来打开（§229 为了看同步点）。但 `osi_mutex_lock/unlock` /
     * `osi_semphr_take/give` 每次打 1~3 行，而库在 `esp_wifi_init_internal`
     * 内部要做**上千次**同步操作。115200 波特率下每行约 3.5ms，光打印就要
     * 好几分钟 —— 真板看起来就是"卡在 WSTAGE 0x1005 不动"，
     * 实际上库每推进一步都在等串口。
     *
     * §254 当初的注释说"关掉它反而走得更短"，正是因为打印改变了时序；
     * 现在 §438 把抢占切换关掉之后，时序依赖的前提已经不成立，
     * 关掉打印只会让 init 跑得更快。 */
    g_dbg_sync = 0u;
    s3_wifi_osi_init();
    s3_ets_printf("B1 slot13=%08x slot18=%08x magic=%08x ver=%08x\n",
               (u32)(void *)g_wifi_osi_funcs._semphr_create,
               (u32)(void *)g_wifi_osi_funcs._mutex_create,
               (u32)(void *)g_wifi_osi_funcs._magic,
               (u32)(void *)g_wifi_osi_funcs._version);
    g_init_task = s3_w2c_current_task();
    *(volatile u32 *)0x50001400u = g_init_task;
    s3_ets_printf("B2 cfg@%08x osi=%08x magic@144=%08x\n",
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
        s3_ets_printf("[wifi] init 前的库状态 @%08x = %u\n",
                   (unsigned)(u32)(void *)wst, (unsigned)*wst);
        /* §421 实验：我们手动做了 PHY enable，库的状态机残留成 2
         * （"正在初始化"），于是 init 直接返回 0x3013。复位成 1 再进去。 */
        if (*wst != 1u) { *wst = 1u; }
        irc = (u32)(int)esp_wifi_init_internal((const void *)g_wifi_cfg_blob);
        s3_ets_printf("[wifi] init 后的库状态 @%08x = %u\n",
                   (unsigned)(u32)(void *)wst, (unsigned)*wst);

        s3_ets_printf("[wifi] esp_wifi_init_internal -> %u\n", (unsigned)irc);
        /* ★★★ §1077：库的初始化是**异步**的。
         *
         * esp_wifi_init_internal 只是把 `wifi_init_process`（0x42039dec）
         * 作为消息 handler 投给 ieee80211_ioctl，然后立刻把状态置 3、返回 0。
         * 真正的 ic 对象（写 g_ic = 0x3fcd474c 的那个）要等 ppTask 处理完
         * 那条消息、走 wifi_init_process → wifi_lmac_init → ic_init 才会建出来。
         *
         * 而我们的 bringup 是同步一路跑下去的：init 返回后马上 set_mode/
         * set_config/scan_start，此时 g_ic 还是 NULL ⇒ esp_wifi_scan_start
         * 恒返回 0x3002（ESP_ERR_WIFI_NOT_STARTED）。§1070 已证明那条消息
         * 就躺在队列里（[scanmsg] 里能看到 handler=42039dec）。
         *
         * 这里主动让出 CPU 等它建好（一旦 g_ic 非 0 立刻继续）。 */
        /* §1077 试过在这里等 g_ic 就绪（让出 100×10 tick），真板实测会**挂住**
         * 在等待循环里、g_ic 仍为 0 —— 因为让出后没有任何东西把主任务唤醒。
         * 要等 ic 就绪，得先让 tick/超时链路真正跑通（§1076）。已撤回。 */
        g_dbg_sync = 0u;
        return irc;
    }
}
void *s3_wifi_calls_ptr(void) { return (void *)g_osi_calls; }

/* 诊断：把关键 OSI 函数的调用计数打到串口（供 .c6 侧经桥调用） */
static void pnm(const char *n, u32 v)
{
    s3_ets_printf("%s=%08x ", n, v);
}

void s3_osi_dump(void)
{
    s3_ets_printf("\n[osi] ");
    pnm("mutex",    g_osi_calls[18]);
    pnm("lock",     g_osi_calls[21]);
    pnm("qcreat",   g_osi_calls[23]);
    pnm("sem",      g_osi_calls[13]);
    pnm("task",     g_osi_calls[37]);
    pnm("evt",      g_osi_calls[31]);
    pnm("wmalloc",  g_osi_calls[90]);
    pnm("log",      g_osi_calls[83]);
    s3_ets_printf("\n");
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
    int r;
    s3_wrap_bump(2u);
    s3_ets_printf("[ictask] in\n");
    r = __real_ic_create_wifi_task();
    s3_ets_printf("[ictask] -> %d\n", r);
    return r;
}

extern int __real_ieee80211_ioctl_init(void);
int s3_wrap_timer_func(void *arg);   /* §664 forward */

int __wrap_ieee80211_ioctl_init(void)
{
    int r;
    s3_wrap_bump(3u);
    s3_ets_printf("[ioctl_init] in\n");
    r = __real_ieee80211_ioctl_init();
    s3_ets_printf("[ioctl_init] -> %d\n", r);
#ifdef S3_TRACE_IOCTL
    /* §655: 库把 g_config_func 设成内部 callx8 固定地址，--wrap 抓不到。
     * 这里直接把指针换成带日志的 wrapper，判定 ppTask 是否真的走到 ioctl 分发。 */
    extern int __wrap_ieee80211_ioctl_process(void *msg);
    *(volatile u32 *)0x3fcef8c0u = (u32)(void *)&__wrap_ieee80211_ioctl_process;
    s3_ets_printf("[ioctlinit] r=%d gcfg patched to %x\n", r,
                  *(volatile u32 *)0x3fcef8c0u);
#endif
#ifdef S3_TRACE_SCANMSG
    /* §664: 同样 patch g_timer_func，追踪扫描通道驻留定时器是否触发。
     * ieee80211_timer_do_process @0x42016de0，直接 callx8 到真函数。 */
    *(volatile u32 *)0x3fcef8b8u = (u32)(void *)&s3_wrap_timer_func;
    s3_ets_printf("[timerinit] gtimer patched\n");
#endif
    return r;
}

/* §664: g_timer_func 的日志 wrapper。签名按 ppTask 调用方式 (a10 = msg data)，
 * 直接 callx8 到库真函数 0x42016de0（windowed, a10 传入）。 */
int s3_wrap_timer_func(void *arg)
{
    typedef int (*fn_t)(void *);
    fn_t real = (fn_t)0x42016de0u;
    s3_ets_printf("[tmr] a=%x\n", (u32)arg);
    return real(arg);
}

extern int __real_pp_create_task(void);
int __wrap_pp_create_task(void)
{
    s3_wrap_bump(4u);
    return __real_pp_create_task();
}

extern int __real_pp_post(unsigned int ppq, unsigned int data);
/* §§1182：`ieee80211_sta_scan` 要求 `*(u32*)(g_ic+16)` 非 0，否则直接 return -1。
 * 真板实测它是 0；而 `*(u32*)g_ic` 就是 `g_cnxMgr`（0x3fcd5730）。先把它补进去试。
 * 放在 `.flash.text`，避免 IRAM 溢出。 */
/* §§1208：`esf_buf_setup` 会读 `g_wifi_menuconfig + 4` 当缓冲个数，
 * 但真板上 `g_wifi_menuconfig @0x3fcd5660` 是 NULL ⇒ 它读地址 4 ⇒ 必崩。
 * 这里先把它补上（放 flash，避 IRAM 溢出）。 */
/* §§1274 ★★★★ **自己把 TX 缓冲挂进空闲链表**。
 *
 * `esf_buf_alloc_dynamic` 在本构建里永远返回 0（见 §1270），
 * 所以 `esf_buf_setup` / `esf_buf_alloc` 都拿不到缓冲。
 * 但 `esf_buf_recycle(buf, type)` 是另一条路：它会把 buf 挂回
 * `g_eb_list_desc[type]` 的空闲链表（前提 `desc+16` 非 0）。
 * 所以：自己用 `_wifi_malloc` 分配几块、设好 `buf[26]=type`、
 * 再调 `esf_buf_recycle` 挂进去。 */
__attribute__((section(".flash.text")))
void s3_seed_tx_buffers(void)
{
    extern unsigned char g_eb_list_desc[];
    extern unsigned char g_wifi_menuconfig[];
    extern void *osi_malloc(u32 n);
    extern int s3_ets_printf(const char *, ...);
    u32 ids[3];
    u32 t;

    /* §§1288 ★★★★ 播种改到**扫描前**（库初始化完成之后），
     * 避免在库自己还在初始化 `g_eb_list_desc` 时就去改它（真板上表现为
     * 同一固件连续三次刷写得到 231/154/113 行，很不稳定）。
     *
     * 只种 **idx=2 / idx=4**（§1286 探针实测只有这两个能分到缓冲），
     * 每个 4 块，链表 next 在 **buf+40**。
     * 另外把 `menuconfig+8/+84` 置 0，否则 `esf_buf_alloc` 会直接走 dynamic 分支、
     * 根本不看空闲链表。 */
    *(volatile u32 *)(void *)(g_wifi_menuconfig + 8u) = 0u;
    *(volatile u32 *)(void *)(g_wifi_menuconfig + 84u) = 0u;
    /* §§1310：把 wdev 函数表里的分配器换成 **ROM 版**
     * （`libpp.a` 里那份在本构建下走不到分配路径）。 */
    {
        extern unsigned char *pp_wdev_funcs;
        volatile u32 *wf = (volatile u32 *)(void *)pp_wdev_funcs;
        if (wf != 0) {
            wf[0x188u / 4u] = 0x4003edbcu;   /* ROM esf_buf_alloc */
            wf[0x18cu / 4u] = 0x4003ebf8u;   /* ROM esf_buf_alloc_dynamic */
            s3_ets_printf("[wdev] patched 188=%x 18c=%x\n",
                          (unsigned)wf[0x188u / 4u], (unsigned)wf[0x18cu / 4u]);
        }
    }
    /* §§1289 ★★★★ 库真正用的是 **type=0 + idx 7/8/9**（管理帧）。
     * 从调用点反汇编：
     *   `403842ca: movi a11, 7;  movi a10, 0`  ⇒ esf_buf_alloc(0, idx=7)
     *   `4038692d: movi a11, 8;  movi a10, 0`  ⇒ (0, 8)
     * 之前播 idx 2/4 + type 1 是错的。
     * 一次播 6 块 × 3 个 = 18 × 1600B = 28.8KB，在 48KB 堆内。 */
    /* §§1312 ★★★★★ **`ieee80211_getmgtframe` 按帧长选的是 idx 2/3/4，不是 7！**
     *
     * 反汇编：`movi a11, 4` → 若 len<=0x100 则 `movi a11, 2`，
     * 若 len<=64 则 `movi a11, 3`。所以 idx ∈ {2,3,4}。
     * 真板也印证了这点：播种在 idx=7 的链表一直没被消费。
     * §1286 的探针也只有 idx 2 / 4 能分到缓冲 —— 完全对得上。 */
    ids[0] = 2u;
    ids[1] = 3u;
    ids[2] = 4u;
    for (t = 0u; t < 3u; t++) {
        u32 k = ids[t];
        /* §§1309 ★★★★★ **真正的列表基址在 `g_eb_list_desc_ptr`（`0x3fcef92c`）里！**
         *
         * ROM 版 `esf_buf_alloc`（`0x4003edbc`）：
         *   `l32r a6, g_eb_list_desc_ptr ; l32i.n a6, a6, 0`
         *   `addx4 a7, idx*4, a6` ⇒ desc = **base + idx*16**
         *   `l32i.n a6, a7, 0` → 空闲链表头
         * 真板 `g_eb_list_desc_ptr` = **`0x3fcbb3ac`**，
         * 而我们之前一直往 `g_eb_list_desc`（`0x3fcbb354`，**根本不是列表**）里写。 */
        extern unsigned char *g_eb_list_desc_ptr;
        volatile unsigned char *dk = (volatile unsigned char *)g_eb_list_desc_ptr + k * 16u;
        void *head = 0;
        void *prev = 0;
        u32 i2;
        for (i2 = 0u; i2 < 8u; i2++) {
            void *b = osi_malloc(320u);
            void *dt;
            if (b == 0) { break; }
            dt = osi_malloc(1600u);
            if (dt == 0) { break; }
            /* §§1296 ★★★：每块缓冲配一个数据区（`buf+4` 必须非 0，
             * 否则 `esf_buf_alloc` 会走 memcpy 用未初始化源指针而崩）。
             * `esf_buf` 本体 320 字节就够（实测最大字段偏移 ~200）。 */
            *((volatile void **)b + 1) = dt;                  /* buf+4 = 数据区 */
            *((volatile void **)b + 10) = 0;                  /* buf+40 = next */
            *((volatile unsigned char *)b + 26u) = 0u;        /* type = 0 */
            if (prev != 0) { *((volatile void **)prev + 10) = b; }
            else { head = b; }
            prev = b;
        }
        if (head != 0) {
            *(volatile void **)(void *)dk = head;
            if (*(volatile unsigned short *)(void *)(dk + 16u) == 0u) {
                *(volatile unsigned short *)(void *)(dk + 16u) = (unsigned short)i2;
            }
        }
        s3_ets_printf("[seed] idx=%u i=%u head=%x\n", (unsigned)k, (unsigned)i2, (unsigned)head);
    }
}

__attribute__((section(".flash.text")))
void s3_fix_menuconfig(void)
{
    extern unsigned char g_wifi_menuconfig[];
    static u32 s_mc[16];

    if (*(volatile u32 *)(void *)g_wifi_menuconfig == 0u) {
        s_mc[1] = 64u;
        *(volatile u32 *)(void *)g_wifi_menuconfig = (u32)(void *)s_mc;
        { extern int s3_ets_printf(const char *, ...);
          s3_ets_printf("[mc] set %x\n", (unsigned)(void *)s_mc); }
    }
}

__attribute__((section(".flash.text")))
void s3_fix_ic16(void)
{
    extern unsigned char g_ic[];
    extern int s3_ets_printf(const char *, ...);
    u32 *icp = (u32 *)(void *)g_ic;

    if (icp[4] == 0u && icp[0] != 0u) {
        icp[4] = icp[0];
        s3_ets_printf("[ic16] set to %x\n", (unsigned)icp[4]);
    }
}

/* §§1256 ★★★★ `esf_buf_setup` 的 static 分支被 ROM 函数
 * `config_is_cache_tx_buf_enabled()` 门控：返回 0 就不去建 TX 缓冲池。
 * 真板上它很可能返回 0，于是 `_wifi_malloc` 零调用、
 * `esf_buf_alloc` 恒 NULL、`ieee80211_send_setup` 断言死循环。
 * 这里强制让它返回 1。 */
__attribute__((section(".flash.text")))
int __wrap_config_is_cache_tx_buf_enabled(void)
{
    return 1;
}

int __wrap_pp_post(unsigned int ppq, unsigned int data)
{
    s3_wrap_bump(7u);
    s3_ets_printf("[pppost] ppq=%u data=%x\n", ppq, data);
    /* §§1164（无打印版）：`wifi_init_process` 需要 `msg->[12]` 非 0 且
     * `g_ic` 指向 0x2b4 字节的 IC 结构（它一进来就 memset(g_ic,0,0x2b4)）。
     * 不加任何打印，避免影响时序。 */
    if (ppq == 6u && data != 0u) {
        extern unsigned char g_ic[];
        volatile u32 *m = (volatile u32 *)data;
        if (m[3] == 0u) {
            if (*(volatile u32 *)(void *)g_ic == 0u) {
                /* 从 OSI 堆分配（不能用 static 数组：会把 .bss 撞到硬编码的栈界）。 */
                extern void *osi_malloc(u32 n);
                *(volatile u32 *)(void *)g_ic = (u32)osi_malloc(0x2b4u);
            }
            m[3] = *(volatile u32 *)(void *)g_ic;
        }
    }
#ifdef S3_TRACE_SCANMSG
    /* §656: 扫描消息由库内部 callx8 建好，--wrap 抓不到回调。
     * 在 pp_post 边界把消息头打出来，并确认 b1 是否 = wifi_scan_start_process。 */
    if (ppq == 6u && data != 0u) {
        volatile u32 *m = (volatile u32 *)data;
        s3_ets_printf("[scanmsg] %x: %08x %08x %08x %08x %08x %08x\n",
                      data, m[0], m[1], m[2], m[3], m[4], m[5]);
        /* §674: b0=0x305 是 scan-start；记下库构造的真消息指针 */
        if (m[0] == 0x305u) { g_s3_real_scan_msg = data; }
        /* §669: 只做裸内存读取，不调用 call0 内核函数（windowed ABI 会 TRAP）。
         * OsQueue: buf0 size4 cap8 head12 tail16 count20 magic24 wg28 wp32 */
        {
            volatile u32 *qp = (volatile u32 *)*(volatile u32 *)0x3fcef904u;
            if (qp != 0u) {
                s3_ets_printf("[pq] q=%x cnt=%u wg=%u hd=%u tl=%u\n",
                              (u32)qp, qp[5], qp[7], qp[3], qp[4]);
            }
        }
    }
#endif
    {
#ifdef S3_SIGFIX
        /* §697: pp_sig_cnt[ppq] 卡在 1 时，pp_post 对 ppq=0 根本不入 xphyQueue
         * （0x4201df8b 分支只调 OSI slot 11 就 ret），TX-kick 全部丢失。
         * 强制清零，让每次 post 都真正投递。 */
        if (ppq < 18u) { ((volatile u8 *)0x3fcbbd50u)[ppq] = 0u; }
#endif
        int rr = __real_pp_post(ppq, data);
#ifdef S3_TRACE_SCANMSG
        /* §671: 第 6 条消息投递后，用 windowed 包装读 ppTask 状态 + 队列计数。
         * os_task_probe 是 call0，直接从 windowed 的 wifi_glue.c 调会 TRAP，
         * s3_w2c_task_probe 才是正确的 windowed 入口。 */
        if (ppq == 6u || ppq == 0u) {   /* §679: 也看 scan timer 的 pp_post(0,0) */
            extern u32 s3_w2c_task_probe(u32 id, u32 what);
            volatile u32 *qp = (volatile u32 *)*(volatile u32 *)0x3fcef904u;
            s3_ets_printf("[w6] q=%u r=%d st=%u wk=%u wo=%x cnt=%u\n",
                          ppq,
                          rr, s3_w2c_task_probe(2u, 0u), s3_w2c_task_probe(2u, 1u),
                          s3_w2c_task_probe(2u, 2u), qp != 0u ? qp[5] : 0u);
        }
#endif
        return rr;
    }
}

extern int __real_wifi_scan_start_process(void *msg);
/* §651: 追踪 ppTask 的 jump-table[6] -> g_config_func 派发是否到达。 */
extern int __real_ieee80211_ioctl_process(void *msg);
int __wrap_ieee80211_ioctl_process(void *msg)
{
    int r;
    s3_ets_printf("[ioctlp] enter msg=%x b0=%x b1=%x\n", (u32)msg,
                  *(volatile u32 *)msg, *(volatile u32 *)((u8 *)msg + 4));
    r = __real_ieee80211_ioctl_process(msg);
    s3_ets_printf("[ioctlp] leave r=%d\n", r);
    return r;
}

int __wrap_wifi_scan_start_process(void *msg)
{
    int r;
    s3_ets_printf("[scanp] enter msg=%x b0=%x b1=%x\n", (u32)msg,
                  *(volatile u32 *)msg, *(volatile u32 *)((u8 *)msg + 4));
    r = __real_wifi_scan_start_process(msg);
    s3_ets_printf("[scanp] leave r=%d\n", r);
    return r;
}

extern int __real_ieee80211_sta_scan(void *arg);
int __wrap_ieee80211_sta_scan(void *arg)
{
    int r;
    s3_ets_printf("[stascan] enter arg=%x\n", (u32)arg);
    r = __real_ieee80211_sta_scan(arg);
    s3_ets_printf("[stascan] leave r=%d\n", r);
    return r;
}

/* §640: 追踪 scan_start 里两个可能把扫描挡掉的函数 */
extern int __real_pm_offchan_ready_or_defer(void *a, void *b);
int __wrap_pm_offchan_ready_or_defer(void *a, void *b)
{
    int r = __real_pm_offchan_ready_or_defer(a, b);
    s3_ets_printf("[pmoff] a=%x b=%x r=%d\n", (u32)a, (u32)b, r);
    return r;
}

extern int __real_chm_acquire_lock(void *a);
int __wrap_chm_acquire_lock(void *a)
{
    int r = __real_chm_acquire_lock(a);
    s3_ets_printf("[chmlk] a=%x r=%d\n", (u32)a, r);
    return r;
}

/* §694: 零 printf 计数器（JTAG 直接读 g_s3_diag[]），判定 probe 发送链在哪一环断掉。 */
volatile u32 g_s3_diag[12];
#define S3_PROBE_N (g_s3_diag[0])
#define S3_CHM_N   (g_s3_diag[1])
#define S3_CHM_R   (g_s3_diag[2])
#define S3_RGD_N   (g_s3_diag[3])
#define S3_RGD_R   (g_s3_diag[4])
#define S3_PROBE_A2 (g_s3_diag[5])
#define S3_PROBE_A3 (g_s3_diag[6])
#define S3_PROBE_A4 (g_s3_diag[7])
#define S3_PROBE_B0 (g_s3_diag[8])
#define S3_PROBE_B4 (g_s3_diag[9])
extern void __real_ieee80211_send_probereq(u32, u32, u32, u32, u32, u32, u32, u32);
__attribute__((section(".flash.text")))
void __wrap_ieee80211_send_probereq(u32 a1, u32 a2, u32 a3, u32 a4,
                                    u32 a5, u32 a6, u32 a7, u32 a8)
{
    S3_PROBE_N++;
    /* §§1339：**记录库传进来的 a2** —— 判定是"库传 0"还是
     * "`send_probereq` 内部把它弄丢了"。 */
    S3_PROBE_A2  = a2;
    S3_PROBE_A3  = a3;
    S3_PROBE_A4  = a4;
    if (a2 != 0u) {
        S3_PROBE_B0 = *(volatile u32 *)(void *)a2;
        S3_PROBE_B4 = *(volatile u32 *)(void *)(a2 + 4u);
    }
    __real_ieee80211_send_probereq(a1, a2, a3, a4, a5, a6, a7, a8);
}
extern u32 __real_ic_tx_pkt(u32);
volatile u32 g_s3_txret = 0u;      /* §715: ppTxPkt 返回值（非 0 = TX 被拒） */
volatile u32 g_s3_txarg = 0u;
__attribute__((section(".flash.text")))
void __wrap_ic_tx_pkt(u32 a)
{
    g_s3_diag[5]++;
    g_s3_txarg = a;
    g_s3_txret = __real_ic_tx_pkt(a);
}
extern int __real_ieee80211_regdomain_is_active_scan(u32);
__attribute__((section(".flash.text")))
int __wrap_ieee80211_regdomain_is_active_scan(u32 ch)
{
    S3_RGD_N++;
    int r = __real_ieee80211_regdomain_is_active_scan(ch);
    S3_RGD_R = (u32)r;
    return r;
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
 * 输出走 ROM 的 s3_ets_printf（windowed，本文件也是 windowed，ABI 一致）。
 * 只需要支持很小一部分格式：%s %d %u %x %c %% 和宽度。够读 md5/自检失败。 */

extern int s3_ets_printf(const char *fmt, ...);
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
        s3_ets_printf("[%s] %s\n", tag, buf);
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
