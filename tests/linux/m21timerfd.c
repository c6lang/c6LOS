/* M21 验收：timerfd_create/settime/gettime + read/read-poll 语义。
 * 普通 glibc 程序，全部用真实 syscall：
 *   1) 单次 timer：settime 30ms，read 阻塞到点返回 1，再 read 又阻塞
 *   2) poll(EPOLLIN) 在到期前返回 0、到期后返回 1
 *   3) 周期 timer：读到的计数值 >=2，gettime 剩余时间合法
 *   4) settime 清零停止 timer
 *   5) gettime 返回 it_interval 与设定一致
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

/* ms → itimerspec */
static void spec_ms(struct itimerspec *s, long ms, long interval_ms)
{
    memset(s, 0, sizeof(*s));
    s->it_value.tv_nsec = (ms % 1000) * 1000000L;
    s->it_value.tv_sec = ms / 1000;
    s->it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;
    s->it_interval.tv_sec = interval_ms / 1000;
}

int main(void)
{
    int fd;
    struct itimerspec s, cur;
    unsigned long long v;
    struct pollfd pfd;
    int n;

    fd = timerfd_create(CLOCK_MONOTONIC, 0);
    CHECK(fd >= 0);
    if (fd < 0) { goto done; }

    /* 1. 单次 timer：settime 后 read 阻塞到点 */
    spec_ms(&s, 30, 0);
    CHECK(timerfd_settime(fd, 0, &s, 0) == 0);
    CHECK(read(fd, &v, 8) == 8);
    CHECK(v == 1);                              /* 到期一次 */
    CHECK(timerfd_gettime(fd, &cur) == 0);
    CHECK(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);  /* 停表 */

    /* 2. poll：未 armed 时 0；armed 且未到期时 0；到期后 1 */
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 0) == 0);                /* 未 armed */
    spec_ms(&s, 30, 0);
    CHECK(timerfd_settime(fd, 0, &s, 0) == 0);
    CHECK(poll(&pfd, 1, 0) == 0);                /* armed 但未到点 */
    CHECK(poll(&pfd, 1, 1000) == 1);             /* 到点后可读 */
    CHECK((pfd.revents & POLLIN) != 0);
    CHECK(read(fd, &v, 8) == 8 && v == 1);

    /* 3. 周期 timer：10ms 间隔，累计 >=2 次 */
    spec_ms(&s, 10, 10);
    CHECK(timerfd_settime(fd, 0, &s, 0) == 0);
    CHECK(timerfd_gettime(fd, &cur) == 0);
    CHECK(cur.it_interval.tv_nsec == 10000000L); /* 周期回读 */
    n = 0;
    while (n < 2) {
        CHECK(read(fd, &v, 8) == 8);
        n += (int)v;
    }
    CHECK(n >= 2);
    CHECK(timerfd_gettime(fd, &cur) == 0);
    CHECK(cur.it_value.tv_nsec > 0 || cur.it_value.tv_sec > 0); /* 仍 armed */

    /* 4. 清零停止：之后 poll 立刻 0 */
    spec_ms(&s, 0, 0);
    CHECK(timerfd_settime(fd, 0, &s, 0) == 0);
    CHECK(poll(&pfd, 1, 0) == 0);
    CHECK(timerfd_gettime(fd, &cur) == 0);
    CHECK(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    CHECK(close(fd) == 0);

done:
    if (failures == 0) {
        dprintf(1, "PASS: m21timerfd (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m21timerfd (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
