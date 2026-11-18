/* linux/socket.c —— M18：Linux socket syscall → LiteOS SAL
 *
 * 架构：socket fd 直接复用主 fd 表（linux/fdt.c 的 LxFdTab），kind=LX_FD_SOCK、
 * node=SAL socket id。这样 socket fd 和文件/pipe fd 共用同一个命名空间，
 * dup/close/fork 继承/exec CLOEXEC 全部自动正确，不需要另立映射表。
 * sys.c 只负责拆用户指针，Linux↔SAL 语义转换（sockaddr_in 布局、字节序、
 * errno）都在这里。C6OS_NET=0 时全部返回 -EAFNOSUPPORT，其它 port 零侵入。
 *
 * close：lx_proc_close 看到 kind==LX_FD_SOCK 会调 lx_sock_close 释放 SAL sd。
 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

/* M24：轮询式网卡泵。syscall 等待路径（poll/select/recv）每轮调一次，
 * 才能收到 ARP 应答与 UDP 响应——纯轮询驱动没有中断，不泵就永远收不到。 */
void lx_net_pump(void);

/* 取 socket fd 对应的 LxFile（AF_INET 的 LX_FD_SOCK 或 AF_UNIX 的
 * LX_FD_UNIX 都接受）。放在 #if C6OS_NET 之外，桩段也要用。 */
static LxFile *lxs_get_(u64 fd)
{
    LxProc *p = lx_cur_proc();
    LxFile *f;

    if (p == (LxProc *)0) {
        return (LxFile *)0;
    }
    f = lx_fdtab_get(p->fd, fd);
    if (f == (LxFile *)0 ||
        (f->kind != LX_FD_SOCK && f->kind != LX_FD_UNIX)) {
        return (LxFile *)0;
    }
    return f;
}

#if C6OS_NET
#include "los/los_typedef.h"
#include "los/sal.h"
#include "los/net.h"

#define LX_AF_INET      2u
#define LX_SOCK_STREAM  1u
#define LX_SOCK_DGRAM   2u
#define LX_SOL_SOCKET   1u
#define LX_SOCK_MAX     64u

/* M24：泵轮询式网卡（x86 e1000）。NetIfPollX86 把 RX 环里的帧喂进协议栈，
 * ARP 应答与 DNS 响应才会被处理；没有中断，等待路径必须主动调。 */
void lx_net_pump(void)
{
    LosNetIf *nif = NetIfGet(0u);

    if (nif != (LosNetIf *)0) {
        (void)NetIfPollX86(nif);
    }
}

/* M24：单个 socket 的就绪位。UDP 看接收队列计数，TCP 看 SalPoll。 */
u16 lx_sock_poll_one(u64 sd)
{
    LxFile *f = lxs_get_(sd);
    int revents;

    if (f == (LxFile *)0) {
        return LX_POLLNVAL;
    }
    if (f->kind == LX_FD_UNIX) {
        return lx_unix_poll(f->node);
    }
    revents = SalPoll((int)f->node, 0x1u | 0x4u);   /* POLLIN | POLLOUT */
    if (revents < 0) {
        return 0u;
    }
    return (u16)((revents & 0x1u) ? LX_POLLIN : 0u) |
           (u16)((revents & 0x4u) ? LX_POLLOUT : 0u);
}

static u64 lxs_err_(int sal_rc)
{
    /* SAL 返回 -errno（POSIX 数值），Linux ABI 同口径，直接透传。 */
    return (sal_rc >= 0) ? (u64)sal_rc : (u64)0 - (u64)(-sal_rc);
}

/* Linux struct sockaddr_in: family(2) port(2,网络序) addr(4,网络序) zero[8]。
 * LosSockAddr: family(2) port(2,主机序) addr(4,网络序)。port 要 ntohs。 */
static void lxs_addr_in_(u64 uaddr, LosSockAddr *out)
{
    const u8 *p = (const u8 *)uaddr;
    u16 fam = (u16)((u16)p[0] | ((u16)p[1] << 8));
    u16 port_be = (u16)((u16)p[2] | ((u16)p[3] << 8));

    out->family = fam;
    out->port = (u16)((port_be >> 8) | (port_be << 8));
    /* 网络序字节数组 [a,b,c,d] → 主机序 0xaabbccdd（和 nif->ip 同口径）。 */
    out->addr = ((u32)p[4] << 24) | ((u32)p[5] << 16) |
                ((u32)p[6] << 8) | (u32)p[7];
}

static void lxs_addr_out_(const LosSockAddr *in, u64 uaddr)
{
    u8 *p = (u8 *)uaddr;
    u16 port_be = (u16)((in->port >> 8) | (in->port << 8));
    u64 i;

    p[0] = (u8)(in->family & 0xffu);
    p[1] = (u8)((in->family >> 8) & 0xffu);
    p[2] = (u8)(port_be & 0xffu);
    p[3] = (u8)((port_be >> 8) & 0xffu);
    p[4] = (u8)(in->addr & 0xffu);
    p[5] = (u8)((in->addr >> 8) & 0xffu);
    p[6] = (u8)((in->addr >> 16) & 0xffu);
    p[7] = (u8)((in->addr >> 24) & 0xffu);
    for (i = 8ull; i < 16ull; i++) {
        p[i] = 0u;
    }
}

u64 lx_sock_socket(u64 domain, u64 type, u64 proto)
{
    LxProc *p = lx_cur_proc();
    u64 base_type = type & ~(u64)(0x80000ull | 0x800ull);  /* 去 CLOEXEC/NONBLOCK */
    u64 want_flags = type & (u64)(0x80000ull | 0x800ull);
    int sd;
    u64 fd;

    if (p == (LxProc *)0) {
        return (u64)0 - LX_EBADF;
    }
    /* M25：AF_UNIX(1) 走内核本地对象（linux/afunix.c），不进 SAL。 */
    if (domain == 1u) {
        return lx_unix_socket(type);
    }
    /* M24：glibc 的 getaddrinfo 用 socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC|
     * SOCK_NONBLOCK, 0)；严格比较 type 会把这些标志当成非法 type。 */
    if (domain != LX_AF_INET ||
        (base_type != LX_SOCK_STREAM && base_type != LX_SOCK_DGRAM)) {
        return (u64)0 - LX_EAFNOSUPPORT;
    }
    /* Linux 语义：proto=0 表示"按 type 选默认协议"。SAL 按 (domain,type,proto)
     * 三元组精确匹配，注册的 UDP 是 17、TCP 是 6，glibc 却传 0 —— 不补默认
     * 值就 EAFNOSUPPORT（M18 验收里 socket A/B 失败的原因）。 */
    if (proto == 0ull) {
        proto = (base_type == LX_SOCK_DGRAM) ? 17ull : 6ull;
    }
    sd = SalSocket((int)domain, (int)base_type, (int)proto);
    if (sd < 0) {
        return lxs_err_(sd);
    }
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) {
        (void)SalClose(sd);
        return (u64)0 - LX_EMFILE;
    }
    {
        LxFile *f = &p->fd->ent[fd];   /* alloc 出的槽 node 还是 NONE，不能用 get */

        f->node = (u64)sd;                 /* node = SAL socket id */
        f->pos = 0ull;
        f->is_dir = 0u;
        f->kind = LX_FD_SOCK;
        f->flags = (want_flags & 0x80000ull) ? LX_FD_CLOEXEC : 0u;
        f->oflags = (want_flags & 0x800ull) ? LX_O_NONBLOCK : 0u;
    }
    return fd;
}

u64 lx_sock_close(u64 sd)
{
    LxFile *f = lxs_get_(sd);

    if (f != (LxFile *)0 && f->kind == LX_FD_UNIX) {
        return lx_unix_close(sd);
    }
    return lxs_err_(SalClose((int)sd));
}

u64 lx_sock_bind(u64 fd, u64 uaddr, u64 ulen)
{
    LxFile *f = lxs_get_(fd);
    LosSockAddr a;
    int rc;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) {
        return lx_unix_bind(fd, uaddr, ulen);
    }
    if (lx_user_ok(uaddr, 16ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    (void)ulen;
    lxs_addr_in_(uaddr, &a);
    rc = SalBind((int)f->node, &a);
    return lxs_err_(rc);
}

u64 lx_sock_connect(u64 fd, u64 uaddr, u64 ulen)
{
    LxFile *f = lxs_get_(fd);
    LosSockAddr a;
    int rc;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) {
        return lx_unix_connect(fd, uaddr, ulen);
    }
    if (lx_user_ok(uaddr, 16ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    (void)ulen;
    lxs_addr_in_(uaddr, &a);
    rc = SalConnect((int)f->node, &a);
    return lxs_err_(rc);
}

u64 lx_sock_listen(u64 fd, u64 backlog)
{
    LxFile *f = lxs_get_(fd);

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) { return lx_unix_listen(fd, backlog); }
    return lxs_err_(SalListen((int)f->node, (int)backlog));
}

u64 lx_sock_accept(u64 fd, u64 uaddr, u64 uaddrlen)
{
    LxFile *f = lxs_get_(fd);
    LxProc *p = lx_cur_proc();
    LosSockAddr a;
    int csd;
    u64 cfd;

    if (f == (LxFile *)0 || p == (LxProc *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) { return lx_unix_accept(fd, uaddr, uaddrlen); }
    if (uaddr != 0ull && lx_user_ok(uaddr, 16ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    if (uaddrlen != 0ull && lx_user_ok(uaddrlen, 8ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    csd = SalAccept((int)f->node, &a);
    if (csd < 0) { return lxs_err_(csd); }
    cfd = lx_fdtab_alloc(p->fd);
    if (cfd == LX_FD_NONE) {
        (void)SalClose(csd);
        return (u64)0 - LX_EMFILE;
    }
    {
        LxFile *cf = &p->fd->ent[cfd];   /* alloc 出的槽 node 还是 NONE */

        cf->node = (u64)csd;
        cf->pos = 0ull;
        cf->is_dir = 0u;
        cf->kind = LX_FD_SOCK;
        cf->flags = 0u;
        cf->oflags = 0u;
    }
    if (uaddr != 0ull) {
        lxs_addr_out_(&a, uaddr);
    }
    if (uaddrlen != 0ull) {
        *(u64 *)uaddrlen = 16ull;
    }
    return cfd;
}

/* M24：SalSend 在 ARP 未解析时返回 -113(EHOSTUNREACH)。轮询式网卡没有
 * 中断，用户态同步 send 没有机会泵网卡收 ARP 应答，第一个 DNS 包永远
 * 发不出去。这里在 -113 时泵网卡再重试，让 ARP 有时间完成。 */
static int sal_send_retry_(int sd, const void *buf, u32 len)
{
    int rc = SalSend(sd, buf, len);
    int i;

    for (i = 0; i < 4000 && rc == -113; i++) {
        lx_net_pump();
        rc = SalSend(sd, buf, len);
    }
    return rc;
}

static int sal_sendto_retry_(int sd, const void *buf, u32 len,
                             const LosSockAddr *a)
{
    int rc = SalSendTo(sd, buf, len, a);
    int i;

    for (i = 0; i < 4000 && rc == -113; i++) {
        lx_net_pump();
        rc = SalSendTo(sd, buf, len, a);
    }
    return rc;
}

static u64 lxs_send_ret_(int rc, u64 want)
{
    /* SAL 外部发送成功返回 0（已交给 IP 层排队），环回返回 len；
     * Linux send/sendto/sendmsg 的返回值一律是"发送的字节数"。 */
    if (rc == 0) {
        return want;
    }
    return lxs_err_(rc);
}

u64 lx_sock_sendto(u64 fd, u64 ubuf, u64 len, u64 flags,
                   u64 uaddr, u64 ulen)
{
    LxFile *f = lxs_get_(fd);
    int rc;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) {
        return lx_unix_sendto(fd, ubuf, len, uaddr, ulen);
    }
    if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    (void)flags;
    if (uaddr != 0ull) {
        LosSockAddr a;

        if (lx_user_ok(uaddr, 16ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
        (void)ulen;
        lxs_addr_in_(uaddr, &a);
        rc = sal_sendto_retry_((int)f->node, (const void *)ubuf, (u32)len, &a);
    } else {
        rc = sal_send_retry_((int)f->node, (const void *)ubuf, (u32)len);
    }
    return lxs_send_ret_(rc, len);
}

/* M24：UDP 空队列返回 -113? 不，返回 -11(EAGAIN)。等待响应时泵网卡收包，
 * 再试一次——让不经过 poll 的同步 recv（如 SO_RCVTIMEO 循环）也能收到
 * ARP 应答/DNS 响应。 */
static int sal_recv_retry_(int sd, void *buf, u32 len)
{
    int rc = SalRecv(sd, buf, len);
    int i;

    for (i = 0; i < 2000 && rc == -11; i++) {
        lx_net_pump();
        rc = SalRecv(sd, buf, len);
    }
    return rc;
}

static int sal_recvfrom_retry_(int sd, void *buf, u32 len, LosSockAddr *a)
{
    int rc = SalRecvFrom(sd, buf, len, a);
    int i;

    for (i = 0; i < 2000 && rc == -11; i++) {
        lx_net_pump();
        rc = SalRecvFrom(sd, buf, len, a);
    }
    return rc;
}

u64 lx_sock_recvfrom(u64 fd, u64 ubuf, u64 len, u64 flags,
                     u64 uaddr, u64 uaddrlen)
{
    LxFile *f = lxs_get_(fd);
    int rc;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) {
        return lx_unix_recvfrom(fd, ubuf, len, uaddr, uaddrlen);
    }
    if (lx_user_ok(ubuf, len, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
    (void)flags;
    if (uaddr != 0ull) {
        LosSockAddr a;

        if (lx_user_ok(uaddr, 16ull, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
        rc = sal_recvfrom_retry_((int)f->node, (void *)ubuf, (u32)len, &a);
        if (rc >= 0) {
            lxs_addr_out_(&a, uaddr);
            if (uaddrlen != 0ull && lx_user_ok(uaddrlen, 8ull, C6OS_PROT_W) != 0) {
                *(u64 *)uaddrlen = 16ull;
            }
        }
    } else {
        rc = sal_recv_retry_((int)f->node, (void *)ubuf, (u32)len);
    }
    return lxs_err_(rc);
}

u64 lx_sock_send(u64 fd, u64 ubuf, u64 len, u64 flags)
{
    return lx_sock_sendto(fd, ubuf, len, flags, 0ull, 0ull);
}

u64 lx_sock_recv(u64 fd, u64 ubuf, u64 len, u64 flags)
{
    return lx_sock_recvfrom(fd, ubuf, len, flags, 0ull, 0ull);
}

u64 lx_sock_setsockopt(u64 fd, u64 level, u64 optname, u64 uval, u64 optlen)
{
    LxFile *f = lxs_get_(fd);
    int val;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (level != LX_SOL_SOCKET || optlen < 4ull ||
        lx_user_ok(uval, 4ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EINVAL;
    }
    val = (int)*(const i32 *)uval;
    (void)optname;                            /* SAL 目前只有一个通用 setopt */
    return lxs_err_(SalSetSockOpt((int)f->node, 0, (const void *)&val, 4u));
}

u64 lx_sock_getsockopt(u64 fd, u64 level, u64 optname, u64 uval, u64 uoptlen)
{
    LxFile *f = lxs_get_(fd);

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    (void)level; (void)optname; (void)uval;
    /* 没有对应查询：长度写 0、成功返回（比 ENOPROTOOPT 更兼容探测代码）。 */
    if (lx_user_ok(uoptlen, 4ull, C6OS_PROT_W) != 0) {
        *(u32 *)uoptlen = 0u;
    }
    return 0ull;
}

u64 lx_sock_shutdown(u64 fd, u64 how)
{
    LxFile *f = lxs_get_(fd);

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    (void)how;                                /* SAL 无 shutdown：直接关掉 */
    (void)SalClose((int)f->node);
    f->node = LX_FD_NONE;
    f->kind = LX_FD_FILE;
    return 0ull;
}

/* getpeername/getsockname：SAL 没有对端/本地地址查询接口，返回全 0 sockaddr，
 * 让调用方不报错即可（真实连接地址由协议栈内部使用，不影响 echo 验收）。 */
static u64 lxs_name_(u64 fd, u64 uaddr, u64 uaddrlen)
{
    LxFile *f = lxs_get_(fd);
    u64 i;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (lx_user_ok(uaddr, 16ull, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
    for (i = 0ull; i < 16ull; i++) {
        *(u8 *)(uaddr + i) = 0u;
    }
    if (uaddrlen != 0ull && lx_user_ok(uaddrlen, 8ull, C6OS_PROT_W) != 0) {
        *(u64 *)uaddrlen = 16ull;
    }
    return 0ull;
}

u64 lx_sock_getpeername(u64 fd, u64 uaddr, u64 uaddrlen)
{
    return lxs_name_(fd, uaddr, uaddrlen);
}

u64 lx_sock_getsockname(u64 fd, u64 uaddr, u64 uaddrlen)
{
    return lxs_name_(fd, uaddr, uaddrlen);
}

/* ---- M23：sendmsg/recvmsg：iovec 展开到内核中转缓冲 ----
 * x86-64 msghdr 56B：name@0 namelen@8 iov@16 iovlen@24 control@32
 * controllen@40 flags@48；iovec 16B：base@0 len@8。
 *  ancillary(control) 数据暂忽略（返回时 controllen=0）；把多段 iovec
 *  拼成单个内核缓冲，一次 SalSend/SalRecv，保证 UDP 是一个数据报。 */
#define LX_IOV_MAX   64u
#define LX_MSG_BUFMAX 65536u

u64 lx_sock_sendmsg(u64 fd, u64 umsg, u64 flags)
{
    LxFile *f = lxs_get_(fd);
    u64 iov, niov, total = 0ull;
    u8 *buf;
    u32 i;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) { return lx_unix_sendmsg(fd, umsg, flags); }
    if (lx_user_ok(umsg, 56ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    iov = *(const u64 *)(umsg + 16ull);
    niov = *(const u64 *)(umsg + 24ull);
    if (niov == 0ull || niov > LX_IOV_MAX) { return (u64)0 - LX_EINVAL; }
    if (lx_user_ok(iov, 16ull * niov, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    for (i = 0u; i < (u32)niov; i++) {
        u64 len = *(const u64 *)(iov + (u64)i * 16ull + 8ull);

        total += len;
        if (total > LX_MSG_BUFMAX) { return (u64)0 - LX_EINVAL; }
    }
    buf = (u8 *)port_kcalloc(1ull, total ? total : 1ull);
    if (buf == (u8 *)0) { return (u64)0 - LX_ENOMEM; }
    /* 按 iovec 顺序把用户缓冲段拼到内核中转缓冲 */
    {
        u64 pos = 0ull;

        for (i = 0u; i < (u32)niov; i++) {
            u64 base = *(const u64 *)(iov + (u64)i * 16ull);
            u64 len = *(const u64 *)(iov + (u64)i * 16ull + 8ull);
            u64 k;

            if (len == 0ull) { continue; }
            if (lx_user_ok(base, len, C6OS_PROT_R) == 0) {
                port_kfree((void *)buf);
                return (u64)0 - LX_EFAULT;
            }
            for (k = 0ull; k < len; k++) {
                buf[pos + k] = *(const u8 *)(base + k);
            }
            pos += len;
        }
    }
    {
        int rc;

        if (*(const u64 *)(umsg + 8ull) != 0ull) {   /* msg_namelen != 0 */
            LosSockAddr a;
            u64 name = *(const u64 *)(umsg + 0ull);

            if (lx_user_ok(name, 16ull, C6OS_PROT_R) == 0) {
                port_kfree((void *)buf);
                return (u64)0 - LX_EFAULT;
            }
            lxs_addr_in_(name, &a);
            rc = sal_sendto_retry_((int)f->node, (const void *)buf, (u32)total, &a);
        } else {
            rc = sal_send_retry_((int)f->node, (const void *)buf, (u32)total);
        }
        port_kfree((void *)buf);
        (void)flags;
        return lxs_send_ret_(rc, total);
    }
}

u64 lx_sock_recvmsg(u64 fd, u64 umsg, u64 flags)
{
    LxFile *f = lxs_get_(fd);
    u64 iov, niov, total = 0ull;
    u8 *buf;
    u32 i;
    int rc;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) { return lx_unix_recvmsg(fd, umsg, flags); }
    if (lx_user_ok(umsg, 56ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    iov = *(const u64 *)(umsg + 16ull);
    niov = *(const u64 *)(umsg + 24ull);
    if (niov == 0ull || niov > LX_IOV_MAX) { return (u64)0 - LX_EINVAL; }
    if (lx_user_ok(iov, 16ull * niov, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    for (i = 0u; i < (u32)niov; i++) {
        total += *(const u64 *)(iov + (u64)i * 16ull + 8ull);
        if (total > LX_MSG_BUFMAX) { return (u64)0 - LX_EINVAL; }
    }
    buf = (u8 *)port_kcalloc(1ull, total ? total : 1ull);
    if (buf == (u8 *)0) { return (u64)0 - LX_ENOMEM; }
    if (*(const u64 *)(umsg + 8ull) != 0ull) {
        LosSockAddr a;
        u64 name = *(const u64 *)(umsg + 0ull);

        if (lx_user_ok(name, 16ull, C6OS_PROT_W) == 0) {
            port_kfree((void *)buf);
            return (u64)0 - LX_EFAULT;
        }
        rc = sal_recvfrom_retry_((int)f->node, (void *)buf, (u32)total, &a);
        if (rc >= 0) { lxs_addr_out_(&a, name); }
    } else {
        rc = sal_recv_retry_((int)f->node, (void *)buf, (u32)total);
    }
    if (rc < 0) {
        port_kfree((void *)buf);
        return lxs_err_(rc);
    }
    /* 按 iovec 分散回填，返回实际收到的字节数（可能小于 total） */
    {
        u64 pos = 0ull;
        u64 got = (u64)rc;
        u64 remain = got;

        for (i = 0u; i < (u32)niov && remain > 0ull; i++) {
            u64 base = *(const u64 *)(iov + (u64)i * 16ull);
            u64 len = *(const u64 *)(iov + (u64)i * 16ull + 8ull);
            u64 k = (len < remain) ? len : remain;

            if (lx_user_ok(base, k, C6OS_PROT_W) == 0) {
                port_kfree((void *)buf);
                return (u64)0 - LX_EFAULT;
            }
            for (pos = 0ull; pos < k; pos++) {
                *(u8 *)(base + pos) = buf[got - remain + pos];
            }
            remain -= k;
        }
    }
    port_kfree((void *)buf);
    (void)flags;
    /* msg_flags/controllen 归零 */
    if (lx_user_ok(umsg + 40ull, 16ull, C6OS_PROT_W) != 0) {
        *(u64 *)(umsg + 40ull) = 0ull;      /* msg_controllen */
        *(u64 *)(umsg + 48ull) = 0ull;      /* msg_flags */
    }
    return (u64)rc;
}

#else /* !C6OS_NET */

/* 无网络 port：INET 栈空操作，零侵入；但 AF_UNIX 不依赖 SAL，照常可用。 */
void lx_net_pump(void) { }
u16 lx_sock_poll_one(u64 sd)
{
    LxFile *f = lxs_get_(sd);
    return (f != (LxFile *)0 && f->kind == LX_FD_UNIX) ? lx_unix_poll(f->node) : 0u;
}

/* 桩段里判断 fd 是不是 AF_UNIX（复用主 fd 表的 kind） */
static int lxs_is_unix_(u64 fd)
{
    LxProc *p = lx_cur_proc();
    LxFile *f;

    if (p == (LxProc *)0) { return 0; }
    f = lx_fdtab_get(p->fd, fd);
    return (f != (LxFile *)0 && f->kind == LX_FD_UNIX) ? 1 : 0;
}

u64 lx_sock_socket(u64 d, u64 t, u64 p)
{
    (void)p;
    return (d == 1u) ? lx_unix_socket(t) : (u64)0 - LX_EAFNOSUPPORT;
}
u64 lx_sock_close(u64 fd)
{
    return lxs_is_unix_(fd) ? lx_unix_close(fd) : (u64)0 - LX_EBADF;
}
u64 lx_sock_bind(u64 fd, u64 a, u64 l)
{
    return lxs_is_unix_(fd) ? lx_unix_bind(fd, a, l) : (u64)0 - LX_EAFNOSUPPORT;
}
u64 lx_sock_connect(u64 fd, u64 a, u64 l)
{
    return lxs_is_unix_(fd) ? lx_unix_connect(fd, a, l) : (u64)0 - LX_EAFNOSUPPORT;
}
u64 lx_sock_listen(u64 fd, u64 b)
{
    LxFile *f = lxs_get_(fd);
    return (f != (LxFile *)0 && f->kind == LX_FD_UNIX)
           ? lx_unix_listen(fd, b) : (u64)0 - LX_EOPNOTSUPP;
}
u64 lx_sock_accept(u64 fd, u64 a, u64 l)
{
    LxFile *f = lxs_get_(fd);
    return (f != (LxFile *)0 && f->kind == LX_FD_UNIX)
           ? lx_unix_accept(fd, a, l) : (u64)0 - LX_EOPNOTSUPP;
}
u64 lx_sock_sendto(u64 fd, u64 b, u64 l, u64 f, u64 a, u64 al)
{
    (void)f;
    return lxs_is_unix_(fd) ? lx_unix_sendto(fd, b, l, a, al)
                            : (u64)0 - LX_EAFNOSUPPORT;
}
u64 lx_sock_recvfrom(u64 fd, u64 b, u64 l, u64 f, u64 a, u64 al)
{
    (void)f;
    return lxs_is_unix_(fd) ? lx_unix_recvfrom(fd, b, l, a, al)
                            : (u64)0 - LX_EAFNOSUPPORT;
}
u64 lx_sock_send(u64 fd, u64 b, u64 l, u64 f) { return lx_sock_sendto(fd, b, l, f, 0ull, 0ull); }
u64 lx_sock_recv(u64 fd, u64 b, u64 l, u64 f) { return lx_sock_recvfrom(fd, b, l, f, 0ull, 0ull); }
u64 lx_sock_setsockopt(u64 fd, u64 l, u64 o, u64 v, u64 ol) { (void)fd; (void)l; (void)o; (void)v; (void)ol; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_getsockopt(u64 fd, u64 l, u64 o, u64 v, u64 ol) { (void)fd; (void)l; (void)o; (void)v; (void)ol; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_shutdown(u64 fd, u64 h) { (void)fd; (void)h; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_getpeername(u64 fd, u64 a, u64 l) { (void)fd; (void)a; (void)l; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_getsockname(u64 fd, u64 a, u64 l) { (void)fd; (void)a; (void)l; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_sendmsg(u64 fd, u64 m, u64 f)
{
    LxFile *x = lxs_get_(fd);
    return (x != (LxFile *)0 && x->kind == LX_FD_UNIX)
           ? lx_unix_sendmsg(fd, m, f) : (u64)0 - LX_EOPNOTSUPP;
}
u64 lx_sock_recvmsg(u64 fd, u64 m, u64 f)
{
    LxFile *x = lxs_get_(fd);
    return (x != (LxFile *)0 && x->kind == LX_FD_UNIX)
           ? lx_unix_recvmsg(fd, m, f) : (u64)0 - LX_EOPNOTSUPP;
}

#endif
