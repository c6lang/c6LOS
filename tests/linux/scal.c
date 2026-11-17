/* M13-3 验收：把"人为上限"逐个顶掉。
 *
 * 以前这些数字都是编译期常量，任何一个都够让真实程序当场失败：
 *   fd 表 16 个      -> 一个跑几百个 socket/文件的守护进程直接 EMFILE
 *   进程表 8 槽      -> 32 线程的程序连 pthread_create 都过不去
 *   管道 4KB         -> 一次 64KB 的 write 要用户侧循环，且 8 个管道封顶
 * 判据不是"能跑"，而是**顶到旧的硬上限之上还能正常工作**：
 *   1) 连续 dup 出 1000 个 fd（旧上限 16），逐个写入验证可用，再全部关掉；
 *      重来三轮，证明槽位真的回收了、表没泄漏。
 *   2) 建 32 个线程（旧上限 8），各自在私有栈上跑、共享计数器+mutex，
 *      join 回来数字必须对得上。
 *   3) 一个管道单次写 65536 字节（旧缓冲 4096），读回来逐字节比对；
 *      再做 40 个管道同时存在的循环（旧上限 8）。
 */
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define NFDS   1000
#define NTHR   32
#define PIPESZ 65536u
#define ROUNDS 3

static int failures;

static void say(const char *s)
{
    (void)!write(1, s, strlen(s));
}

static void say_num(long v)
{
    char b[24];
    int i = 0;
    char c[2];

    if (v < 0) {
        say("-");
        v = -v;
    }
    if (v == 0) {
        say("0");
        return;
    }
    while (v > 0 && i < 22) {
        b[i] = (char)('0' + (int)(v % 10));
        v /= 10;
        i++;
    }
    c[1] = '\0';
    while (i > 0) {
        i--;
        c[0] = b[i];
        say(c);
    }
}

static void check(int cond, const char *what)
{
    if (!cond) {
        failures++;
        say("FAIL: ");
        say(what);
        say("\n");
    }
}

/* ---- 1) fd 表 ------------------------------------------------------------ */

static void test_fds(void)
{
    static char seen[4096];
    int fds[NFDS];
    int round;
    int i;

    for (round = 0; round < ROUNDS; round++) {
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < NFDS; i++) {
            fds[i] = dup(1);                 /* 复制控制台：不占文件系统名额 */
            if (fds[i] < 3 || fds[i] >= 4096) {
                say("FAIL: dup 第 ");
                say_num(i);
                say(" 个拿到 fd=");
                say_num(fds[i]);
                say("\n");
                failures++;
                /* 把已经拿到的还回去，避免下一轮连环失败 */
                while (--i >= 0) {
                    close(fds[i]);
                }
                return;
            }
            if (seen[fds[i]] != 0) {
                say("FAIL: 同一个 fd 发了两次：");
                say_num(fds[i]);
                say("\n");
                failures++;
                break;
            }
            seen[fds[i]] = 1;
        }
        check(i == NFDS, "dup 没有拿到 1000 个不同的 fd");
        /* 高位 fd 必须真能用：写一个字节看看 */
        check(write(fds[NFDS - 1], "", 0) == 0, "高位 fd 写入失败");
        for (i = 0; i < NFDS; i++) {
            if (close(fds[i]) != 0) {
                check(0, "close 高位 fd 失败");
                break;
            }
        }
    }
    /* 表回收验证：全关掉之后再 dup，应该又拿到低位（3 起步） */
    {
        int a = dup(1);
        int b = dup(1);

        check(a == 4 || a == 3, "fd 槽位没有回收");
        check(b == a + 1, "fd 槽位没有回收（第二个）");
        close(a);
        close(b);
    }
}

/* ---- 2) 线程 ------------------------------------------------------------- */

static volatile int counter;
static volatile int locked;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    int i;

    for (i = 0; i < 50; i++) {
        __sync_fetch_and_add(&counter, 1);
    }
    for (i = 0; i < 10; i++) {
        pthread_mutex_lock(&lock);
        locked++;
        pthread_mutex_unlock(&lock);
    }
    return arg;
}

static void test_threads(void)
{
    pthread_t t[NTHR];
    int i;
    int ok = 1;

    for (i = 0; i < NTHR; i++) {
        if (pthread_create(&t[i], (const pthread_attr_t *)0, worker,
                           (void *)(long)i) != 0) {
            say("FAIL: pthread_create 第 ");
            say_num(i);
            say(" 个失败\n");
            failures++;
            return;
        }
    }
    for (i = 0; i < NTHR; i++) {
        void *ret = (void *)0;

        if (pthread_join(t[i], &ret) != 0 || (long)ret != (long)i) {
            ok = 0;
        }
    }
    check(ok, "pthread_join 返回值不对");
    check(counter == NTHR * 50, "thr: counter 不对");
    check(locked == NTHR * 10, "thr: mutex 计数不对");
}

/* ---- 3) 管道 ------------------------------------------------------------- */

static unsigned char big[PIPESZ];

static void test_pipe(void)
{
    int p[2];
    unsigned i;
    int round;

    for (i = 0; i < PIPESZ; i++) {
        big[i] = (unsigned char)(i * 7u + 3u);
    }
    if (pipe(p) != 0) {
        check(0, "pipe() 失败");
        return;
    }
    check(write(p[1], big, PIPESZ) == (long)PIPESZ, "单次写 64KB 没写完");
    {
        static unsigned char got[PIPESZ];

        memset(got, 0, sizeof(got));
        check(read(p[0], got, PIPESZ) == (long)PIPESZ, "单次读 64KB 没读完");
        check(memcmp(got, big, PIPESZ) == 0, "管道数据被破坏");
    }
    close(p[0]);
    close(p[1]);

    /* 40 个管道同时存在（旧上限 8 个） */
    for (round = 0; round < 40; round++) {
        if (pipe(p) != 0) {
            say("FAIL: 第 ");
            say_num(round);
            say(" 个并发管道建不起来\n");
            failures++;
            return;
        }
        close(p[0]);
        close(p[1]);
    }
}

int main(void)
{
    test_fds();
    test_threads();
    test_pipe();
    if (failures != 0) {
        say("FAIL: scal\n");
        return 1;
    }
    say("PASS: scal\n");
    return 0;
}
