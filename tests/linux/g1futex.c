/* G1-2（TDD 先红）：futex 必须按 (地址空间, 用户地址) 匹配唤醒。
 * 旧实现 WAKE 唤醒全部 BLOCK，两个独立地址上的等待者会一起醒。 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int a;
static int b;
static volatile int a_woke;
static volatile int b_woke;
static volatile int a_ready;
static volatile int b_ready;

static int futex_wait(int *p, int v)
{
    return (int)syscall(SYS_futex, p, FUTEX_WAIT, v, 0, 0, 0);
}

static int futex_wake(int *p, int n)
{
    return (int)syscall(SYS_futex, p, FUTEX_WAKE, n, 0, 0, 0);
}

static void *waiter_a(void *arg)
{
    (void)arg;
    a_ready = 1;
    if (futex_wait(&a, 0) == 0) {
        a_woke = 1;
    }
    return NULL;
}

static void *waiter_b(void *arg)
{
    (void)arg;
    b_ready = 1;
    if (futex_wait(&b, 0) == 0) {
        b_woke = 1;
    }
    return NULL;
}

int main(void)
{
    pthread_t ta;
    pthread_t tb;
    int failures = 0;
    int total = 0;
    int i;

    if (pthread_create(&ta, NULL, waiter_a, NULL) != 0 ||
        pthread_create(&tb, NULL, waiter_b, NULL) != 0) {
        dprintf(1, "FAIL: pthread_create\n");
        return 1;
    }
    for (i = 0; i < 100 && (!a_ready || !b_ready); i++) {
        sched_yield();
    }
    total += 2;
    if (!a_ready || !b_ready) {
        failures++;
        dprintf(1, "FAIL: waiters not started a=%d b=%d\n", a_ready, b_ready);
    }
    a = 1;
    total++;
    if (futex_wake(&a, 1) != 1) {
        failures++;
        dprintf(1, "FAIL: wake(a) != 1\n");
    }
    /* 主线程阻塞在 join 上，调度器才会把 a 跑完；否则主线程一直 RUN，
     * b 的阻塞路径测不出来。 */
    pthread_join(ta, NULL);
    total += 2;
    if (!a_woke) {
        failures++;
        dprintf(1, "FAIL: waiter a did not wake\n");
    }
    if (b_woke) {
        failures++;
        dprintf(1, "FAIL: waiter b woke on address a\n");
    }
    b = 1;
    total++;
    if (futex_wake(&b, 1) != 1) {
        failures++;
        dprintf(1, "FAIL: wake(b) != 1\n");
    }
    pthread_join(tb, NULL);
    total++;
    if (!b_woke) {
        failures++;
        dprintf(1, "FAIL: waiter b did not wake\n");
    }

    if (failures == 0) {
        dprintf(1, "PASS: g1futex (%d/%d checks)\n", total, total);
        return 0;
    }
    dprintf(1, "FAIL: g1futex (%d/%d checks)\n", failures, total);
    return 1;
}