/* c6LOS 的 lwIP sys_arch 实现（NO_SYS=0）
 *
 * 全部落在内核原语上，经 port/s3/w2c.S 的 windowed 壳调用：
 *   sys_sem   → os_sem_init / os_sem_pend / os_sem_post
 *   sys_mutex → os_mutex_init / os_mutex_pend / os_mutex_post
 *   sys_mbox  → os_queue_init_ex / os_queue_get_item / os_queue_try_put_item
 *   sys_thread_new → os_task_create（任务入口是 windowed 代码，所以要带
 *                    "下一个任务用 windowed ABI" 的标记，见 g_next_task_windowed）
 *
 * 为什么用**静态池**而不是 malloc：句柄要能长期有效、且要在
 * "内核堆还没准备好"的极端时序下也不炸。数量由 lwIP 的配置上界决定
 * （MEMP_NUM_NETCONN=4 时，tcpip 线程 1 个 mbox + 每个 socket 一个 mbox，
 *  再算上 tcpip 内部的几个信号量/互斥量，16/8/8 绰绰有余）。
 */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "arch/sys_arch.h"

#if !NO_SYS

/* ---- 内核原语（windowed 壳，见 port/s3/w2c.S）---------------------------- */
extern int  s3_w2c_sem_init(void *s, u32_t initial, u32_t max);
extern int  s3_w2c_sem_destroy(void *s);
extern int  s3_w2c_sem_pend(void *s, u32_t timeout);
extern int  s3_w2c_sem_post(void *s);
extern int  s3_w2c_mutex_init(void *m);
extern int  s3_w2c_mutex_destroy(void *m);
extern int  s3_w2c_mutex_pend(void *m, u32_t timeout);
extern int  s3_w2c_mutex_post(void *m);
extern int  s3_w2c_queue_init(void *q, void *storage, u32_t cap, u32_t item_size);
extern int  s3_w2c_queue_put(void *q, const void *item, u32_t timeout);
extern int  s3_w2c_queue_get(void *q, void *out, u32_t timeout);
extern int  s3_w2c_queue_try_put(void *q, const void *item);
extern int  s3_w2c_queue_try_get(void *q, void *out);
extern int  s3_w2c_queue_destroy(void *q);
extern u32_t  s3_w2c_tick_get(void);
extern u32_t  s3_w2c_tick_hz(void);

/* 内核对象的内存布局（和 include/c6os/{sem,mutex,queue}.h 一致）。
 * 这里不 include 那些头：它们拉进来的 types.h 是内核侧的头，windowed 侧
 * 只需要尺寸和"指针"语义，重复声明反而更安全（同一个 ABI 下 C 结构体布局一致）。 */
#define C6_SEM_BYTES   16u    /* OsSem:  count/max/magic/waiters */
#define C6_MUTEX_BYTES 12u    /* OsMutex: owner/count/magic */
#define C6_QUEUE_BYTES 40u    /* OsQueue: buf/item_size/cap/head/tail/count/magic/waiters_get/waiters_put */

#define C6_LWIP_MAX_SEM   16
#define C6_LWIP_MAX_MUTEX  8
#define C6_LWIP_MAX_MBOX   8
#define C6_LWIP_MBOX_SLOTS 16          /* 每个邮箱最多排 16 个指针（lwIP mbox 语义是"至少这么多"，不要求精确） */

struct c6_lwip_sem {
    u32_t  obj[C6_SEM_BYTES / 4u];
    u8_t   used;
};

struct c6_lwip_mutex {
    u32_t  obj[C6_MUTEX_BYTES / 4u];
    u8_t   used;
};

struct c6_lwip_mbox {
    u32_t  obj[C6_QUEUE_BYTES / 4u];
    void  *slots[C6_LWIP_MBOX_SLOTS];
    u8_t   used;
};

static struct c6_lwip_sem   s_sems[C6_LWIP_MAX_SEM];
static struct c6_lwip_mutex s_mutexes[C6_LWIP_MAX_MUTEX];
static struct c6_lwip_mbox  s_mboxes[C6_LWIP_MAX_MBOX];

/* =0 时表示"无限等待"（内核里 timeout 0 就是永久） */
#define C6_WAIT_FOREVER 0u

void sys_init(void)
{
    /* 静态池，什么都不用做 */
}

/* ---- 信号量 ------------------------------------------------------------- */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    int i;

    for (i = 0; i < C6_LWIP_MAX_SEM; i++) {
        if (s_sems[i].used == 0u) {
            if (s3_w2c_sem_init(s_sems[i].obj, (u32_t)count, 0xFFFFu) != 0) {
                return ERR_MEM;
            }
            s_sems[i].used = 1u;
            *sem = &s_sems[i];
            return ERR_OK;
        }
    }
    return ERR_MEM;      /* 池满：比 malloc 失败更好定位 */
}

void sys_sem_free(sys_sem_t *sem)
{
    if (sem != 0 && *sem != 0) {
        (void)s3_w2c_sem_destroy((*sem)->obj);
        (*sem)->used = 0u;
        *sem = SYS_SEM_NULL;
    }
}

void sys_sem_signal(sys_sem_t *sem)
{
    if (sem != 0 && *sem != 0) {
        (void)s3_w2c_sem_post((*sem)->obj);
    }
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    u32_t before;
    u32_t ticks;

    if (sem == 0 || *sem == 0) {
        return SYS_ARCH_TIMEOUT;
    }
    before = sys_now();
    /* lwIP 给的是毫秒，内核要 tick：0 在两边都表示"永久" */
    ticks = timeout;
    if (timeout != 0u) {
        u32_t hz = s3_w2c_tick_hz();

        ticks = (hz == 0u) ? timeout : ((timeout * hz) / 1000u + 1u);
    }
    if (s3_w2c_sem_pend((*sem)->obj, ticks) != 0) {
        return SYS_ARCH_TIMEOUT;
    }
    return sys_now() - before;
}

/* ---- 互斥量 ------------------------------------------------------------- */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    int i;

    for (i = 0; i < C6_LWIP_MAX_MUTEX; i++) {
        if (s_mutexes[i].used == 0u) {
            if (s3_w2c_mutex_init(s_mutexes[i].obj) != 0) {
                return ERR_MEM;
            }
            s_mutexes[i].used = 1u;
            *mutex = &s_mutexes[i];
            return ERR_OK;
        }
    }
    return ERR_MEM;
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (mutex != 0 && *mutex != 0) {
        (void)s3_w2c_mutex_destroy((*mutex)->obj);
        (*mutex)->used = 0u;
        *mutex = 0;
    }
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    if (mutex != 0 && *mutex != 0) {
        (void)s3_w2c_mutex_pend((*mutex)->obj, C6_WAIT_FOREVER);
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if (mutex != 0 && *mutex != 0) {
        (void)s3_w2c_mutex_post((*mutex)->obj);
    }
}

/* ---- 邮箱（= 内核队列，元素是指针）------------------------------------- */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    int i;

    (void)size;      /* 统一用 C6_LWIP_MBOX_SLOTS，容量只会更大 */
    for (i = 0; i < C6_LWIP_MAX_MBOX; i++) {
        if (s_mboxes[i].used == 0u) {
            if (s3_w2c_queue_init(s_mboxes[i].obj, s_mboxes[i].slots,
                                  C6_LWIP_MBOX_SLOTS, 4u) != 0) {
                return ERR_MEM;
            }
            s_mboxes[i].used = 1u;
            *mbox = &s_mboxes[i];
            return ERR_OK;
        }
    }
    return ERR_MEM;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (mbox != 0 && *mbox != 0) {
        (void)s3_w2c_queue_destroy((*mbox)->obj);
        (*mbox)->used = 0u;
        *mbox = SYS_MBOX_NULL;
    }
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if (mbox != 0 && *mbox != 0) {
        (void)s3_w2c_queue_put((*mbox)->obj, &msg, C6_WAIT_FOREVER);
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (mbox == 0 || *mbox == 0) {
        return ERR_MEM;
    }
    return (s3_w2c_queue_try_put((*mbox)->obj, &msg) == 0) ? ERR_OK : ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    /* 单核 + 我们的队列在中断里也能 try_put（不睡眠），所以直接复用 */
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    u32_t before;
    u32_t ticks;
    void *got = 0;

    if (mbox == 0 || *mbox == 0) {
        return SYS_ARCH_TIMEOUT;
    }
    before = sys_now();
    ticks = timeout;
    if (timeout != 0u) {
        u32_t hz = s3_w2c_tick_hz();

        ticks = (hz == 0u) ? timeout : ((timeout * hz) / 1000u + 1u);
    }
    if (s3_w2c_queue_get((*mbox)->obj, &got, ticks) != 0) {
        return SYS_ARCH_TIMEOUT;
    }
    if (msg != 0) {
        *msg = got;
    }
    return sys_now() - before;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    void *got = 0;

    if (mbox == 0 || *mbox == 0) {
        return SYS_ARCH_TIMEOUT;
    }
    if (s3_w2c_queue_try_get((*mbox)->obj, &got) != 0) {
        return SYS_ARCH_TIMEOUT;
    }
    if (msg != 0) {
        *msg = got;
    }
    return 0u;
}

/* ---- 线程 --------------------------------------------------------------- */

/* 任务入口的中间层：lwIP 的线程函数是 void (*)(void *)，保持原样转过去。
 * 内核的任务入口类型是 void (*)(void *)（OsTaskEntry），所以可以直接转。 */
extern int s3_w2c_task_create(const char *name, void *entry, void *arg,
                              u32_t prio, u32_t stack_size);
/* g_next_task_windowed：下一个创建的任务是否用 windowed ABI（见
 * port/s3/wifi_glue.c）。lwIP 的线程体是 windowed 编译的，必须置 1。 */
extern volatile u32_t g_next_task_windowed;

extern volatile unsigned int g_lwip_stage[8];

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio)
{
    u32_t saved = g_next_task_windowed;

    g_lwip_stage[2] = (unsigned int)(void *)thread;   /* 线程入口 */
    g_lwip_stage[3] = (unsigned int)(void *)arg;      /* 线程参数 */

    /* §841: tcpip 线程栈：4096 窗口溢出，16384 把 40960 栈池占满导致
     * os_task_create TRAP。取 8192（24576+8192=32768，留 8192 余量）。 */
    if (stacksize < 8192) {
        stacksize = 8192;
    }

    g_next_task_windowed = 1u;
    g_lwip_stage[4] = 0x541u;
    (void)s3_w2c_task_create(name, (void *)thread, arg,
                             (u32_t)prio, (u32_t)stacksize);
    g_lwip_stage[4] = 0x542u;
    g_next_task_windowed = saved;
    return 1;
}

#endif /* !NO_SYS */
