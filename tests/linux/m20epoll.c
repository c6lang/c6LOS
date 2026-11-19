/* M20 验收：epoll_create1/epoll_ctl/epoll_wait。
 * 普通 glibc 程序：
 *   1) eventfd 注册到 epoll，写计数器 → epoll_wait 报 EPOLLIN + data 回传
 *   2) 管道注册 EPOLLIN，写入 → 报可读；读空后不再报（水平触发）
 *   3) epoll_wait 超时返回 0
 *   4) EPOLL_CTL_DEL / MOD / 重复 ADD 的 errno
 *   5) 多 fd 同一次 wait 返回 2 个就绪
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    int ep, efd, p[2];
    struct epoll_event ev, out[4];
    int n;
    unsigned long long v;
    char c;

    ep = epoll_create1(0);
    CHECK(ep >= 0);
    if (ep < 0) { goto done; }

    /* 1. eventfd + data 回传 */
    efd = eventfd(0, 0);
    CHECK(efd >= 0);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);          /* 未就绪，超时 0 */
    v = 1;
    CHECK(write(efd, &v, 8) == 8);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    if (n == 1) {
        CHECK((out[0].events & EPOLLIN) != 0);
        CHECK(out[0].data.u64 == 0x1122334455667788ull);
    }
    /* 水平触发：读空后再 wait 应不再报 */
    CHECK(read(efd, &v, 8) == 8);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    /* 重复 ADD → EEXIST */
    errno = 0;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == -1 && errno == EEXIST);
    /* MOD */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.u64 = 0xdeadbeefull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev) == 0);
    v = 2;
    CHECK(write(efd, &v, 8) == 8);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    if (n == 1) { CHECK(out[0].data.u64 == 0xdeadbeefull); }
    CHECK(read(efd, &v, 8) == 8);
    /* DEL */
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, efd, &ev) == 0);
    v = 3;
    CHECK(write(efd, &v, 8) == 8);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    CHECK(close(efd) == 0);

    /* 2. 管道 */
    CHECK(pipe(p) == 0);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0xa5a5a5a5a5a5a5a5ull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    CHECK(write(p[1], "x", 1) == 1);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].data.u64 == 0xa5a5a5a5a5a5a5a5ull);
        CHECK((out[0].events & EPOLLIN) != 0);
    }
    CHECK(read(p[0], &c, 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], &ev) == 0);

    /* 3. 超时 */
    {
        long t0 = clock();
        int r = epoll_wait(ep, out, 4, 30);   /* 30ms */
        long ms = (clock() - t0) * 1000 / CLOCKS_PER_SEC;

        CHECK(r == 0);
        /* 内核用 port_cycles 假时钟，与宿主 clock() 不同源；只验不挂死。 */
        CHECK(ms >= 0 && ms < 500);
    }

    /* 4. 多 fd 同时就绪 */
    efd = eventfd(1, 0);
    CHECK(efd >= 0);
    CHECK(pipe(p) == 0);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN; ev.data.u64 = 1;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0);
    ev.data.u64 = 2;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(write(p[1], "y", 1) == 1);
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 2);

    CHECK(close(ep) == 0);

done:
    if (failures == 0) {
        dprintf(1, "PASS: m20epoll (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m20epoll (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
