/* include/los/net.h —— 最小 TCP/IP 子集（M10）
 *
 * 分工：
 *   NetBuf    预分配的收发缓冲（静态池，不依赖堆）
 *   NetIf     网卡抽象：MAC/IP/掩码/网关 + linkoutput（驱动在运行时挂上来）
 *   arp.c6    ARP 表 + 请求/应答
 *   ip.c6     IPv4 头构造/校验/分发（不分片）
 *   icmp.c6   echo request/reply（ping）
 *
 * 约定：
 *   - **IP 地址一律用主机序的 UINT32**（`0x0A000202` 就是 10.0.2.2），
 *     上下线时用 NetPut32/NetGet32 逐字节搬，省得和字节序较劲；
 *     端口同理（主机序 UINT16），收发时 NetPut16/NetGet16。
 *   - 跨文件只传 `LosNetBuf *` / `LosNetIf *` / 整数，和 ABI 规则一致。
 *   - 结构体里的函数指针字段（linkoutput）只能运行时赋值：c6 不支持它的
 *     静态初始化，也**不能**拿它和 0 比（见 §8 M10-0 的坑）。
 *   - 本层不做：分片重组、IP 选项、IPv6、DHCP/DNS。
 */
#ifndef _LOS_NET_H
#define _LOS_NET_H

#include "c6os/types.h"
#include "los/los_typedef.h"

#define LOS_NET_MAX_IF      4u
#define LOS_NET_MAX_BUFS    16u
#define LOS_NET_BUF_SIZE    512u
#define LOS_NET_HDR_ROOM    64u      /* 给链路层+IP 预留的头空间 */

#define LOS_ETH_HLEN        14u
#define LOS_ETH_ALEN        6u
#define LOS_ETH_P_IP        0x0800u
#define LOS_ETH_P_ARP       0x0806u

#define LOS_IP_HLEN         20u
#define LOS_IP_PROTO_ICMP   1u
#define LOS_IP_PROTO_TCP    6u
#define LOS_IP_PROTO_UDP    17u
#define LOS_IP_TTL          64u

#define LOS_ARP_MAX         8u

/* ---- 缓冲 ---- */

typedef struct LosNetBuf {
    UINT32 used;
    UINT32 len;                 /* data[off, off+len) 是有效载荷 */
    UINT32 off;
    UINT8  data[LOS_NET_BUF_SIZE];
} LosNetBuf;

typedef struct LosNetIf LosNetIf;

struct LosNetIf {
    UINT32 used;
    UINT32 up;
    CHAR   name[8];
    UINT8  mac[LOS_ETH_ALEN];
    UINT32 ip;                  /* 主机序 */
    UINT32 mask;
    UINT32 gw;
    UINT32 link;                /* 1 = 驱动已挂上（不做函数指针判空，c6 不可靠） */
    INT32 (*linkoutput)(LosNetIf *nif, LosNetBuf *p);   /* 驱动挂载，运行时赋值 */
    VOID  *drv;                 /* 驱动私有（e1000 用） */
    UINT32 rx_packets;
    UINT32 tx_packets;
    UINT32 rx_dropped;
    UINT32 tx_dropped;
};

/* ---- 字节序/校验和 ---- */

VOID   NetPut16(UINT8 *p, UINT16 v);        /* 写网络序 */
VOID   NetPut32(UINT8 *p, UINT32 v);
UINT16 NetGet16(const UINT8 *p);
UINT32 NetGet32(const UINT8 *p);
UINT16 NetCsum(const UINT8 *data, UINT32 len);

/* ---- 以太网头（上层填目的 MAC + 类型，源 MAC 由 NetIfOutput 补） ---- */

INT32 NetEthPush(LosNetBuf *p, const UINT8 *dst, UINT16 ethertype);
INT32 NetEthPop(LosNetBuf *p, UINT16 *ethertype_out);

/* ---- NetBuf ---- */

LosNetBuf *NetBufAlloc(UINT32 hdr_room, UINT32 len);
VOID       NetBufFree(LosNetBuf *p);
UINT32     NetBufInUse(VOID);

/* ---- NetIf ---- */

LosNetIf *NetIfAlloc(const CHAR *name);
VOID      NetIfSetMac(LosNetIf *nif, const UINT8 *mac);
INT32     NetIfSetIp(LosNetIf *nif, UINT32 ip, UINT32 mask, UINT32 gw);
VOID      NetIfSetLink(LosNetIf *nif, INT32 (*fn)(LosNetIf *, LosNetBuf *));
INT32     NetIfUp(LosNetIf *nif);
INT32     NetIfOutput(LosNetIf *nif, LosNetBuf *p);
INT32     NetIfInput(LosNetIf *nif, LosNetBuf *p);
UINT32    NetIfCount(VOID);

/* x86 参考目标：把 port 层的 e1000 挂上来（netif_x86.c6；S3 不用） */
LosNetIf *NetIfAttachX86(VOID);
UINT32    NetIfPollX86(LosNetIf *nif);
LosNetIf *NetIfGet(UINT32 i);

/* ---- ARP ---- */

INT32  ArpInput(LosNetIf *nif, LosNetBuf *p);
INT32  ArpResolve(LosNetIf *nif, UINT32 ip, UINT8 *mac_out);   /* 1 命中 / 0 未命中 */
INT32  ArpRequest(LosNetIf *nif, UINT32 ip);
UINT32 ArpCacheCount(VOID);

/* ---- IPv4 ---- */

INT32 IpOutput(LosNetIf *nif, UINT32 dst, UINT8 proto, LosNetBuf *p);
INT32 IpInput(LosNetIf *nif, LosNetBuf *p);

/* ---- UDP（kernel/net/udp.c6） ---- */

INT32  UdpSockOpen(VOID);
INT32  UdpSockClose(INT32 ud);
INT32  UdpSockBind(INT32 ud, UINT16 port);
INT32  UdpSockConnect(INT32 ud, UINT32 ip, UINT16 port);
INT32  UdpSockSend(INT32 ud, const VOID *buf, UINT32 len);
INT32  UdpSockSendTo(INT32 ud, UINT32 ip, UINT16 port, const VOID *buf, UINT32 len);
INT32  UdpSockRecv(INT32 ud, VOID *buf, UINT32 max);
INT32  UdpSockRecvFrom(INT32 ud, VOID *buf, UINT32 max, UINT32 *ip, UINT16 *port);
INT32  UdpSockPoll(INT32 ud, UINT32 events);
INT32  UdpInput(LosNetIf *nif, LosNetBuf *p, UINT32 src);

/* 把 UDP 注册到 SAL（kernel/net/sal_udp.c6） */
INT32  SalUdpInit(VOID);

/* ---- ICMP ---- */

INT32  IcmpInput(LosNetIf *nif, LosNetBuf *p, UINT32 src);
INT32  IcmpPing(LosNetIf *nif, UINT32 dst, UINT16 id, UINT16 seq);   /* 发请求，1 = 立即拿到应答 */
INT32  IcmpGotReply(UINT16 id, UINT16 seq);                          /* 该 (id,seq) 的应答到过没 */
UINT32 IcmpReplies(VOID);
UINT32 IcmpRequests(VOID);

#endif
