/* M22 验收：signalfd4 + read 128B signalfd_siginfo。
 * 普通 glibc 程序：
 *   1) signalfd(-1, mask{SIGUSR1}) 新建 fd
 *   2) sigprocmask 阻塞 SIGUSR1，kill(self, SIGUSR1) → read 得到 signo/errno
 *   3) NONBLOCK 无信号时 read → EAGAIN
 *   4) poll 在信号到达后报 POLLIN
 *   5) mask 不含的信号（SIGUSR2）走正常 handler（不被 signalfd 吞）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

static int failures;
static int total;
static volatile int got_usr2;

static void on_usr2(int s) { (void)s; got_usr2 = 1; }

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    sigset_t mask, old;
    struct signalfd_siginfo sfi;
    struct pollfd pfd;
    int sfd;
    ssize_t n;

    /* SIGUSR2 装 handler（验证 mask 外的信号不被吞） */
    signal(SIGUSR2, on_usr2);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &mask, &old) == 0);

    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    CHECK(sfd >= 0);
    if (sfd < 0) { goto done; }

    /* 1. NONBLOCK 空队列 → EAGAIN */
    errno = 0;
    CHECK(read(sfd, &sfi, sizeof(sfi)) == -1 && errno == EAGAIN);

    /* 2. kill 自己 SIGUSR1 → read 到 128B siginfo */
    CHECK(kill(getpid(), SIGUSR1) == 0);
    memset(&sfi, 0, sizeof(sfi));
    n = read(sfd, &sfi, sizeof(sfi));
    CHECK(n == (ssize_t)sizeof(sfi));
    CHECK(sfi.ssi_signo == SIGUSR1);
    CHECK(sfi.ssi_errno == 0);
    CHECK(sfi.ssi_code == SI_USER);

    /* 再读一次应 EAGAIN */
    errno = 0;
    CHECK(read(sfd, &sfi, sizeof(sfi)) == -1 && errno == EAGAIN);

    /* 3. poll：信号到达后报 POLLIN */
    CHECK(kill(getpid(), SIGUSR1) == 0);
    pfd.fd = sfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 1000) == 1);
    CHECK((pfd.revents & POLLIN) != 0);
    CHECK(read(sfd, &sfi, sizeof(sfi)) == (ssize_t)sizeof(sfi));
    CHECK(sfi.ssi_signo == SIGUSR1);

    /* 4. mask 外信号 SIGUSR2 → 走 handler，不被吞 */
    got_usr2 = 0;
    CHECK(kill(getpid(), SIGUSR2) == 0);
    /* 给 handler 一次运行机会：poll 超时空转 */
    CHECK(poll(0, 0, 10) == 0);
    CHECK(got_usr2 == 1);
    errno = 0;
    CHECK(read(sfd, &sfi, sizeof(sfi)) == -1 && errno == EAGAIN);

    CHECK(close(sfd) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &old, 0) == 0);

done:
    if (failures == 0) {
        dprintf(1, "PASS: m22signalfd (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m22signalfd (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
