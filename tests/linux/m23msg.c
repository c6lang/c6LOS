/* M23 验收：sendmsg/recvmsg 的 iovec 展开。
 * 普通 glibc 程序，复用 M18 的 UDP 环回（connect + send 走本机入队，不碰 ARP）：
 *   1) sendmsg 两个 iovec（"hello " + "world"）→ recvmsg 三个 iovec 重组一致
 *   2) recvmsg 的 msg_namelen/来源地址正确
 *   3) sendmsg 带 msg_name（sendto 语义）→ 对端 recvmsg 收到
 *   4) msg_controllen/msg_flags 归零
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static void fill_addr(struct sockaddr_in *a, unsigned short port)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(0x0a00020fu);   /* 10.0.2.15 */
}

int main(void)
{
    int a, b;
    struct sockaddr_in sa, sb, from;
    struct msghdr mh;
    struct iovec iov[3];
    char o1[16], o2[16], o3[16];
    ssize_t n;

    a = socket(AF_INET, SOCK_DGRAM, 0);
    b = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(a >= 0 && b >= 0);
    if (a < 0 || b < 0) { goto done; }

    fill_addr(&sa, 7877);
    fill_addr(&sb, 7878);
    CHECK(bind(a, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(bind(b, (struct sockaddr *)&sb, sizeof(sb)) == 0);
    CHECK(connect(a, (struct sockaddr *)&sb, sizeof(sb)) == 0);

    /* 1. sendmsg 两个 iovec → recvmsg 三个 iovec 重组 */
    {
        char s1[] = "hello ";
        char s2[] = "world";
        struct iovec si[2];

        si[0].iov_base = s1; si[0].iov_len = 6;
        si[1].iov_base = s2; si[1].iov_len = 5;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = si;
        mh.msg_iovlen = 2;
        CHECK(sendmsg(a, &mh, 0) == 11);
    }
    memset(o1, 0, sizeof(o1));
    memset(o2, 0, sizeof(o2));
    memset(o3, 0, sizeof(o3));
    iov[0].iov_base = o1; iov[0].iov_len = 4;    /* "hell" */
    iov[1].iov_base = o2; iov[1].iov_len = 4;    /* "o wo" */
    iov[2].iov_base = o3; iov[2].iov_len = 8;    /* "rld" */
    memset(&from, 0, sizeof(from));
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = &from;
    mh.msg_namelen = sizeof(from);
    mh.msg_iov = iov;
    mh.msg_iovlen = 3;
    n = recvmsg(b, &mh, 0);
    CHECK(n == 11);
    CHECK(memcmp(o1, "hell", 4) == 0);
    CHECK(memcmp(o2, "o wo", 4) == 0);
    CHECK(memcmp(o3, "rld", 3) == 0);
    CHECK(mh.msg_namelen == 16);
    CHECK(from.sin_family == AF_INET);
    CHECK(ntohs(from.sin_port) == 7877);
    CHECK(mh.msg_controllen == 0);
    CHECK(mh.msg_flags == 0);

    /* 2. sendmsg 带 msg_name（sendto 语义，对未 connect 的 b 发） */
    {
        char s[] = "named";
        struct iovec si;

        si.iov_base = s; si.iov_len = 5;
        memset(&mh, 0, sizeof(mh));
        mh.msg_name = &sa;                     /* 目的 = a 的地址(7877) */
        mh.msg_namelen = sizeof(sa);
        mh.msg_iov = &si;
        mh.msg_iovlen = 1;
        CHECK(sendmsg(b, &mh, 0) == 5);
    }
    memset(o1, 0, sizeof(o1));
    iov[0].iov_base = o1; iov[0].iov_len = sizeof(o1);
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 1;
    CHECK(recvmsg(a, &mh, 0) == 5);
    CHECK(memcmp(o1, "named", 5) == 0);

    CHECK(close(a) == 0);
    CHECK(close(b) == 0);

done:
    if (failures == 0) {
        dprintf(1, "PASS: m23msg (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m23msg (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
