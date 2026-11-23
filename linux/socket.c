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

/* socketpair 在 Linux 上只支持 AF_UNIX；两端走内核本地对象（afunix.c），
 * 不进 SAL，所以 C6OS_NET=0 的 port 也能用。busybox wget 的 SSL helper
 * 就是靠它把已连接的 HTTPS socket 在 fork/exec 后交给 openssl。 */
u64 lx_sock_socketpair(u64 domain, u64 type, u64 proto, u64 usockvec)
{
    if (domain != 1u) { return (u64)0 - LX_EAFNOSUPPORT; }
    if (proto != 0ull) { return (u64)0 - LX_EINVAL; }
    return lx_unix_socketpair(type, usockvec);
}

#if C6OS_NET
#include "los/los_typedef.h"
#include "los/sal.h"
#include "los/net.h"

#define LX_AF_INET      2u
#define LX_SOCK_STREAM  1u
#define LX_SOCK_DGRAM   2u
#define LX_SOCK_RAW     3u
#define LX_SOL_SOCKET   1u
#define LX_SOCK_MAX     64u

/* SAL sd 上的 fd 引用计数：dup2/fork 复制出的别名各持一份，最后一份关闭时
 * 才 SalClose。sd 是固定小数组的下标，计数表同样静态即可，零动态分配。 */
static u32 g_sock_refs[LOS_SAL_MAX_SOCKETS];

void lx_sock_ref(u64 sd)
{
    if (sd < (u64)LOS_SAL_MAX_SOCKETS && g_sock_refs[sd] != 0u) {
        g_sock_refs[sd]++;
    }
}

void lx_sock_drop(u64 sd)
{
    if (sd < (u64)LOS_SAL_MAX_SOCKETS && g_sock_refs[sd] != 0u) {
        g_sock_refs[sd]--;
        if (g_sock_refs[sd] == 0u) {
            (void)SalClose((int)sd);
        }
    }
}

/* M24：泵轮询式网卡（x86 e1000）。NetIfPollX86 把 RX 环里的帧喂进协议栈，
 * ARP 应答与 DNS 响应才会被处理；没有中断，等待路径必须主动调。
 *
 * 每次泵末尾必须让一下 CPU：e1000 的收包是 QEMU 主循环里 slirp+设备模型
 * 完成的，vCPU 一直占着主循环就排不上队，应答永远填不进 RX 环。outb 到
 * 0x80 会触发一次 PIO VM exit，QEMU 在这期间放锁让主循环跑一轮 —— 这是
 * 这个端口上唯一安全的手段（hlt 不行：int 0x80 进内核时 IF 已被清掉，
 * 且 PIT 回调可能把 syscall 当成任务切换掉）。 */
void lx_net_pump(void)
{
    LosNetIf *nif = NetIfGet(0u);
    u8 zero = 0u;
    u16 port = 0x80u;

    if (nif != (LosNetIf *)0) {
        (void)NetIfPollX86(nif);
    }
    __asm__ __volatile__("outb %0, %1" : : "a"(zero), "Nd"(port));
}

/* M24：单个 socket 的就绪位。UDP 看接收队列计数，TCP 看 SalPoll。 */
u16 lx_sock_poll_one(u64 sd)
{
    int revents;

    /* sd 是 SAL socket id（LxFile.node），不是 fd。SAL 的 POLLOUT 是 2，
     * Linux 的是 4：以前传 4 进去，TCP 的 Established 永远报不出可写。 */
    revents = SalPoll((int)sd, 0x1u | 0x2u);   /* SAL: POLLIN | POLLOUT */
    if (revents < 0) {
        return 0u;
    }
    return (u16)((revents & 0x1u) ? LX_POLLIN : 0u) |
           (u16)((revents & 0x2u) ? LX_POLLOUT : 0u);
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
    p[4] = (u8)((in->addr >> 24) & 0xffu);   /* sin_addr 是网络序 */
    p[5] = (u8)((in->addr >> 16) & 0xffu);
    p[6] = (u8)((in->addr >> 8) & 0xffu);
    p[7] = (u8)(in->addr & 0xffu);
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
        (base_type != LX_SOCK_STREAM && base_type != LX_SOCK_DGRAM &&
         base_type != LX_SOCK_RAW)) {
        return (u64)0 - LX_EAFNOSUPPORT;
    }
    /* Linux 语义：proto=0 表示"按 type 选默认协议"。SAL 按 (domain,type,proto)
     * 三元组精确匹配，注册的 UDP 是 17、TCP 是 6、ICMP raw 是 1，
     * glibc 却传 0 —— 不补默认值就 EAFNOSUPPORT（M18 验收里的 socket A/B）。 */
    if (proto == 0ull) {
        proto = (base_type == LX_SOCK_RAW) ? 1ull :
                (base_type == LX_SOCK_DGRAM) ? 17ull : 6ull;
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
    g_sock_refs[sd] = 1u;                 /* 新 socket 只有一个 fd 引用 */
    return fd;
}

u64 lx_sock_close(u64 sd)
{
    LxFile *f = lxs_get_(sd);

    if (f != (LxFile *)0 && f->kind == LX_FD_UNIX) {
        return lx_unix_close(sd);
    }
    if (sd >= (u64)LOS_SAL_MAX_SOCKETS || g_sock_refs[sd] == 0u) {
        return (u64)0 - LX_EBADF;
    }
    lx_sock_drop(sd);
    return 0ull;
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
    if (uaddrlen != 0ull && lx_user_ok(uaddrlen, 4ull, C6OS_PROT_W) == 0) {
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
    g_sock_refs[csd] = 1u;                /* accept 出的 socket 同样从 1 开始 */
    if (uaddr != 0ull) {
        lxs_addr_out_(&a, uaddr);
    }
    if (uaddrlen != 0ull) {
        /* socklen_t 是 4 字节：写 8 字节会踩掉调用方栈上紧邻的变量
         * （c-ares 的 fromlen 后面就是 recvfrom 的 sockaddr，family 被
         * 清零后源地址校验失败，DNS 应答被整包丢弃）。 */
        *(u32 *)uaddrlen = 16u;
    }
    return cfd;
}

/* M24：SalSend 在 ARP 未解析时返回 -113(EHOSTUNREACH)；TCP 刚发完 SYN 时
 * 返回 -107(ENOTCONN)。轮询式网卡没有中断，用户态同步 send 没有机会泵
 * 网卡收 ARP 应答/SYN-ACK。这里在这两种错误时泵网卡再重试，让握手完成
 * （否则 busybox wget 的 ssl_client 一 exec 就 send，握手必败）。 */
static int sal_send_retry_(int sd, const void *buf, u32 len)
{
    int rc = SalSend(sd, buf, len);
    int i;

    for (i = 0; i < 4000 && (rc == -113 || rc == -107); i++) {
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

    for (i = 0; i < 4000 && (rc == -113 || rc == -107); i++) {
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

/* M24：UDP 空队列返回 -11(EAGAIN)。等待响应时泵网卡收包再重试——让不经过
 * poll 的同步 recv 也能收到 ARP 应答/DNS 响应。
 *
 * 这里本质上是"自旋等网卡"：e1000 没有中断，收包完全靠 vCPU 主动轮询，
 * 而真正收包的是 QEMU 主循环里的 slirp —— 每次泵末尾的 outb 才让主循环
 * 跑一轮。所以"能收到几个包"直接正比于轮询次数：次数给少了 DNS 应答还在
 * 路上就放弃了（nslookup 报超时），给多了纯浪费。500000 次 ≈ slirp 往返
 * 十几次的余量，实测足够覆盖 ARP + DNS + ping 三种往返。
 *
 * 另加一个墙钟兜底，防止 TSC 异常时永远转下去。
 *
 * 非阻塞 socket 必须**立刻**返回 EAGAIN（select/poll 驱动的程序靠它判就绪），
 * 所以 nonblock 时只泵网卡、不做重试。
 *
 * M60：循环里每轮还要调一次 lx_proc_wait_tick()。自旋等待期间没有任何人
 * 会来跑定时器，busybox ping 靠 setitimer(ITIMER_REAL) 发下一个包，不查闹钟
 * 就永远只发第一个包（屏幕上一直刷 "recvfrom: Resource temporarily
 * unavailable"）。顺带也让串口在这段时间里继续泵，Ctrl-C 能打断它。 */
#define LX_NET_SPIN_MAX   500000ull
#define LX_NET_SPIN_GUARD 30000000000ull   /* ≈10s @3GHz，仅作兜底 */

/* 返回 SAL 的原始结果；*intr 置 1 表示被信号打断（调用方报 EINTR）。 */
static int lx_net_spin_(int sd, void *buf, u32 len, LosSockAddr *a,
                        int want_addr, int nonblock, int *intr)
{
    u64 guard = port_cycles() + LX_NET_SPIN_GUARD;
    u64 limit = (nonblock != 0) ? 1ull : LX_NET_SPIN_MAX;
    u64 i = 0ull;
    int rc;

    if (intr != (int *)0) {
        *intr = 0;
    }
    if (want_addr != 0) {
        rc = SalRecvFrom(sd, buf, len, a);
    } else {
        rc = SalRecv(sd, buf, len);
    }
    while (rc == -11 && port_cycles() < guard) {
        /* 轮询预算用完了，但进程还挂着闹钟/定时器：别急着报 EAGAIN，再等
         * 一会儿让它到点。busybox ping 就是靠这个 —— 它 setitimer 之后在
         * recvfrom 里等应答，如果预算先到期，回来的 EAGAIN 会被它打成一行
         * "recvfrom: Resource temporarily unavailable"，而它想看到的是
         * SIGALRM 引发的 EINTR（那个它直接忽略）。没有定时器的进程（比如
         * m18 里对空队列做阻塞 recv 的用例）行为不变，仍然立刻 EAGAIN。 */
        if (i >= limit && lx_proc_alarm_remain() == 0ull) {
            /* 轮询预算用完了、也没有挂着的定时器。这时候还有两种收场：
             *
             *   - 有信号等着投递（典型是 SIGALRM 已经到点、但 busybox 用
             *     signal() 装 handler 带 SA_RESTART，lx_proc_wait_intr 一直
             *     说"继续等"）：必须回用户态让 handler 跑，否则 ping 的
             *     "发下一个包"永远不执行。报 EINTR 是这里唯一正确的说法 ——
             *     阻塞 socket 上 EAGAIN 本身就不是合法结果。
             *   - 什么信号都没有：才按"真的没数据"报 EAGAIN（m18 依赖它）。 */
            if (lx_proc_sig_pending() != 0) {
                if (intr != (int *)0) {
                    *intr = 1;
                }
                return rc;
            }
            break;
        }
        lx_net_pump();
        (void)lx_proc_wait_tick();
        if (lx_proc_wait_intr() != 0) {
            if (intr != (int *)0) {
                *intr = 1;
            }
            return rc;
        }
        if (want_addr != 0) {
            rc = SalRecvFrom(sd, buf, len, a);
        } else {
            rc = SalRecv(sd, buf, len);
        }
        i++;
    }
    return rc;
}

static int sal_recv_retry_(int sd, void *buf, u32 len, int nonblock, int *intr)
{
    return lx_net_spin_(sd, buf, len, (LosSockAddr *)0, 0, nonblock, intr);
}

static int sal_recvfrom_retry_(int sd, void *buf, u32 len, LosSockAddr *a,
                               int nonblock, int *intr)
{
    return lx_net_spin_(sd, buf, len, a, 1, nonblock, intr);
}

u64 lx_sock_recvfrom(u64 fd, u64 ubuf, u64 len, u64 flags,
                     u64 uaddr, u64 uaddrlen)
{
    LxFile *f = lxs_get_(fd);
    int rc;
    int nb;
    int intr = 0;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (f->kind == LX_FD_UNIX) {
        return lx_unix_recvfrom(fd, ubuf, len, uaddr, uaddrlen);
    }
    if (lx_user_ok(ubuf, len, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
    /* MSG_DONTWAIT(0x40) 或 O_NONBLOCK：不许在内核里等。 */
    nb = (((flags & 0x40ull) != 0ull) || ((f->oflags & LX_O_NONBLOCK) != 0u)) ? 1 : 0;
    if (uaddr != 0ull) {
        LosSockAddr a;

        if (lx_user_ok(uaddr, 16ull, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
        rc = sal_recvfrom_retry_((int)f->node, (void *)ubuf, (u32)len, &a,
                                 nb, &intr);
        if (rc >= 0) {
            lxs_addr_out_(&a, uaddr);
            if (uaddrlen != 0ull && lx_user_ok(uaddrlen, 4ull, C6OS_PROT_W) != 0) {
                *(u32 *)uaddrlen = 16u;
            }
        }
    } else {
        rc = sal_recv_retry_((int)f->node, (void *)ubuf, (u32)len, nb, &intr);
    }
    /* 被信号打断（Ctrl-C / SIGALRM）：先回用户态把信号送进去，别把 EAGAIN
     * 当"真的没数据"报给调用方——busybox ping 会把 EAGAIN 打成一行错误，
     * 而归规矩的阻塞 socket 在这种局面下报的是 EINTR。 */
    if (intr != 0) { return (u64)0 - LX_EINTR; }
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
    if (f->kind == LX_FD_UNIX) { return lx_unix_shutdown(fd, how); }
    /* SAL 没有真正的半关闭。以前这里把整个 fd 干掉，busybox wget 在
     * HTTPS 下对 socketpair 调 shutdown(SHUT_WR) 之后读就变成 EBADF；
     * 即使 fd 是 AF_INET，shutdown 也绝不能销毁 fd。这里成功返回即可。 */
    (void)how;
    return 0ull;
}

/* M64：getpeername/getsockname 填真实的 sockaddr_in。
 *
 * 以前 SAL 没有名字查询接口，这里返回 16 个 0（family=0）只求"不报错"；
 * 但 c-ares 1.33 的 ares__conn_set_self_ip() 用 getsockname 的结果判定
 * 地址族，family=0 直接给 ARES_ECONNREFUSED —— curl 的
 * "Could not resolve host" 就是卡在这（TLS/HTTP 本身没问题，--resolve 能通）。
 * 现在 UDP/TCP 都通过 SalGetSockName/SalGetPeerName 回真实的本地端口和
 * 对端地址；AF_UNIX 没有 INET 名字，给一个空路径的 sockaddr_un（family=1）。 */
static u64 lxs_name_(u64 fd, u64 uaddr, u64 uaddrlen, int peer)
{
    LxFile *f = lxs_get_(fd);
    LosSockAddr a;
    int rc = 0;

    if (f == (LxFile *)0) { return (u64)0 - LX_EBADF; }
    if (lx_user_ok(uaddr, 16ull, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
    a.family = LX_AF_INET;
    a.port = 0u;
    a.addr = 0u;
    if (f->kind == LX_FD_UNIX) {
        a.family = 1u;                   /* AF_UNIX */
    } else if (peer != 0) {
        rc = SalGetPeerName((int)f->node, &a);
        if (rc < 0) { return lxs_err_(rc); }
    } else {
        rc = SalGetSockName((int)f->node, &a);
        if (rc < 0) { return lxs_err_(rc); }
    }
    lxs_addr_out_(&a, uaddr);
    if (uaddrlen != 0ull && lx_user_ok(uaddrlen, 4ull, C6OS_PROT_W) != 0) {
        *(u32 *)uaddrlen = 16u;
    }
    return 0ull;
}

u64 lx_sock_getpeername(u64 fd, u64 uaddr, u64 uaddrlen)
{
    return lxs_name_(fd, uaddr, uaddrlen, 1);
}

u64 lx_sock_getsockname(u64 fd, u64 uaddr, u64 uaddrlen)
{
    return lxs_name_(fd, uaddr, uaddrlen, 0);
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
    int nb;
    int intr;

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
    nb = (((flags & 0x40ull) != 0ull) || ((f->oflags & LX_O_NONBLOCK) != 0u)) ? 1 : 0;
    intr = 0;
    if (*(const u64 *)(umsg + 8ull) != 0ull) {
        LosSockAddr a;
        u64 name = *(const u64 *)(umsg + 0ull);

        if (lx_user_ok(name, 16ull, C6OS_PROT_W) == 0) {
            port_kfree((void *)buf);
            return (u64)0 - LX_EFAULT;
        }
        rc = sal_recvfrom_retry_((int)f->node, (void *)buf, (u32)total, &a,
                                 nb, &intr);
        if (rc >= 0) { lxs_addr_out_(&a, name); }
    } else {
        rc = sal_recv_retry_((int)f->node, (void *)buf, (u32)total, nb, &intr);
    }
    if (rc < 0) {
        port_kfree((void *)buf);
        return (intr != 0) ? (u64)0 - LX_EINTR : lxs_err_(rc);
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

/* ---- M59：socket fd 上的 SIOCGIF*（ifconfig）----
 * x86-64 struct ifreq = name[16] + 24 字节 union；写回的 sockaddr 用网络序。 */
#define LX_SIOCGIFCONF    0x8912ull
#define LX_SIOCGIFFLAGS   0x8913ull
#define LX_SIOCSIFFLAGS   0x8914ull
#define LX_SIOCGIFADDR    0x8915ull
#define LX_SIOCGIFBRDADDR 0x8919ull
#define LX_SIOCGIFNETMASK 0x891bull
#define LX_SIOCGIFMTU     0x8921ull
#define LX_SIOCGIFHWADDR  0x8927ull
#define LX_SIOCGIFINDEX   0x8933ull

static void lxs_addr_out_ip_(u64 ureq, u32 ip)
{
    u8 *p = (u8 *)ureq + 16ull;

    p[0] = 2u; p[1] = 0u;                    /* AF_INET */
    p[2] = 0u; p[3] = 0u;                    /* sin_port */
    p[4] = (u8)(ip >> 24); p[5] = (u8)(ip >> 16);
    p[6] = (u8)(ip >> 8);  p[7] = (u8)ip;
}

u64 lx_sock_ioctl(u64 fd, u64 req, u64 arg)
{
    LxFile *f = lxs_get_(fd);
    LosNetIf *nif = (LosNetIf *)0;
    u8 *p = (u8 *)0;
    u64 i;
    u64 cap;
    u64 buf;

    if (f == (LxFile *)0 || f->kind == LX_FD_UNIX) {
        return (u64)0 - LX_EOPNOTSUPP;
    }
    nif = NetIfGet(0u);
    if (nif == (LosNetIf *)0) {
        return (u64)0 - LX_ENOENT;
    }
    if (req == LX_SIOCGIFCONF) {
        if (lx_user_ok(arg, 16ull, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        cap = (u64)*(const u32 *)arg;
        buf = *(const u64 *)(arg + 8ull);
        if (buf == 0ull || cap < 40ull) {
            *(u32 *)arg = 0u;
            return 0ull;
        }
        if (lx_user_ok(buf, 40ull, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        p = (u8 *)buf;
        for (i = 0ull; i < 40ull; i++) { p[i] = 0u; }
        p[0] = 'e'; p[1] = 't'; p[2] = 'h'; p[3] = '0';
        lxs_addr_out_ip_(buf, nif->ip);
        *(u32 *)arg = 40u;
        return 0ull;
    }
    if (lx_user_ok(arg, 40ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    p = (u8 *)arg;
    if (req == LX_SIOCGIFFLAGS) {
        /* IFF_UP | BROADCAST | RUNNING | MULTICAST */
        *(u16 *)(p + 16ull) = 0x1043u;
        return 0ull;
    }
    if (req == LX_SIOCSIFFLAGS) { return 0ull; }  /* 只有 eth0，接受即可 */
    if (req == LX_SIOCGIFADDR)  {
        lxs_addr_out_ip_(arg, nif->ip);
        return 0ull;
    }
    if (req == LX_SIOCGIFNETMASK) {
        lxs_addr_out_ip_(arg, nif->mask);
        return 0ull;
    }
    if (req == LX_SIOCGIFBRDADDR) {
        lxs_addr_out_ip_(arg, nif->ip | ~nif->mask);
        return 0ull;
    }
    if (req == LX_SIOCGIFMTU)    { *(i32 *)(p + 16ull) = 1500;  return 0ull; }
    if (req == LX_SIOCGIFINDEX)  { *(i32 *)(p + 16ull) = 1;      return 0ull; }
    if (req == LX_SIOCGIFHWADDR) {
        /* ifreq.ifr_hwaddr 是 sockaddr：family@16，sa_data@18，MAC 从 18 开始。 */
        p[16] = 1u; p[17] = 0u;              /* ARPHRD_ETHER */
        for (i = 0u; i < 6u; i++) { p[18u + i] = nif->mac[i]; }
        return 0ull;
    }
    return (u64)0 - LX_ENOTTY;
}

#else /* !C6OS_NET */

/* 无网络 port：INET 栈空操作，零侵入；但 AF_UNIX 不依赖 SAL，照常可用。 */
void lx_net_pump(void) { }
void lx_sock_ref(u64 sd) { (void)sd; }
void lx_sock_drop(u64 sd) { (void)sd; }
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
u64 lx_sock_shutdown(u64 fd, u64 h) { return lxs_is_unix_(fd) ? lx_unix_shutdown(fd, h) : (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_getpeername(u64 fd, u64 a, u64 l) { (void)fd; (void)a; (void)l; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_getsockname(u64 fd, u64 a, u64 l) { (void)fd; (void)a; (void)l; return (u64)0 - LX_EOPNOTSUPP; }
u64 lx_sock_ioctl(u64 fd, u64 req, u64 arg)
{
    (void)fd; (void)req; (void)arg;
    return (u64)0 - LX_EOPNOTSUPP;
}
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
