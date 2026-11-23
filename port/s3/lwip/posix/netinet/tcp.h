/* netinet/tcp.h（c6LOS + lwIP 端口）—— 见 port/s3/lwip/posix/sys/socket.h 的说明 */
#ifndef C6OS_POSIX_NETINET_TCP_H
#define C6OS_POSIX_NETINET_TCP_H

#include <sys/socket.h>

/* 数值与 lwIP 的 lwip/sockets.h 一致 */
#define TCP_NODELAY    0x01
#define TCP_KEEPALIVE  0x02
#define TCP_KEEPIDLE   0x03
#define TCP_KEEPINTVL  0x04
#define TCP_KEEPCNT    0x05

#endif /* C6OS_POSIX_NETINET_TCP_H */
