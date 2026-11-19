/* M18 验收：Linux socket syscall → LiteOS SAL，UDP 环回。
 * 普通 Linux 程序（gcc -static），内核零专用接口。
 *   1. socket(AF_INET, SOCK_DGRAM, 0) 拿到 fd
 *   2. bind 两个 socket 到不同端口（7777/7778）
 *   3. sendto 从 A 发到 B，recvfrom 收到且内容/来源正确
 *   4. recvfrom 无数据时 EAGAIN（SAL 非阻塞语义）
 *   5. close 释放
 * 端口/地址按主机字节序填 sockaddr_in（port 用 htons）。 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static int failures;

static void say(const char *s)
{
    (void)!write(1, s, strlen(s));
}

static void check(int cond, const char *what)
{
    if (!cond) {
        failures++;
        say("FAIL: ");
        say(what);
        say("\n");
    }
}

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
    socklen_t fromlen;
    char buf[32];
    ssize_t n;

    a = socket(AF_INET, SOCK_DGRAM, 0);
    check(a >= 0, "socket A");
    b = socket(AF_INET, SOCK_DGRAM, 0);
    check(b >= 0, "socket B");
    if (a < 0 || b < 0) {
        say("FAIL: m18sock\n");
        return 1;
    }

    fill_addr(&sa, 7777);
    fill_addr(&sb, 7778);
    {
        int rc = bind(a, (struct sockaddr *)&sa, sizeof(sa));
        if (rc != 0) { char m[64]; int n = snprintf(m, sizeof(m), "bind A errno=%d\n", errno); (void)!write(1, m, n); }
        check(rc == 0, "bind A");
    }
    check(bind(b, (struct sockaddr *)&sb, sizeof(sb)) == 0, "bind B");

    /* connect + send 走 UdpSockSend 的环回直入队路径（不碰 ARP/link）；
     * sendto 会经 IpOutput 要 ARP，验收环境不 wait link-up。 */
    check(connect(a, (struct sockaddr *)&sb, sizeof(sb)) == 0, "connect A->B");
    {
        int rc = send(a, "hello", 5, 0);
        if (rc != 5) { char m[64]; int n = snprintf(m, sizeof(m), "send errno=%d rc=%d\n", errno, rc); (void)!write(1, m, n); }
        check(rc == 5, "send A->B");
    }

    memset(buf, 0, sizeof(buf));
    memset(&from, 0, sizeof(from));
    fromlen = sizeof(from);
    n = recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    check(n == 5 && memcmp(buf, "hello", 5) == 0, "recvfrom B content");
    check(from.sin_family == AF_INET && ntohs(from.sin_port) == 7777,
          "recvfrom B source");

    /* 无数据时 SAL 返回 EAGAIN（errno 正数 11） */
    errno = 0;
    n = recvfrom(b, buf, sizeof(buf), 0, 0, 0);
    check(n == -1 && errno == EAGAIN, "recvfrom empty EAGAIN");

    close(a);
    close(b);

    if (failures == 0) {
        say("PASS: m18sock\n");
        return 0;
    }
    say("FAIL: m18sock\n");
    return 1;
}
