/* include/los/sal.h —— LiteOS-M 风格的 SAL（Socket Abstract Layer）兼容面
 *
 * 上层（Linux ABI 的 socket 族 / shell / 应用）只依赖这一层；底下挂哪套
 * 协议栈由 SalRegister() 决定 —— 与 M9 的 LOS_Fs* 同一思路，区别是分发键
 * 从"挂载点类型字符串"换成了 (domain, type, proto) 三元组，载荷是函数表
 * （SalProto），所以可以在运行时注册多套协议栈（UDP/TCP 各一份，将来换成
 * 别的实现也不用改调用方）。
 *
 * 错误约定：一律返回 -errno（POSIX 风格），与 include/los/los_fs.h 一致；
 * LiteOS-M 的原生编码是 LOS_ERRNO_*，这里刻意不做，理由见 los_fs.h 顶部。
 * 例外：SalSocket() 成功时返回 sd（>= 0）。
 */
#ifndef _LOS_SAL_H
#define _LOS_SAL_H

#include "los/los_typedef.h"

#define LOS_SAL_AF_INET          2

#define LOS_SAL_SOCK_STREAM      1
#define LOS_SAL_SOCK_DGRAM       2
#define LOS_SAL_SOCK_RAW         3

#define LOS_SAL_IPPROTO_ICMP     1
#define LOS_SAL_IPPROTO_TCP      6
#define LOS_SAL_IPPROTO_UDP      17

#define LOS_SAL_MAX_SOCKETS      16
#define LOS_SAL_MAX_PROTOS       4

/* 错误码（与 Linux/POSIX 数值一致，Linux ABI 侧可以直接透传） */
#define LOS_SAL_E_NOENT          2
#define LOS_SAL_E_BADF           9
#define LOS_SAL_E_AGAIN          11
#define LOS_SAL_E_NOMEM          12
#define LOS_SAL_E_EXIST          17
#define LOS_SAL_E_INVAL          22
#define LOS_SAL_E_MFILE          24
#define LOS_SAL_E_PROTONOSUPPORT 93
#define LOS_SAL_E_OPNOTSUPP      95
#define LOS_SAL_E_AFNOSUPPORT    97
#define LOS_SAL_E_ISCONN         106
#define LOS_SAL_E_NOTCONN        107

/* 地址：family + **主机序** port + **主机序** IPv4（addr == 0 表示任意地址）。
 * addr 的主机序口径与 LosNetIf.ip 一致：网络序字节 [a,b,c,d] 记成 0xaabbccdd。
 * （早期注释写的"网络序"与实际实现不符 —— lx_sock 的解析/输出和协议栈内部
 * 一直按主机序；M64 新增的 SalGetSockName/SalGetPeerName 依赖这个口径。） */
typedef struct LosSockAddr {
    UINT16 family;
    UINT16 port;
    UINT32 addr;
} LosSockAddr;

/* 协议栈函数表。SAL 只按 (domain, type, proto) 查注册表，然后原样转发。
 * 约定：返回 >= 0 是字节数/0，< 0 是 -errno。 */
typedef struct SalProto {
    const CHAR *name;
    INT32 domain;
    INT32 type;
    INT32 proto;
    INT32 (*socket)(VOID);               /* 建协议侧 socket；priv 由 SAL 填槽位号 */
    INT32 (*close)(VOID *priv);
    INT32 (*bind)(VOID *priv, const LosSockAddr *a);
    INT32 (*connect)(VOID *priv, const LosSockAddr *a);
    INT32 (*listen)(VOID *priv, INT32 backlog);
    INT32 (*accept)(VOID *priv, VOID **child, LosSockAddr *a);
    INT32 (*send)(VOID *priv, const VOID *buf, UINT32 len);
    INT32 (*recv)(VOID *priv, VOID *buf, UINT32 len);
    INT32 (*sendto)(VOID *priv, const VOID *buf, UINT32 len,
                    const LosSockAddr *a);
    INT32 (*recvfrom)(VOID *priv, VOID *buf, UINT32 len, LosSockAddr *a);
    INT32 (*setopt)(VOID *priv, INT32 opt, const VOID *val, UINT32 len);
    INT32 (*poll)(VOID *priv, UINT32 events);
    /* M64：名字查询（getsockname/getpeername）。协议栈没实现的可以留 NULL，
     * SAL 统一回 -EOPNOTSUPP。 */
    INT32 (*getsockname)(VOID *priv, LosSockAddr *a);
    INT32 (*getpeername)(VOID *priv, LosSockAddr *a);
} SalProto;

/* ---- 协议栈注册表 ---- */
INT32  SalRegister(SalProto *p);
INT32  SalUnregister(INT32 domain, INT32 type, INT32 proto);
UINT32 SalProtoCount(VOID);
SalProto *SalProtoGet(UINT32 i);

/* ---- socket 面 ---- */
INT32  SalSocket(INT32 domain, INT32 type, INT32 proto);
INT32  SalClose(INT32 sd);
INT32  SalBind(INT32 sd, const LosSockAddr *a);
INT32  SalConnect(INT32 sd, const LosSockAddr *a);
INT32  SalListen(INT32 sd, INT32 backlog);
INT32  SalAccept(INT32 sd, LosSockAddr *a);
INT32  SalSend(INT32 sd, const VOID *buf, UINT32 len);
INT32  SalRecv(INT32 sd, VOID *buf, UINT32 len);
INT32  SalSendTo(INT32 sd, const VOID *buf, UINT32 len, const LosSockAddr *a);
INT32  SalRecvFrom(INT32 sd, VOID *buf, UINT32 len, LosSockAddr *a);
INT32  SalSetSockOpt(INT32 sd, INT32 opt, const VOID *val, UINT32 len);
INT32  SalPoll(INT32 sd, UINT32 events);
/* M64：本地/对端地址查询（给 Linux ABI 的 getsockname/getpeername 用）。
 * 未 bind/未 connect 时由协议栈层按 Linux 语义填 0.0.0.0:0 或 -ENOTCONN。 */
INT32  SalGetSockName(INT32 sd, LosSockAddr *a);
INT32  SalGetPeerName(INT32 sd, LosSockAddr *a);

/* ---- 查询（调试/诊断用） ---- */
INT32  SalLastError(INT32 sd);
UINT32 SalSocketCount(VOID);
SalProto *SalSockProto(INT32 sd);
VOID  *SalSockPriv(INT32 sd);

#endif
