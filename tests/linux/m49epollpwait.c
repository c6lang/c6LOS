/* M49 验收：epoll_pwait(2)（nr255）
 *  1) epoll_pwait 基本收发：注册 pipe，写数据，epoll_pwait 报 EPOLLIN
 *  2) sigmask 语义：传非空掩码，调用后进程掩码恢复到调用前的值
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
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
    sigset_t mask, before, after;
    int n;

    ep = epoll_create1(0);
    CHECK(ep >= 0);
    CHECK(pipe(p) == 0);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x49ull;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(write(p[1], "hi", 2) == 2);

    /* 传一个临时信号掩码 */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, 0, &before);          /* 读当前掩码 */
    n = epoll_pwait(ep, out, 4, 0, &mask);
    CHECK(n == 1);
    CHECK(n == 1 && out[0].data.u64 == 0x49ull);
    sigprocmask(SIG_BLOCK, 0, &after);           /* 读回，应等于调用前 */
    /* 内核 sigset 只有 64 位（8 字节）；sigset_t 其余栈上字节未初始化，
     * 故只比较内核实际写入的部分。 */
    CHECK(memcmp(&before, &after, 8) == 0);
    CHECK(sigismember(&after, SIGUSR1) == sigismember(&before, SIGUSR1));

    /* NULL 掩码等价 epoll_wait */
    CHECK(write(p[1], "again", 5) == 5);
    n = epoll_pwait(ep, out, 4, 0, 0);
    CHECK(n == 1);

    close(ep);
    close(p[0]);
    close(p[1]);

    if (failures == 0) {
        dprintf(1, "PASS: m49epollpwait (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m49epollpwait (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
