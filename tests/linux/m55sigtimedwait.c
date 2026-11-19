/* M55 验收：rt_sigtimedwait(128) 信号定时等待
 * 1) 零超时无 pending 信号 → EAGAIN
 * 2) 先 kill 自己 SIGUSR1（阻塞），再 rt_sigtimedwait 取到 → 返回 SIGUSR1
 * 3) 非零超时 + 子进程定时发信号 → 取到对应信号
 * 4) siginfo.si_signo 正确 */
#define _GNU_SOURCE
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int fails;
#define CHECK(x) do { if (!(x)) { \
    char b[128]; int n = __builtin_snprintf(b, sizeof b, \
        "FAIL: %s (line %d, errno=%d)\n", #x, __LINE__, errno); \
    (void)!write(2, b, n); fails++; } } while (0)

static long stwait(const sigset_t *set, siginfo_t *info,
                   const struct timespec *to)
{
    return syscall(128, set, info, to, (size_t)8);
}

int main(void)
{
    sigset_t set, blocked;
    siginfo_t info;
    struct timespec zero = {0, 0};
    struct timespec short_to = {0, 20000000}; /* 20ms */
    long r;

    /* 等待前先阻塞这两个信号，使 kill 产生的信号挂起而非终止进程。
     * rt_sigtimedwait 会在调用期间自动屏蔽等待集，这里必须提前阻塞。 */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &blocked, 0) != 0) return 89;

    /* 1) 空等待 */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    errno = 0;
    r = stwait(&set, &info, &zero);
    CHECK(r == -1 && errno == EAGAIN);

    /* 2) pending 命中 */
    if (kill(getpid(), SIGUSR1) != 0) return 90;
    errno = 0;
    r = stwait(&set, &info, &zero);
    CHECK(r == SIGUSR1);
    CHECK(info.si_signo == SIGUSR1);

    /* 3) 带超时等待，子进程 10ms 后发 SIGUSR2 */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    {
        pid_t kid = fork();
        if (kid == 0) {
            struct timespec s = {0, 10000000};
            nanosleep(&s, 0);
            kill(getppid(), SIGUSR2);
            _exit(0);
        }
        CHECK(kid > 0);
        errno = 0;
        r = stwait(&set, &info, &short_to);
        CHECK(r == SIGUSR2);
        CHECK(info.si_signo == SIGUSR2);
        /* 回收子进程 */
        { int st; waitpid(kid, &st, 0); }
    }

    if (fails == 0) {
        const char *s = "PASS: m55sigtimedwait (8/8 checks)\n";
        (void)!write(1, s, 36);
        return 0;
    }
    {
        char b[64];
        int n = __builtin_snprintf(b, sizeof b, "FAIL: m55sigtimedwait (%d/8)\n", fails);
        (void)!write(2, b, n);
    }
    return 1;
}
