/* POSIX 名字的薄封装：socket()/connect()/poll()/... → lwIP 的 lwip_*()
 *
 * 为什么要这一层（docs/wifi-plan.md §209）：
 *   `/home/winger/c6/std/net/tcp.c6`、`https.c6` 这些模块是按 POSIX 写的
 *   （socket/connect/poll/getaddrinfo/read/write），而 lwIP 编出来的符号是
 *   `lwip_socket`/`lwip_connect`/...（因为我们的 lwipopts.h 里
 *   `LWIP_COMPAT_SOCKETS=0`）。所以要么打开 lwIP 的宏式兼容层（那套宏只在
 *   include lwIP 头的 TU 里生效），要么就像本文件这样提供**真正的符号**。
 *   这里选后者：c6 侧只 include 我们自己的 `port/s3/lwip/posix/` 头，
 *   不依赖 lwIP 的 include 树。
 *
 * 结构体/常量在 `port/s3/lwip/posix/sys/socket.h` 里是**逐字节照抄 lwIP** 的，
 * 所以这里全是纯转发，一个字段都不搬。
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/def.h"

/* lwIP 可能把 htons/htonl 定义成宏；这里要的是真符号 */
#ifdef htons
#undef htons
#endif
#ifdef ntohs
#undef ntohs
#endif
#ifdef htonl
#undef htonl
#endif
#ifdef ntohl
#undef ntohl
#endif

int socket(int domain, int type, int protocol)
{
    return lwip_socket(domain, type, protocol);
}

int bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    return lwip_bind(s, name, namelen);
}

int listen(int s, int backlog)
{
    return lwip_listen(s, backlog);
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    return lwip_accept(s, addr, addrlen);
}

int connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    return lwip_connect(s, name, namelen);
}

int getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    return lwip_getpeername(s, name, namelen);
}

int getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    return lwip_getsockname(s, name, namelen);
}

int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    return lwip_getsockopt(s, level, optname, optval, optlen);
}

int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    return lwip_setsockopt(s, level, optname, optval, optlen);
}

int send(int s, const void *data, socklen_t size, int flags)
{
    return lwip_send(s, data, size, flags);
}

int sendto(int s, const void *data, socklen_t size, int flags,
           const struct sockaddr *to, socklen_t tolen)
{
    return lwip_sendto(s, data, size, flags, to, tolen);
}

int recv(int s, void *mem, socklen_t len, int flags)
{
    return lwip_recv(s, mem, len, flags);
}

int recvfrom(int s, void *mem, socklen_t len, int flags,
             struct sockaddr *from, socklen_t *fromlen)
{
    return lwip_recvfrom(s, mem, len, flags, from, fromlen);
}

int shutdown(int s, int how)
{
    return lwip_shutdown(s, how);
}

int ioctl(int s, long cmd, void *arg)
{
    return lwip_ioctl(s, cmd, arg);
}

int fcntl(int s, int cmd, int val)
{
    return lwip_fcntl(s, cmd, val);
}

int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    return lwip_poll(fds, nfds, timeout);
}

int getaddrinfo(const char *nodename, const char *servname,
                const struct addrinfo *hints, struct addrinfo **res)
{
    return lwip_getaddrinfo(nodename, servname, hints, res);
}

void freeaddrinfo(struct addrinfo *ai)
{
    lwip_freeaddrinfo(ai);
}

struct hostent *gethostbyname(const char *name)
{
    return lwip_gethostbyname(name);
}

u32_t htonl(u32_t x) { return lwip_htonl(x); }
u16_t htons(u16_t x) { return lwip_htons(x); }
u32_t ntohl(u32_t x) { return lwip_ntohl(x); }
u16_t ntohs(u16_t x) { return lwip_ntohs(x); }

/* 这三个和内核/宿主 libc 的符号同名，做成可关的：
 * 我们的 freestanding 目标上没有 libc 的 read/write/close，所以默认提供。 */
#ifndef C6_LWIP_NO_POSIX_IO
int close(int s)
{
    return lwip_close(s);
}

int read(int s, void *buf, unsigned int len)
{
    return lwip_read(s, buf, len);
}

int write(int s, const void *buf, unsigned int len)
{
    return lwip_write(s, buf, len);
}
#endif
