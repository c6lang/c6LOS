/* poll.h（c6LOS + lwIP 端口）—— 见 port/s3/lwip/posix/sys/socket.h 的说明
 *
 * `std/net/tcp.c6` 的 tcp_connect(timeout) 是"非阻塞 connect + poll"实现的，
 * 所以 poll 是必需的（lwIP 侧对应 lwip_poll）。
 */
#ifndef C6OS_POSIX_POLL_H
#define C6OS_POSIX_POLL_H

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif /* C6OS_POSIX_POLL_H */
