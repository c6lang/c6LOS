/* M41 验收：AF_UNIX SOCK_STREAM 的 listen/connect/accept。
 * 用 fork：父 listen+accept，子 connect，双方 send/recv 验字节流往返。 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static void make_addr(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, path, sizeof(a->sun_path) - 1);
}

int main(void)
{
    struct sockaddr_un sa;
    int srv, cli, acc;
    pid_t c;
    char buf[32];
    ssize_t n;
    int st;

    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    dprintf(1, "M41: srv=%d\n", srv);
    CHECK(srv >= 0);
    make_addr(&sa, "/tmp/m41-stream");
    CHECK(bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(listen(srv, 4) == 0);

    c = fork();
    dprintf(1, "M41: fork=%d\n", c);
    CHECK(c >= 0);
    if (c == 0) {
        cli = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli < 0) { dprintf(2, "C: socket fail %d\n", errno); _exit(2); }
        if (connect(cli, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            dprintf(2, "C: connect fail %d\n", errno); _exit(3);
        }
        if (send(cli, "from-client", 11, 0) != 11) {
            dprintf(2, "C: send fail %d\n", errno); _exit(4);
        }
        n = recv(cli, buf, sizeof(buf), 0);
        if (n != 10 || memcmp(buf, "from-serve", 10) != 0) {
            dprintf(2, "C: recv fail n=%zd errno=%d\n", n, errno); _exit(5);
        }
        _exit(0);
    }

    dprintf(1, "M41: before accept\n");
    acc = accept(srv, 0, 0);
    dprintf(1, "M41: acc=%d\n", acc);
    CHECK(acc >= 0);
    if (acc >= 0) {
        n = recv(acc, buf, sizeof(buf), 0);
        dprintf(1, "M41: server recv=%zd\n", n);
        CHECK(n == 11);
        CHECK(n == 11 && memcmp(buf, "from-client", 11) == 0);
        CHECK(send(acc, "from-serve", 10, 0) == 10);
        close(acc);
    }
    CHECK(waitpid(c, &st, 0) == c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(srv);

    if (failures == 0) {
        dprintf(1, "PASS: m41unixstream (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m41unixstream (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
