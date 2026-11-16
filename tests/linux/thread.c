/* L7 验收：线程。
 *
 * 未修改的 pthread 程序：4 个线程各自在**自己的栈**上跑、各自有 TLS，
 * 并发自增同一个全局变量，再由 pthread_join（futex + clear_child_tid）
 * 收口；外加一把 mutex 验证 FUTEX_WAIT/WAKE。
 */
#include <pthread.h>
#include <unistd.h>

static volatile int counter = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int locked_counter = 0;

static void say(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0') {
        n++;
    }
    write(1, s, (unsigned)n);
}

static void *worker(void *arg)
{
    int i;

    for (i = 0; i < 100; i++) {
        __sync_fetch_and_add(&counter, 1);   /* 抢占后普通 ++ 会丢更新 */
    }
    for (i = 0; i < 20; i++) {
        pthread_mutex_lock(&lock);
        locked_counter++;
        pthread_mutex_unlock(&lock);
    }
    return arg;
}

int main(void)
{
    pthread_t t[4];
    long i;
    int ok = 1;

    for (i = 0; i < 4; i++) {
        if (pthread_create(&t[i], (const pthread_attr_t *)0, worker,
                           (void *)i) != 0) {
            say("thr: pthread_create FAIL\n");
            return 1;
        }
    }
    for (i = 0; i < 4; i++) {
        void *ret = (void *)0;

        if (pthread_join(t[i], &ret) != 0 || (long)ret != i) {
            ok = 0;
        }
    }
    if (counter != 400 || locked_counter != 80) {
        ok = 0;
    }
    say(ok ? "thr: 4 threads joined, counter=400, mutex=80\n"
           : "thr: FAIL\n");
    say("thr: done\n");
    return 0;
}
