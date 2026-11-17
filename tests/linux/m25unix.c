/* M25 验收：AF_UNIX socket（SOCK_DGRAM）。
 * 普通 glibc 程序，验 Linux ABI 的 AF_UNIX 语义：
 *   1) socket(AF_UNIX, SOCK_DGRAM, 0) 创建
 *   2) bind 路径名 + 重复 bind → EADDRINUSE
 *   3) connect 到对端路径 + 双端 send/recv 数据正确
 *   4) recv 空队列：阻塞端先有数据再读；非阻塞端 EAGAIN
 *   5) connect 不存在的路径 → ECONNREFUSED
 *   6) close 后对象回收（可重新 bind 同名） */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static int make_addr(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, path, sizeof(a->sun_path) - 1);
    return (int)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

int main(void)
{
    int a, b, c;
    struct sockaddr_un sa, sb;
    char buf[64];

    a = socket(AF_UNIX, SOCK_DGRAM, 0);
    b = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(a >= 0 && b >= 0);
    if (a < 0 || b < 0) { goto done; }

    make_addr(&sa, "/tmp/m25-a");
    make_addr(&sb, "/tmp/m25-b");
    CHECK(bind(a, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(bind(b, (struct sockaddr *)&sb, sizeof(sb)) == 0);

    /* 重复 bind → EADDRINUSE */
    c = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(c >= 0);
    errno = 0;
    CHECK(bind(c, (struct sockaddr *)&sa, sizeof(sa)) == -1 &&
          errno == EADDRINUSE);

    /* 不存在的路径 → ECONNREFUSED */
    {
        struct sockaddr_un missing;

        make_addr(&missing, "/tmp/m25-nope");
        errno = 0;
        CHECK(connect(c, (struct sockaddr *)&missing, sizeof(missing)) == -1 &&
              errno == ECONNREFUSED);
    }
    close(c);

    /* 双端 connect + 通信 */
    CHECK(connect(a, (struct sockaddr *)&sb, sizeof(sb)) == 0);
    CHECK(connect(b, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(send(a, "hello-unix", 10, 0) == 10);
    memset(buf, 0, sizeof(buf));
    CHECK(recv(b, buf, sizeof(buf), 0) == 10);
    CHECK(memcmp(buf, "hello-unix", 10) == 0);
    CHECK(send(b, "pong", 4, 0) == 4);
    memset(buf, 0, sizeof(buf));
    CHECK(recv(a, buf, sizeof(buf), 0) == 4);
    CHECK(memcmp(buf, "pong", 4) == 0);

    /* 非阻塞端空队列 → EAGAIN */
    c = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(c >= 0);
    make_addr(&sb, "/tmp/m25-c");
    CHECK(bind(c, (struct sockaddr *)&sb, sizeof(sb)) == 0);
    CHECK(connect(a, (struct sockaddr *)&sb, sizeof(sb)) == 0);
    errno = 0;
    CHECK(recv(c, buf, sizeof(buf), 0) == -1 && errno == EAGAIN);
    CHECK(send(a, "to-c", 4, 0) == 4);
    memset(buf, 0, sizeof(buf));
    CHECK(recv(c, buf, sizeof(buf), 0) == 4);
    CHECK(memcmp(buf, "to-c", 4) == 0);
    close(c);

    CHECK(close(a) == 0);
    CHECK(close(b) == 0);

done:
    if (failures == 0) {
        dprintf(1, "PASS: m25unix (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m25unix (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
