/* M46 验收：AF_UNIX SOCK_STREAM 真字节流语义
 *  1) 多次小写 → 一次读合并（无消息边界）
 *  2) 大写 → 多次部分读，剩余字节不丢
 *  3) SCM_RIGHTS（M42）不受影响 —— 单独一个 fd 随数据传递
 * 用 fork：父 listen+accept，子 connect；子发、父收。
 */
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
    char buf[64];
    ssize_t n;
    int st;

    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(srv >= 0);
    make_addr(&sa, "/tmp/m46-stream");
    CHECK(bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(listen(srv, 4) == 0);

    c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        cli = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli < 0) { _exit(2); }
        if (connect(cli, (struct sockaddr *)&sa, sizeof(sa)) != 0) { _exit(3); }
        /* 三次小写："ab" + "cd" + "ef" */
        if (send(cli, "ab", 2, 0) != 2 ||
            send(cli, "cd", 2, 0) != 2 ||
            send(cli, "ef", 2, 0) != 2) { _exit(4); }
        /* 一次大写 10 字节 */
        if (send(cli, "0123456789", 10, 0) != 10) { _exit(5); }
        /* 等父进程回信号再退出 */
        n = recv(cli, buf, 1, 0);
        if (n != 1) { _exit(6); }
        _exit(0);
    }

    acc = accept(srv, 0, 0);
    CHECK(acc >= 0);
    if (acc >= 0) {
        /* 合并读：应一次拿到 "abcdef" */
        memset(buf, 0, sizeof(buf));
        n = recv(acc, buf, 6, 0);
        CHECK(n == 6);
        CHECK(n == 6 && memcmp(buf, "abcdef", 6) == 0);
        /* 部分读：先读 4，再读剩余 6，不丢 */
        memset(buf, 0, sizeof(buf));
        n = recv(acc, buf, 4, 0);
        CHECK(n == 4);
        CHECK(n == 4 && memcmp(buf, "0123", 4) == 0);
        n = recv(acc, buf, 6, 0);
        CHECK(n == 6);
        CHECK(n == 6 && memcmp(buf, "456789", 6) == 0);
        CHECK(send(acc, "z", 1, 0) == 1);
        close(acc);
    }
    CHECK(waitpid(c, &st, 0) == c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(srv);

    if (failures == 0) {
        dprintf(1, "PASS: m46stream (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m46stream (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
