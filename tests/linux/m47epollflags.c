/* M47 验收：epoll EPOLLET / EPOLLONESHOT
 *  ET：注册 pipe 读端 EPOLLIN|EPOLLET，写数据 → 报 1 次；
 *     不读不写再 wait(0) → 0 次（边沿不重复）；读空 + 再写 → 又报 1 次。
 *  ONESHOT：EPOLLIN|EPOLLONESHOT，写数据 → 报 1 次；wait(0) → 0；
 *     EPOLL_CTL_MOD 重武装 + 再写 → 又报 1 次。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    int ep, p[2];
    struct epoll_event ev, out[4];
    int n;
    char buf[8];

    ep = epoll_create1(0);
    CHECK(ep >= 0);
    CHECK(pipe(p) == 0);

    /* ---- ET ---- */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u64 = 0xE7ull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(write(p[1], "a", 1) == 1);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    CHECK(n == 1 && out[0].data.u64 == 0xE7ull);
    /* 没有新数据：ET 不重复上报 */
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    /* 读空后重新写：新边沿 */
    CHECK(read(p[0], buf, 1) == 1);
    CHECK(write(p[1], "b", 1) == 1);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);

    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) == 0);
    /* 把管道读空 */
    CHECK(read(p[0], buf, 1) == 1);

    /* ---- ONESHOT ---- */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.u64 = 0x1ull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(write(p[1], "c", 1) == 1);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    CHECK(n == 1 && out[0].data.u64 == 0x1ull);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);       /* 已 fired，不再报 */
    /* MOD 重新武装 */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.u64 = 0x2ull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
    n = epoll_wait(ep, out, 4, 0);               /* 数据还在，重报 */
    CHECK(n == 1);
    CHECK(n == 1 && out[0].data.u64 == 0x2ull);

    close(ep);
    close(p[0]);
    close(p[1]);

    if (failures == 0) {
        dprintf(1, "PASS: m47epollflags (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m47epollflags (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
