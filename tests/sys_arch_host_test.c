/* tests/sys_arch_host_test.c —— 在宿主上验证 port/s3/lwip/sys_arch.c
 *                                  （docs/wifi-plan.md §227）
 *
 * 板子上的 lwIP 用 NO_SYS=0：它有 tcpip 线程、邮箱、信号量、互斥量，全部落在
 * 我们的内核原语上（port/s3/lwip/sys_arch.c 经 s3_w2c_* 壳调用内核）。这一层
 * 是"WiFi 能连上 ≠ lwIP 能跑"的关键，而它不需要 WiFi 硬件 —— 宿主上把
 * s3_w2c_* 用 pthread 实现（port/s3/lwip/host/arch_stub.c）就能原样验证。
 *
 * 覆盖：
 *   1) 邮箱：投递/按序取出/tryfetch；
 *   2) 邮箱超时：空邮箱 fetch(200ms) 必须返回 SYS_ARCH_TIMEOUT 且耗时约 200ms；
 *   3) 跨线程：sys_thread_new 起的线程投递，主线程取出；
 *   4) 信号量：wait 超时 + 由别的线程 signal 后唤醒；
 *   5) 互斥量：lock/unlock；
 *   6) sys_now 单调递增。
 *
 * 跑法：make lwipcheck
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"

static int fails = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("   ok   %s\n", what);
    } else {
        printf("   FAIL %s\n", what);
        fails++;
    }
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000ll + (long long)(ts.tv_nsec / 1000000);
}

/* ---- 跨线程投递/唤醒用 ---- */
static sys_mbox_t g_mbox;
static sys_sem_t g_sem;
static volatile int g_worker_ran = 0;

static void worker(void *arg)
{
    (void)arg;
    g_worker_ran = 1;
    sys_mbox_post(&g_mbox, (void *)0x1234);
    sys_sem_signal(&g_sem);
}

int main(void)
{
    sys_mbox_t mbox = SYS_MBOX_NULL;
    sys_sem_t sem = SYS_MBOX_NULL;
    sys_mutex_t mtx = 0;
    void *msg = 0;
    u32_t r = 0;
    long long t0 = 0;
    u32_t t1 = 0;
    u32_t t2 = 0;

    sys_init();

    printf("== 邮箱：投递 3 条、按序取出、tryfetch\n");
    r = sys_mbox_new(&mbox, 4);
    check(r == ERR_OK && mbox != SYS_MBOX_NULL, "sys_mbox_new");
    sys_mbox_post(&mbox, (void *)1);
    sys_mbox_post(&mbox, (void *)2);
    sys_mbox_post(&mbox, (void *)3);
    r = sys_arch_mbox_fetch(&mbox, &msg, 1000);
    check((int)(size_t)msg == 1, "第一条是 1");
    r = sys_arch_mbox_tryfetch(&mbox, &msg);
    check(r == 0 && (int)(size_t)msg == 2, "tryfetch 拿到 2");
    r = sys_arch_mbox_fetch(&mbox, &msg, 1000);
    check((int)(size_t)msg == 3, "第三条是 3");

    printf("== 邮箱超时：空邮箱 fetch(200ms)\n");
    t0 = now_ms();
    r = sys_arch_mbox_fetch(&mbox, &msg, 200);
    t1 = (u32_t)(now_ms() - t0);
    check(r == SYS_ARCH_TIMEOUT, "返回 SYS_ARCH_TIMEOUT");
    check(t1 >= 150u && t1 <= 900u, "耗时约 200ms（允许调度误差）");
    sys_mbox_free(&mbox);

    printf("== 信号量：wait 超时 / 跨线程 signal\n");
    r = sys_sem_new(&sem, 0);
    check(r == ERR_OK && sem != SYS_SEM_NULL, "sys_sem_new(0)");
    t0 = now_ms();
    r = sys_arch_sem_wait(&sem, 200);
    t1 = (u32_t)(now_ms() - t0);
    check(r == SYS_ARCH_TIMEOUT, "空信号量 wait 超时");
    check(t1 >= 150u && t1 <= 900u, "耗时约 200ms");

    printf("== 跨线程：sys_thread_new 起的线程投递邮箱并 signal\n");
    (void)sys_mbox_new(&g_mbox, 4);
    g_sem = sem;
    check(sys_thread_new("worker", worker, 0, 4096, 2) != 0, "sys_thread_new");
    r = sys_arch_mbox_fetch(&g_mbox, &msg, 2000);
    check((int)(size_t)msg == 0x1234, "从工作线程收到消息");
    r = sys_arch_sem_wait(&g_sem, 2000);
    check(r != SYS_ARCH_TIMEOUT, "被工作线程唤醒");
    check(g_worker_ran == 1, "工作线程确实跑了");
    sys_mbox_free(&g_mbox);
    sys_sem_free(&sem);

    printf("== 互斥量\n");
    r = sys_mutex_new(&mtx);
    check(r == ERR_OK && mtx != 0, "sys_mutex_new");
    sys_mutex_lock(&mtx);
    sys_mutex_unlock(&mtx);
    sys_mutex_lock(&mtx);
    sys_mutex_unlock(&mtx);
    check(1, "lock/unlock 两次没死锁");
    sys_mutex_free(&mtx);

    printf("== sys_now 单调递增\n");
    t1 = sys_now();
    sys_msleep(30);
    t2 = sys_now();
    check(t2 >= t1, "sys_now 不回退");
    check((t2 - t1) >= 20u && (t2 - t1) <= 500u, "sys_msleep(30) 后 sys_now 前进约 30ms");

    (void)t0;
    printf("%s（失败 %d 项）\n", fails == 0 ? "sys_arch 自测通过" : "自测失败", fails);
    return fails == 0 ? 0 : 1;
}
