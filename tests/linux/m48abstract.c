/* M48 验收：AF_UNIX 抽象名字空间（sun_path[0]=='\0'）
 *  1) abstract DGRAM bind/connect/send/recv 往返
 *  2) abstract 与文件系统路径同名隔离（两者互不干扰）
 */
#define _GNU_SOURCE
#include <errno.h>
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

static socklen_t make_abs(struct sockaddr_un *a, const char *name)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    a->sun_path[0] = '\0';
    strcpy(a->sun_path + 1, name);
    return (socklen_t)(2 + strlen(name));
}

static socklen_t make_path(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return (socklen_t)(2 + strlen(path) + 1);
}

int main(void)
{
    struct sockaddr_un sa, sb;
    socklen_t al, bl;
    int a, b;
    char buf[32];
    ssize_t n;

    /* abstract DGRAM 往返 */
    a = socket(AF_UNIX, SOCK_DGRAM, 0);
    b = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(a >= 0 && b >= 0);
    al = make_abs(&sa, "m48-abstract");
    CHECK(bind(a, (struct sockaddr *)&sa, al) == 0);
    CHECK(connect(b, (struct sockaddr *)&sa, al) == 0);
    CHECK(send(b, "abs-hello", 9, 0) == 9);
    memset(buf, 0, sizeof(buf));
    n = recv(a, buf, sizeof(buf), 0);
    CHECK(n == 9);
    CHECK(n == 9 && memcmp(buf, "abs-hello", 9) == 0);
    close(a);
    close(b);

    /* 与文件系统路径同名隔离：abstract "m48same" 与 /tmp/m48same */
    a = socket(AF_UNIX, SOCK_DGRAM, 0);
    b = socket(AF_UNIX, SOCK_DGRAM, 0);
    al = make_abs(&sa, "m48same");
    bl = make_path(&sb, "/tmp/m48same");
    CHECK(bind(a, (struct sockaddr *)&sa, al) == 0);
    CHECK(bind(b, (struct sockaddr *)&sb, bl) == 0);   /* 不冲突 */
    close(a);
    close(b);

    if (failures == 0) {
        dprintf(1, "PASS: m48abstract (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m48abstract (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
