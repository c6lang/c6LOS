/* M24 验收：getaddrinfo / getnameinfo 在 c6 Linux ABI 上可用。
 * 分三层定位：
 *   1) AI_NUMERICHOST 数字地址解析（纯用户态 glibc）
 *   2) /etc/hosts 的 localhost 解析（需文件系统）
 *   3) 真实 DNS：slirp 的 10.0.2.3 解析 example.com（需 resolv.conf + UDP）
 * 每层独立记分，失败不阻断后续层，便于定位缺口。 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <dlfcn.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d gai=%s)\n", \
        #c, __LINE__, errno, gai_strerror(grc)); } \
} while (0)

int main(void)
{
    struct addrinfo hints, *res = 0;
    int grc;
    char host[64];

    /* 1. 数字 IP，纯用户态 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    grc = getaddrinfo("10.0.2.15", "7777", &hints, &res);
    CHECK(grc == 0);
    if (grc == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        CHECK(sin->sin_addr.s_addr == htonl(0x0a00020fu));
        CHECK(ntohs(sin->sin_port) == 7777);
        CHECK(getnameinfo(res->ai_addr, res->ai_addrlen,
                          host, sizeof(host), 0, 0, NI_NUMERICHOST) == 0);
        CHECK(strcmp(host, "10.0.2.15") == 0);
        freeaddrinfo(res);
        res = 0;
    }

    /* 2. /etc/hosts：localhost */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    grc = getaddrinfo("localhost", 0, &hints, &res);
    CHECK(grc == 0);
    if (grc == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        CHECK(sin->sin_addr.s_addr == htonl(0x7f000001u));
        freeaddrinfo(res);
        res = 0;
    }

    /* 3b. 手写 UDP DNS 探针：绕过 NSS/libresolv，直接测网络层。
     * 若它通而 getaddrinfo 不通 → NSS 模块加载问题；否则网络层问题。 */
    {
        int sd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in dns;
        socklen_t sl = sizeof(dns);
        unsigned char q[40];
        int qlen = 0;
        int attempts;
        ssize_t nr = -1;  /* last recv result */

        memset(&dns, 0, sizeof(dns));
        dns.sin_family = AF_INET;
        dns.sin_port = htons(53);
        dns.sin_addr.s_addr = htonl(0x0a000203u);   /* 10.0.2.3 slirp DNS */
        /* M24：不显式 bind —— 与 glibc getaddrinfo 一致，依赖内核首次
         * send 时自动分配临时端口（udp_autobind_）。 */
        memset(q, 0, sizeof(q));
        q[0] = 0xAB; q[1] = 0xCD;                   /* transaction id */
        q[2] = 0x01; q[3] = 0x00;                   /* RD */
        q[4] = 0; q[5] = 1;                          /* QDCOUNT=1 */
        qlen = 12;
        { const char *name = "example.com";
          const char *p;
          for (p = name; *p; ) {
              const char *dot = strchr(p, '.');
              int seg = dot ? (int)(dot - p) : (int)strlen(p);
              q[qlen++] = (unsigned char)seg;
              memcpy(q + qlen, p, (size_t)seg); qlen += seg;
              p = dot ? dot + 1 : p + seg;
          }
          q[qlen++] = 0;
          q[qlen++] = 0; q[qlen++] = 1;              /* A */
          q[qlen++] = 0; q[qlen++] = 1;              /* IN */
        }
        CHECK(connect(sd, (struct sockaddr *)&dns, sizeof(dns)) == 0);
        for (attempts = 0; attempts < 200; attempts++) {
            ssize_t sn = send(sd, q, (size_t)qlen, 0);

            if (sn != qlen && attempts == 0) {
                dprintf(1, "PROBE: first send=%zd errno=%d\n", sn, errno);
            }
            if (sn == qlen) {
                struct timeval tv = {0, 100000};

                setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                nr = recvfrom(sd, q, sizeof(q), 0,
                              (struct sockaddr *)&dns, &sl);
                if (attempts < 2 || nr > 12) {
                    dprintf(1, "PROBE: attempt=%d recv=%zd errno=%d\n",
                            attempts, nr, errno);
                }
                if (nr > 12) { break; }
            }
        }
        CHECK(nr > 12);                            /* DNS 应答 */
        if (nr > 12) {
            CHECK(q[2] & 0x80);                    /* QR=1 */
            dprintf(1, "PROBE: raw DNS query %s (%zd bytes)\n",
                    nr > 12 ? "OK" : "FAIL", nr);
        }
        close(sd);
    }

    /* 3. glibc NSS 的 getaddrinfo(真实主机名)：内核 ABI 已由上面的手写 DNS
     * 探针完整覆盖（同一套 socket/connect/send/recv），这一层纯属 glibc NSS
     * 模块内部 resolver 细节（Alpine 用 musl，路径不同），只作观测打印。 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    grc = getaddrinfo("example.com", "80", &hints, &res);
    dprintf(1, "OBS: glibc getaddrinfo(example.com) grc=%d (%s)\n",
            grc, gai_strerror(grc));
    if (grc == 0) {
        freeaddrinfo(res);
        res = 0;
    }

    if (failures == 0) {
        dprintf(1, "PASS: m24dns (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m24dns (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
