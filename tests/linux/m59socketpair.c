/* M59 验收：socketpair(AF_UNIX)。
 *
 * busybox wget 的 SSL helper 用 socketpair(AF_UNIX, SOCK_STREAM, 0) 建一对
 * socket，fork 后把一端 dup2 到 stdin/stdout 交给 openssl s_client；父进程
 * 关掉自己那份写端再读写。所以除了"两端能收发"，还必须验证 dup2 + close
 * 原 fd、fork 继承、对端全关后读到 EOF 这三条引用计数语义。 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static int eq(const char *buf, const char *want, int n)
{
    return memcmp(buf, want, (size_t)n) == 0;
}

int main(void)
{
    int sv[2];
    int dsv[2];
    int nsv[2];
    char buf[16];
    pid_t c;
    int st;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    if (sv[0] >= 0 && sv[1] >= 0) {
        CHECK(sv[0] >= 3 && sv[1] >= 3 && sv[0] != sv[1]);

        CHECK(write(sv[0], "AB", 2) == 2);
        CHECK(read(sv[1], buf, 2) == 2 && eq(buf, "AB", 2));
        CHECK(write(sv[1], "CD", 2) == 2);
        CHECK(read(sv[0], buf, 2) == 2 && eq(buf, "CD", 2));

        /* xmove_fd 模式：dup2 出别名后 close 原 fd，别名仍然可用 */
        CHECK(dup2(sv[1], 10) == 10);
        CHECK(close(sv[1]) == 0);
        CHECK(write(10, "EF", 2) == 2);
        CHECK(read(sv[0], buf, 2) == 2 && eq(buf, "EF", 2));

        /* fork 继承：父关掉自己那份写端，子仍然能把数据写回来 */
        c = fork();
        CHECK(c >= 0);
        if (c == 0) {
            close(sv[0]);
            if (write(10, "GH", 2) != 2) { _exit(2); }
            close(10);
            _exit(0);
        }
        CHECK(close(10) == 0);
        CHECK(waitpid(c, &st, 0) == c);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK(read(sv[0], buf, 2) == 2 && eq(buf, "GH", 2));
        /* 子的写端全关后，读端应看到 EOF 而不是永久阻塞 */
        CHECK(read(sv[0], buf, 1) == 0);
        CHECK(close(sv[0]) == 0);
    }

    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, dsv) == 0);
    if (dsv[0] >= 0 && dsv[1] >= 0) {
        CHECK(write(dsv[0], "XYZ", 3) == 3);
        CHECK(read(dsv[1], buf, 3) == 3 && eq(buf, "XYZ", 3));
        CHECK(close(dsv[0]) == 0);
        CHECK(close(dsv[1]) == 0);
    }

    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, nsv) == 0);
    if (nsv[0] >= 0 && nsv[1] >= 0) {
        errno = 0;
        CHECK(read(nsv[0], buf, 1) == -1 && errno == EAGAIN);
        CHECK(close(nsv[0]) == 0);
        CHECK(close(nsv[1]) == 0);
    }

    errno = 0;
    CHECK(socketpair(AF_INET, SOCK_STREAM, 0, sv) == -1 &&
          errno == EAFNOSUPPORT);
    errno = 0;
    CHECK(socketpair(AF_UNIX, SOCK_RAW, 0, sv) == -1 && errno == EINVAL);

    if (failures == 0) {
        dprintf(1, "PASS: m59socketpair (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m59socketpair (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
