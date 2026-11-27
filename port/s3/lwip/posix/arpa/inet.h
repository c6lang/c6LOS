/* arpa/inet.h（c6os + lwIP 端口）—— 见 port/s3/lwip/posix/sys/socket.h 的说明
 *
 * 真正的内容在 netinet/in.h 里（inet_pton/ntop/addr/ntoa + hton 系列/ntoh 系列），
 * 这里只是 POSIX 习惯上的转发。
 */
#ifndef C6OS_POSIX_ARPA_INET_H
#define C6OS_POSIX_ARPA_INET_H

#include <netinet/in.h>

#endif /* C6OS_POSIX_ARPA_INET_H */
