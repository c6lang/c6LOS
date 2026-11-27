/* netinet/in.h（c6os + lwIP 端口）—— 见 port/s3/lwip/posix/sys/socket.h 的说明 */
#ifndef C6OS_POSIX_NETINET_IN_H
#define C6OS_POSIX_NETINET_IN_H

#include <sys/socket.h>

#define INADDR_ANY       ((in_addr_t)0x00000000UL)
#define INADDR_BROADCAST ((in_addr_t)0xffffffffUL)
#define INADDR_NONE      ((in_addr_t)0xffffffffUL)

#define IN_CLASSA(a)     ((((in_addr_t)(a)) & 0x80000000UL) == 0)
#define IN_CLASSA_NET    0xff000000UL
#define IN_CLASSA_NSHIFT 24

#define IN_CLASSB(a)     ((((in_addr_t)(a)) & 0xc0000000UL) == 0x80000000UL)
#define IN_CLASSB_NET    0xffff0000UL
#define IN_CLASSB_NSHIFT 16

#define IN_CLASSC(a)     ((((in_addr_t)(a)) & 0xe0000000UL) == 0xc0000000UL)
#define IN_CLASSC_NET    0xffffff00UL
#define IN_CLASSC_NSHIFT 8

/* 端口/地址在结构体里**始终是网络字节序**（和 lwIP 一致）。
 * 注意 struct in6_addr / sockaddr_in6 定义在 <sys/socket.h> 里（和 lwIP 一样），
 * 这里不再重复定义。 */

int          inet_aton(const char *cp, struct in_addr *inp);
in_addr_t    inet_addr(const char *cp);
char        *inet_ntoa(struct in_addr addr);
const char  *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int          inet_pton(int af, const char *src, void *dst);

/* 字节序（Xtensa 是小端；这些都编成 bswap 指令） */
u32_t htonl(u32_t x);
u16_t htons(u16_t x);
u32_t ntohl(u32_t x);
u16_t ntohs(u16_t x);

#endif /* C6OS_POSIX_NETINET_IN_H */
