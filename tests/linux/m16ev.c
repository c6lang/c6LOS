/* M16 验收：事件与时间 syscall。宿主 gcc 普通 Linux 程序，内核零专用接口。
 *   1. select：管道可读 → select 返回且 rset 置位；无数据+超时 → 返回 0
 *   2. pselect6：同 select 的 timespec 超时路径
 *   3. eventfd2：write 计数 → read 拿到值并清零；select 看到 POLLIN
 *   4. gettimeofday/clock_getres：字段合理，不 ENOSYS
 * 用例不依赖 sleep，抢占下确定。 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void say(const char *s)
{
    (void)!write(1, s, strlen(s));
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

int main(void)
{
    int p[2];
    fd_set rfds;
    struct timeval tv;
    struct timespec ts;
    int efd;
    unsigned long long v;
    struct timeval now;
    struct timespec res;

    /* ---- 1. select：超时返回 0 ---- */
    check(pipe(p) == 0, "pipe");
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    check(select(p[0] + 1, &rfds, 0, 0, &tv) == 0, "select timeout");

    /* ---- 2. select：有数据可读 → 返回 1 且位置位 ---- */
    check(write(p[1], "x", 1) == 1, "pipe write");
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    check(select(p[0] + 1, &rfds, 0, 0, &tv) == 1, "select ready");
    check(FD_ISSET(p[0], &rfds) != 0, "select rset bit");
    {
        char c = 0;
        check(read(p[0], &c, 1) == 1, "drain pipe");
    }
    close(p[0]);
    close(p[1]);

    /* ---- 3. pselect6：超时路径 ---- */
    FD_ZERO(&rfds);
    tv.tv_sec = 0;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    check(pselect(0, &rfds, 0, 0, &ts, 0) == 0, "pselect6 timeout");

    /* ---- 4. eventfd2：write → read → 清零 ---- */
    efd = eventfd(0, 0);
    check(efd >= 0, "eventfd2 create");
    if (efd >= 0) {
        v = 5;
        check(write(efd, &v, 8) == 8, "eventfd write");
        FD_ZERO(&rfds);
        FD_SET(efd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        check(select(efd + 1, &rfds, 0, 0, &tv) == 1, "eventfd select POLLIN");
        v = 0;
        check(read(efd, &v, 8) == 8 && v == 5, "eventfd read value");
        check(read(efd, &v, 8) == 8 && v == 0, "eventfd read cleared");
        close(efd);
    }

    /* ---- 5. 时间 syscall ---- */
    check(gettimeofday(&now, 0) == 0, "gettimeofday");
    check(now.tv_sec >= 1700000000ull && now.tv_usec >= 0, "gettimeofday fields");
    check(clock_getres(CLOCK_MONOTONIC, &res) == 0, "clock_getres");
    check(res.tv_sec == 0, "clock_getres nsec precision");

    if (failures == 0) {
        say("PASS: m16ev\n");
        return 0;
    }
    say("FAIL: m16ev\n");
    return 1;
}
