/* BSD socket 接口（c6os / ESP32-S3 + lwIP 端口用）
 *
 * 为什么需要它（docs/wifi-plan.md §209）：
 *   c6 自带的那套 libc 头（/home/winger/c6/c6c/include/）**没有** socket 相关头：
 *   `sys/socket.h`、`netinet/in.h`、`netdb.h`、`poll.h`、`arpa/inet.h` 一个都没有
 *   （那边的 `sys/` 里只有 `stat.h`）。而 `/home/winger/c6/std/net/tcp.c6`
 *   （tcp_connect/tcp_recv 等）正是靠 include 这些头来用 POSIX socket 的。
 *   所以"用 c6 标准库的 net 模块"这件事，前提是我们自己把这几个头补上。
 *
 * 放在这里而不是仓库根的 include/ 的原因：
 *   宿主（x86）构建也用 `-Iinclude`，如果把 `sys/socket.h` 放进 `include/`，
 *   会**遮蔽 glibc 的同名头**，把宿主构建搞坏。所以这些头单独放
 *   `port/s3/lwip/posix/`，只在 S3 + lwIP 的那个构建里通过
 *   `S3_C6FLAGS=-Iport/s3/lwip/posix` 加进来。
 *
 * 布局和常量**逐字节照抄 lwIP 的 `lwip/sockets.h`**（含 `sa_len` 首字段这种
 * BSD 风格），这样 `port/s3/lwip/c6_lwip_posix.c` 里的 POSIX 名字包装就是纯转发，
 * 不需要任何字段搬运，也就没有"结构体布局不一致"的隐患。
 */
#ifndef C6OS_POSIX_SYS_SOCKET_H
#define C6OS_POSIX_SYS_SOCKET_H

#include <stdint.h>

typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef uint8_t  sa_family_t;
typedef uint32_t socklen_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr {
    u8_t        sa_len;
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    u8_t        s2_len;
    sa_family_t ss_family;
    char        s2_data1[2];
    u32_t       s2_data2[3];
    u32_t       s2_data3[3];
};

struct sockaddr_in {
    u8_t            sin_len;
    sa_family_t     sin_family;
    in_port_t       sin_port;
    struct in_addr  sin_addr;
    char            sin_zero[8];
};

struct in6_addr {
    u8_t u_addr[16];
};

struct sockaddr_in6 {
    u8_t             sin6_len;
    sa_family_t      sin6_family;
    in_port_t        sin6_port;
    u32_t            sin6_flowinfo;
    struct in6_addr  sin6_addr;
    u32_t            sin6_scope_id;
};

extern const struct in6_addr in6addr_any;
#define IN6ADDR_ANY_INIT { { 0 } }

/* ---- 协议族 / socket 类型 / 层 ---- */
#define AF_UNSPEC       0
#define AF_INET         2
/* IPv6 只做**编译期**兼容：c6 的 std/net/tcp.c6 会同时试 v4/v6，
 * 我们的 lwIP 配置是 IPv6 关（§207），运行时 v6 的 socket() 直接失败，
 * 调用方会回落到 v4。数值与 lwIP 保持一致。 */
#define AF_INET6        10
#define PF_INET6        AF_INET6
#define PF_INET         AF_INET
#define PF_UNSPEC       AF_UNSPEC

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_RAW     255

#define SOL_SOCKET      0xfff

/* ---- setsockopt 选项（数值与 lwIP 一致）---- */
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_BROADCAST    0x0020
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008
#define SO_CONTIMEO     0x1009
#define SO_NO_CHECK     0x100a
#define SO_BINDTODEVICE 0x100b

/* ---- send/recv 标志 ---- */
#define MSG_PEEK        0x01
#define MSG_WAITALL     0x02
#define MSG_DONTWAIT    0x08
#define MSG_MORE        0x10
#define MSG_NOSIGNAL    0x20

/* ---- shutdown ---- */
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/* ---- 函数（实现在 port/s3/lwip/c6_lwip_posix.c，纯转发到 lwip_*）---- */
int     socket(int domain, int type, int protocol);
int     bind(int s, const struct sockaddr *name, socklen_t namelen);
int     listen(int s, int backlog);
int     accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int     connect(int s, const struct sockaddr *name, socklen_t namelen);
int     getpeername(int s, struct sockaddr *name, socklen_t *namelen);
int     getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int     getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen);
int     setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int     send(int s, const void *data, socklen_t size, int flags);
int     sendto(int s, const void *data, socklen_t size, int flags,
               const struct sockaddr *to, socklen_t tolen);
int     recv(int s, void *mem, socklen_t len, int flags);
int     recvfrom(int s, void *mem, socklen_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
int     shutdown(int s, int how);
int     ioctl(int s, long cmd, void *arg);
/* fcntl() **不在这里声明**：c6 自带的 <fcntl.h> 已经声明了它（我们 include
 * <sys/socket.h> 的单元往往也 include <fcntl.h>，两边签名不同会打架）。
 * 实现在 port/s3/lwip/c6_lwip_posix.c 里，符号名一致就对得上。 */

/* POSIX 里 close() 属于 <unistd.h>，但 c6 那份 unistd.h 里没有它
 * （socket 是一整套后加的接口）。S3 目标上没有 libc 的 close，所以在这儿补声明，
 * 实现在 c6_lwip_posix.c（→ lwip_close）。 */
int     close(int s);

#endif /* C6OS_POSIX_SYS_SOCKET_H */
