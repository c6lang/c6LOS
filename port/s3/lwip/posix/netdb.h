/* netdb.h（c6os + lwIP 端口）—— 见 port/s3/lwip/posix/sys/socket.h 的说明
 *
 * 布局照抄 lwIP 的 `lwip/netdb.h`：`std/net/tcp.c6` 用 getaddrinfo() 把
 * "host/端口"变成 sockaddr，所以这里的 addrinfo 必须是 lwIP 认得的那一份。
 */
#ifndef C6OS_POSIX_NETDB_H
#define C6OS_POSIX_NETDB_H

#include <sys/socket.h>

struct hostent {
    char  *h_name;        /* 主机正式名 */
    char **h_aliases;     /* 别名列表（以 0 结尾） */
    int    h_addrtype;    /* AF_INET */
    int    h_length;      /* 地址长度（4） */
    char **h_addr_list;   /* 地址列表（以 0 结尾） */
};

#define h_addr  h_addr_list[0]

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08
#define AI_V4MAPPED    0x08
#define AI_ADDRCONFIG  0x10

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -5
#define EAI_SOCKTYPE   -6
#define EAI_SERVICE    -7
#define EAI_MEMORY     -8
#define EAI_SYSTEM     -9

int             getaddrinfo(const char *nodename, const char *servname,
                            const struct addrinfo *hints, struct addrinfo **res);
void            freeaddrinfo(struct addrinfo *ai);
struct hostent *gethostbyname(const char *name);
int             gethostbyname_r(const char *name, struct hostent *ret, char *buf,
                                size_t buflen, struct hostent **result, int *h_errnop);
const char     *gai_strerror(int errcode);

#endif /* C6OS_POSIX_NETDB_H */
