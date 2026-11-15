/* 宿主版的 sys_arch 原语：把 s3_w2c_* 这套接口用 pthread 实现出来，
 * 于是 port/s3/lwip/sys_arch.c（板子上跑的那份）可以**原样**在宿主上跑，
 * 配合 tests/lwip_host_test.c 验证 sys_arch + lwIP 线程模型（docs §227）。
 *
 * 语义对照（和板子上一致）：
 *   - timeout 单位是**内核 tick**（板子上 100Hz = 10ms），这里按 10ms 折算；
 *   - timeout = 0 表示永久等待；
 *   - 返回值：0 = 成功，非 0 = 超时/失败（sys_arch.c 就是这么判的）。
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

#define STUB_TICK_MS 10

static unsigned long g_ticks = 0;      /* 用时间模拟 tick */

unsigned int s3_w2c_tick_get(void)
{
    struct timespec ts;
    unsigned long long ms;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ms = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
    g_ticks = (unsigned long)(ms / STUB_TICK_MS);
    return (unsigned int)g_ticks;
}

unsigned int s3_w2c_tick_hz(void) { return 1000u / STUB_TICK_MS; }

/* ---- 通用：mutex + cond 实现的"可超时等待" ---- */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int count;
    int max;
    int used;
} StubSem;

static int wait_ms(int ticks)
{
    return (ticks == 0) ? -1 : ticks * STUB_TICK_MS;
}

int s3_w2c_sem_init(void *s, unsigned int initial, unsigned int max)
{
    StubSem *p = (StubSem *)s;

    pthread_mutex_init(&p->m, 0);
    pthread_cond_init(&p->c, 0);
    p->count = (int)initial;
    p->max = (int)max;
    p->used = 1;
    return 0;
}

int s3_w2c_sem_destroy(void *s)
{
    StubSem *p = (StubSem *)s;

    pthread_mutex_destroy(&p->m);
    pthread_cond_destroy(&p->c);
    p->used = 0;
    return 0;
}

int s3_w2c_sem_pend(void *s, unsigned int ticks)
{
    StubSem *p = (StubSem *)s;
    struct timespec ts;
    int ms = wait_ms((int)ticks);

    pthread_mutex_lock(&p->m);
    while (p->count <= 0) {
        if (ms < 0) {
            pthread_cond_wait(&p->c, &p->m);
        } else {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&p->c, &p->m, &ts) == ETIMEDOUT && p->count <= 0) {
                pthread_mutex_unlock(&p->m);
                return -1;
            }
        }
    }
    p->count = p->count - 1;
    pthread_mutex_unlock(&p->m);
    return 0;
}

int s3_w2c_sem_post(void *s)
{
    StubSem *p = (StubSem *)s;

    pthread_mutex_lock(&p->m);
    if (p->count < p->max) { p->count = p->count + 1; }
    pthread_cond_signal(&p->c);
    pthread_mutex_unlock(&p->m);
    return 0;
}

/* 互斥（内核是递归所有者互斥；这里用递归互斥模拟） */
int s3_w2c_mutex_init(void *m)
{
    pthread_mutex_t *p = (pthread_mutex_t *)m;
    pthread_mutexattr_t a;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(p, &a);
    pthread_mutexattr_destroy(&a);
    return 0;
}

int s3_w2c_mutex_destroy(void *m) { pthread_mutex_destroy((pthread_mutex_t *)m); return 0; }
int s3_w2c_mutex_pend(void *m, unsigned int ticks) { (void)ticks; return pthread_mutex_lock((pthread_mutex_t *)m); }
int s3_w2c_mutex_post(void *m) { return pthread_mutex_unlock((pthread_mutex_t *)m); }

/* 队列：容量固定、元素 4 字节（和板子上的用法一致：邮箱发指针） */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t notfull;
    pthread_cond_t notempty;
    unsigned char *buf;
    int cap;
    int item;
    int head;
    int count;
    int used;
} StubQ;

int s3_w2c_queue_init(void *q, void *storage, unsigned int cap, unsigned int item_size)
{
    StubQ *p = (StubQ *)q;

    pthread_mutex_init(&p->m, 0);
    pthread_cond_init(&p->notfull, 0);
    pthread_cond_init(&p->notempty, 0);
    p->buf = (unsigned char *)storage;
    p->cap = (int)cap;
    p->item = (int)item_size;
    p->head = 0;
    p->count = 0;
    p->used = 1;
    return 0;
}

int s3_w2c_queue_destroy(void *q) { (void)q; return 0; }

static int q_put(StubQ *p, const void *item, int wait_forever, int ms)
{
    struct timespec ts;

    pthread_mutex_lock(&p->m);
    while (p->count >= p->cap) {
        if (wait_forever) {
            pthread_cond_wait(&p->notfull, &p->m);
        } else {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&p->notfull, &p->m, &ts) == ETIMEDOUT && p->count >= p->cap) {
                pthread_mutex_unlock(&p->m);
                return -1;
            }
        }
    }
    memcpy(p->buf + ((p->head + p->count) % p->cap) * p->item, item, (size_t)p->item);
    p->count = p->count + 1;
    pthread_cond_signal(&p->notempty);
    pthread_mutex_unlock(&p->m);
    return 0;
}

static int q_get(StubQ *p, void *out, int wait_forever, int ms)
{
    struct timespec ts;

    pthread_mutex_lock(&p->m);
    while (p->count <= 0) {
        if (wait_forever) {
            pthread_cond_wait(&p->notempty, &p->m);
        } else {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&p->notempty, &p->m, &ts) == ETIMEDOUT && p->count <= 0) {
                pthread_mutex_unlock(&p->m);
                return -1;
            }
        }
    }
    memcpy(out, p->buf + p->head * p->item, (size_t)p->item);
    p->head = (p->head + 1) % p->cap;
    p->count = p->count - 1;
    pthread_cond_signal(&p->notfull);
    pthread_mutex_unlock(&p->m);
    return 0;
}

int s3_w2c_queue_put(void *q, const void *item, unsigned int ticks)
{
    return q_put((StubQ *)q, item, ticks == 0u, wait_ms((int)ticks));
}

int s3_w2c_queue_get(void *q, void *out, unsigned int ticks)
{
    return q_get((StubQ *)q, out, ticks == 0u, wait_ms((int)ticks));
}

int s3_w2c_queue_try_put(void *q, const void *item) { return q_put((StubQ *)q, item, 0, 0); }
int s3_w2c_queue_try_get(void *q, void *out) { return q_get((StubQ *)q, out, 0, 0); }
unsigned int s3_w2c_queue_count(void *q) { return (unsigned int)((StubQ *)q)->count; }

/* 任务：pthread 里跑 entry(arg) */
typedef struct {
    void (*entry)(void *);
    void *arg;
} StubTask;

static void *task_tramp(void *p)
{
    StubTask t = *(StubTask *)p;

    free(p);
    t.entry(t.arg);
    return 0;
}

int s3_w2c_task_create(const char *name, void *entry, void *arg,
                       unsigned int prio, unsigned int stack_size)
{
    pthread_attr_t a;
    pthread_t th;
    StubTask *t = (StubTask *)malloc(sizeof(StubTask));

    (void)name; (void)prio; (void)stack_size;
    t->entry = (void (*)(void *))entry;
    t->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256u * 1024u);
    pthread_create(&th, &a, task_tramp, t);
    pthread_detach(th);
    pthread_attr_destroy(&a);
    return 1;
}

void s3_w2c_task_delay(unsigned int ticks)
{
    struct timespec ts;

    ts.tv_sec = (time_t)((ticks * STUB_TICK_MS) / 1000);
    ts.tv_nsec = (long)(((ticks * STUB_TICK_MS) % 1000) * 1000000L);
    nanosleep(&ts, 0);
}

void s3_w2c_task_yield(void) { sched_yield(); }
unsigned int s3_w2c_current_task(void) { return (unsigned int)(unsigned long)pthread_self(); }

/* sys_arch.c 里用到的两个外部符号，板子上分别来自 c6_lwip_port.c（sys_now）
 * 和 ctx.c（g_next_task_windowed）。宿主测试里补上。 */
unsigned int sys_now(void)
{
    return s3_w2c_tick_get() * STUB_TICK_MS;
}

unsigned int g_next_task_windowed = 0u;

/* arch/cc.h 里的 LWIP_PLATFORM_DIAG/ASSERT 钩子（板子上在 c6_lwip_port.c） */
#include <stdarg.h>

void c6lwip_diag(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void c6lwip_assert(const char *msg)
{
    fprintf(stderr, "lwip assert: %s\n", msg);
    abort();
}

unsigned int c6lwip_rand(void)
{
    static unsigned int s = 0x12345678u;

    s = s * 1103515245u + 12345u;
    return s >> 8;
}
