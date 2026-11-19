/* M54 验收：readlink / faccessat / fchdir / accept4
 *  1) readlink("/proc/self/exe") 返回 "/hello"
 *  2) faccessat(AT_FDCWD, existing, F_OK, 0) 成功；不存在 ENOENT
 *  3) fchdir 切到已打开目录 fd 后 getcwd 反映新目录
 *  4) accept4 在 AF_UNIX STREAM 上接受连接并设置 CLOEXEC 标志
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    char buf[64];
    ssize_t n;
    int dirfd;
    char cwd[256];
    int srv, cli, acc;

    /* readlink */
    memset(buf, 0, sizeof(buf));
    n = readlink("/proc/self/exe", buf, sizeof(buf));
    CHECK(n == 6);
    CHECK(n == 6 && memcmp(buf, "/hello", 6) == 0);

    /* faccessat */
    CHECK(faccessat(AT_FDCWD, "/tmp", F_OK, 0) == 0);
    errno = 0;
    CHECK(faccessat(AT_FDCWD, "/no-such-m54-file", F_OK, 0) == -1);
    CHECK(errno == ENOENT);

    /* fchdir：用 raw syscall（glibc fchdir 也走 nr81） */
    dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0);
    if (dirfd >= 0) {
        CHECK(fchdir(dirfd) == 0);
        memset(cwd, 0, sizeof(cwd));
        CHECK(getcwd(cwd, sizeof(cwd)) != 0);
        CHECK(strstr(cwd, "tmp") != 0);
        close(dirfd);
    }

    /* accept4 在 AF_UNIX STREAM：建一对已连接 socket，accept4 取连接端 */
    {
        struct sockaddr_un sa;
        socklen_t sl;

        srv = socket(AF_UNIX, SOCK_STREAM, 0);
        cli = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(srv >= 0 && cli >= 0);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, "/tmp/m54-sock");
        sl = sizeof(sa);
        CHECK(bind(srv, (struct sockaddr *)&sa, sl) == 0);
        CHECK(listen(srv, 4) == 0);
        CHECK(connect(cli, (struct sockaddr *)&sa, sl) == 0);

        acc = syscall(SYS_accept4, srv, 0, 0, SOCK_CLOEXEC);
        CHECK(acc >= 0);
        if (acc >= 0) {
            int fl = fcntl(acc, F_GETFD);
            CHECK(fl >= 0 && (fl & FD_CLOEXEC));
            close(acc);
        }
        close(cli);
        close(srv);
    }

    if (failures == 0) {
        dprintf(1, "PASS: m54dirfd (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m54dirfd (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
