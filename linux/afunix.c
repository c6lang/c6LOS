/* linux/afunix.c —— M25：AF_UNIX socket（Linux ABI → 内核本地对象）
 *
 * 架构：与 AF_INET 的 SAL 路径并列，不侵入 SAL。AF_UNIX socket 走主 fd 表
 * （LX_FD_UNIX，node=对象 id），dup/close/fork/CLOEXEC 全部自动正确。
 *
 * 对象模型（内核堆动态表，不占 .bss）：
 *   - path：bind 的文件系统路径（Linux AF_UNIX 名字空间，不落真实 VFS）
 *   - peer：connect 到的对端 id
 *   - 定长环形队列（每槽 4KB，8 槽）：SOCK_DGRAM 一条消息一槽；
 *     SOCK_STREAM 也按消息成槽（简化：保留边界），足够 shell/dbus/syslog
 *     这类小消息场景。
 * 阻塞语义：空队列 recv 走 lx_proc_block()，send/recv 唤醒对端。
 *
 * 不做：SCM_RIGHTS ancillary、抽象名字空间('\0')、OOB、真正字节流的
 * 任意切分 —— 记为后续。C6OS_NET=0 时本文件仍编（AF_UNIX 不依赖网络）。
 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

#define UNIX_PATH_MAX 108u
#define UNIX_QDEPTH   8u
#define UNIX_MSG_MAX  4096u
#define UNIX_MAX      64u

/* 每条消息可附带一个 SCM_RIGHTS fd：存 LxFile 的值副本（含 kind/node/pos/
 * flags），接收方 install 到自己 fd 表；管道类 fd 需要 pipe_ref_ 增引用，
 * 但那是 proc.c 私有函数，这里通过 lx_unix_fd_pass_ 钩子完成。 */
#define UNIX_PASS_MAX 4u

typedef struct {
    u8  data[UNIX_MSG_MAX];
    u32 len;
    u32 off;                          /* STREAM：已经读走的字节数（部分读保留） */
    u32 used;
    u32 nfd;                          /* 附带的 fd 个数 */
    LxFile fds[UNIX_PASS_MAX];        /* fd 状态副本 */
} UnixMsg;

#define UNIX_BACKLOG 8u

typedef struct {
    char path[UNIX_PATH_MAX];
    u32  bound;
    u32  abstract;                   /* sun_path[0]=='\0'：抽象名字空间 */
    u32  len;                        /* 绑定名字长度（抽象名可含 NUL，不能靠 strlen） */
    u32  type;                       /* LX_SOCK_DGRAM / LX_SOCK_STREAM */
    u32  listening;                  /* STREAM：已 listen */
    u64  peer;                       /* 对端对象 id，~0=未连接 */
    UnixMsg q[UNIX_QDEPTH];
    u32  head;
    u32  tail;
    u32  count;
    u32  used;
    u64  pending[UNIX_BACKLOG];      /* STREAM listen：待 accept 的对端 id */
    u32  phead;
    u32  ptail;
    u32  pcount;
    u64  listener;                   /* STREAM：accept 用的监听对端 */
} UnixSock;

static UnixSock *g_unix;
static u32 g_unix_cap;

/* ---- 小工具（内核 libc 无 strlen/strncmp）---- */
static u32 lx_str_len_(const char *s, u32 cap)
{
    u32 n = 0u;

    while (n < cap && s[n] != '\0') { n++; }
    return n;
}

static int lx_str_eq_(const char *a, const char *b, u32 n)
{
    u32 i;

    for (i = 0u; i < n; i++) {
        if (a[i] != b[i]) { return 0; }
    }
    return 1;
}

static UnixSock *unix_at_(u64 id)
{
    if (g_unix == (UnixSock *)0 || id >= g_unix_cap || g_unix[id].used == 0u) {
        return (UnixSock *)0;
    }
    return &g_unix[id];
}

static int unix_grow_(void)
{
    u32 ncap = (g_unix_cap == 0u) ? 8u : g_unix_cap * 2u;
    UnixSock *ne;

    if (ncap > UNIX_MAX) { return -1; }
    ne = (UnixSock *)port_kcalloc((u64)ncap, (u64)sizeof(UnixSock));
    if (ne == (UnixSock *)0) { return -1; }
    if (g_unix != (UnixSock *)0) {
        u32 i;

        for (i = 0u; i < g_unix_cap; i++) { ne[i] = g_unix[i]; }
        port_kfree((void *)g_unix);
    }
    g_unix = ne;
    g_unix_cap = ncap;
    return 0;
}

static UnixSock *unix_by_path_(const char *path, u32 len, u32 abstract)
{
    u32 i;

    if (g_unix == (UnixSock *)0) { return (UnixSock *)0; }
    for (i = 0u; i < g_unix_cap; i++) {
        UnixSock *s = &g_unix[i];
        u32 slen = (s->abstract != 0u) ? s->len : lx_str_len_(s->path, UNIX_PATH_MAX);

        if (s->used != 0u && s->bound != 0u && s->abstract == abstract &&
            slen == len && lx_str_eq_(s->path, path, len)) {
            return s;
        }
    }
    return (UnixSock *)0;
}

/* 由 fd 取对象（必须 LX_FD_UNIX） */
static UnixSock *unix_fd_(u64 fd)
{
    LxProc *p = lx_cur_proc();
    LxFile *f;

    if (p == (LxProc *)0) { return (UnixSock *)0; }
    f = lx_fdtab_get(p->fd, fd);
    if (f == (LxFile *)0 || f->kind != LX_FD_UNIX) { return (UnixSock *)0; }
    return unix_at_(f->node);
}

/* 读 sockaddr_un：family(u16)@0 + path@2，最多 106 字节。
 * 普通路径：以 NUL 结尾，长度靠 strlen。
 * 抽象名字空间：sun_path[0]=='\0'，名字是后面 ulen-2 个字节（可含 NUL、
 * 不以 NUL 结尾），长度由 addrlen 给出 —— 这是 dbus/systemd 用的形式。
 * out_abs 输出是否抽象；抽象时 path[0] 存名字首字节，长度用 outlen。 */
static int unix_addr_(u64 uaddr, u64 ulen, char *path, u32 *outlen, u32 *out_abs)
{
    u8 *p;
    u32 len;

    *out_abs = 0u;
    if (lx_user_ok(uaddr, 2ull, C6OS_PROT_R) == 0) { return -1; }
    p = (u8 *)uaddr;
    if (((u16)p[0] | ((u16)p[1] << 8)) != 1u) { return -1; }  /* AF_UNIX=1 */
    if (ulen > 110ull) { ulen = 110ull; }
    if (ulen <= 2ull) { return -1; }
    if (p[2] == 0u) {                       /* 抽象名字空间 */
        len = (u32)ulen - 2u;
        if (len == 0u || len > UNIX_PATH_MAX - 2u) { return -1; }
        if (lx_user_ok(uaddr + 2ull, len, C6OS_PROT_R) == 0) { return -1; }
        for (*outlen = 0u; *outlen < len; (*outlen)++) {
            path[*outlen] = (char)p[2u + *outlen];
        }
        *out_abs = 1u;
        return 0;
    }
    len = lx_str_len_((const char *)(p + 2u), UNIX_PATH_MAX - 2u);
    if (len == 0u || len > UNIX_PATH_MAX - 2u) { return -1; }
    for (*outlen = 0u; *outlen < len; (*outlen)++) {
        path[*outlen] = (char)p[2u + *outlen];
    }
    path[*outlen] = '\0';
    return 0;
}

u64 lx_unix_socket(u64 type)
{
    LxProc *p = lx_cur_proc();
    u64 base = type & ~(u64)(0x80000ull | 0x800ull);
    u64 fd;
    u32 i;

    if (p == (LxProc *)0 || (base != 1u && base != 2u)) {
        return (u64)0 - LX_EINVAL;   /* 仅 STREAM(1)/DGRAM(2) */
    }
    for (i = 0u; i < g_unix_cap; i++) {
        if (g_unix[i].used == 0u) { break; }
    }
    if (i >= g_unix_cap) {
        if (unix_grow_() != 0) { return (u64)0 - LX_ENOMEM; }
        for (i = 0u; i < g_unix_cap; i++) {
            if (g_unix[i].used == 0u) { break; }
        }
    }
    if (i >= g_unix_cap) { return (u64)0 - LX_ENOMEM; }
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) { return (u64)0 - LX_EMFILE; }
    {
        UnixSock *s = &g_unix[i];
        LxFile *f = &p->fd->ent[fd];

        s->used = 1u;
        s->bound = 0u;
        s->type = (u32)base;
        s->peer = ~0ull;
        s->head = 0u;
        s->tail = 0u;
        s->count = 0u;
        s->listener = ~0ull;
        f->node = (u64)i;
        f->pos = 0ull;
        f->is_dir = 0u;
        f->kind = LX_FD_UNIX;
        f->flags = (type & 0x80000ull) ? LX_FD_CLOEXEC : 0u;
        f->oflags = (type & 0x800ull) ? LX_O_NONBLOCK : 0u;
    }
    return fd;
}

u64 lx_unix_bind(u64 fd, u64 uaddr, u64 ulen)
{
    UnixSock *s = unix_fd_(fd);
    char path[UNIX_PATH_MAX];
    u32 len = 0u;
    u32 abstract = 0u;
    u32 i;

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (unix_addr_(uaddr, ulen, path, &len, &abstract) != 0) {
        return (u64)0 - LX_EINVAL;
    }
    if (unix_by_path_(path, len, abstract) != (UnixSock *)0) {
        return (u64)0 - LX_EADDRINUSE;
    }
    for (i = 0u; i < len; i++) { s->path[i] = path[i]; }
    s->path[len] = '\0';
    s->len = len;
    s->abstract = abstract;
    s->bound = 1u;
    return 0ull;
}

u64 lx_unix_listen(u64 fd, u64 backlog)
{
    UnixSock *s = unix_fd_(fd);

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (s->type != 1u || s->bound == 0u) { return (u64)0 - LX_EINVAL; }
    (void)backlog;
    s->listening = 1u;
    return 0ull;
}

/* 新建一个 AF_UNIX 对象，只返回对象 id（不分配 fd）。
 * connect 时用它造"服务端接受端"，accept 时再把该 id 挂到新 fd。 */
static u64 unix_new_obj_(const UnixSock *tmpl, u64 peer)
{
    u32 i;

    for (i = 0u; i < g_unix_cap; i++) {
        if (g_unix[i].used == 0u) { break; }
    }
    if (i >= g_unix_cap) {
        if (unix_grow_() != 0) { return (u64)0 - LX_ENOMEM; }
        for (i = 0u; i < g_unix_cap; i++) {
            if (g_unix[i].used == 0u) { break; }
        }
    }
    if (i >= g_unix_cap) { return (u64)0 - LX_ENOMEM; }
    {
        UnixSock *s = &g_unix[i];

        s->used = 1u;
        s->bound = tmpl->bound;
        s->type = tmpl->type;
        s->listening = 0u;
        s->peer = peer;
        s->head = 0u; s->tail = 0u; s->count = 0u;
        s->phead = 0u; s->ptail = 0u; s->pcount = 0u;
        s->listener = ~0ull;
        for (u32 k = 0u; k < UNIX_PATH_MAX; k++) { s->path[k] = tmpl->path[k]; }
    }
    return (u64)i;
}

/* 把对象 id 挂到当前进程的一个新 fd 上。 */
static u64 unix_fd_of_obj_(u64 id)
{
    LxProc *p = lx_cur_proc();
    u64 fd;
    LxFile *f;

    if (p == (LxProc *)0 || unix_at_(id) == (UnixSock *)0) {
        return (u64)0 - LX_EBADF;
    }
    fd = lx_fdtab_alloc(p->fd);
    if (fd == LX_FD_NONE) { return (u64)0 - LX_EMFILE; }
    f = &p->fd->ent[fd];
    f->node = id;
    f->pos = 0ull;
    f->is_dir = 0u;
    f->kind = LX_FD_UNIX;
    f->flags = 0u;
    f->oflags = 0u;
    return fd;
}

/* connect 对 STREAM 监听 socket：造一个服务端接受端对象，把它的 id
 * 挂到监听者的 pending 队列；客户端 peer 指向它。 */
static u64 unix_connect_stream_(UnixSock *s, UnixSock *t)
{
    if (t->listening == 0u) { return (u64)0 - LX_ECONNREFUSED; }
    if (t->pcount >= UNIX_BACKLOG) { return (u64)0 - LX_EAGAIN; }
    {
        u64 me = (u64)(s - g_unix);
        u64 server_end = unix_new_obj_(t, me);

        if ((i64)server_end < 0) { return server_end; }
        s->peer = server_end;
        s->listener = (u64)(t - g_unix);
        t->pending[t->ptail] = server_end;
        t->ptail = (t->ptail + 1u) % UNIX_BACKLOG;
        t->pcount++;
        lx_unix_wake();                    /* 唤醒阻塞在 accept() 的服务端 */
    }
    return 0ull;
}

u64 lx_unix_accept(u64 fd, u64 uaddr, u64 uaddrlen)
{
    UnixSock *s = unix_fd_(fd);

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (s->type != 1u || s->listening == 0u) { return (u64)0 - LX_EINVAL; }
    while (s->pcount == 0u) {
        LxFile *f = lx_fdtab_get(lx_cur_proc()->fd, fd);

        if (f != (LxFile *)0 && (f->oflags & LX_O_NONBLOCK) != 0u) {
            return (u64)0 - LX_EAGAIN;
        }
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
    }
    {
        u64 server_end = s->pending[s->phead];
        UnixSock *se = unix_at_(server_end);
        u64 cfd;

        s->phead = (s->phead + 1u) % UNIX_BACKLOG;
        s->pcount--;
        cfd = unix_fd_of_obj_(server_end);
        if ((i64)cfd < 0) { return cfd; }
        /* 接受端与客户端对象互相绑定（都用对象 id） */
        if (se != (UnixSock *)0) {
            UnixSock *ce = unix_at_(se->peer);

            if (ce != (UnixSock *)0) { ce->peer = server_end; }
        }
        (void)uaddr;
        (void)uaddrlen;
        return cfd;
    }
}

u64 lx_unix_connect(u64 fd, u64 uaddr, u64 ulen)
{
    UnixSock *s = unix_fd_(fd);
    UnixSock *t;
    char path[UNIX_PATH_MAX];
    u32 len = 0u;
    u32 abstract = 0u;

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (unix_addr_(uaddr, ulen, path, &len, &abstract) != 0) {
        return (u64)0 - LX_EINVAL;
    }
    t = unix_by_path_(path, len, abstract);
    if (t == (UnixSock *)0) { return (u64)0 - LX_ECONNREFUSED; }
    if (s->type != t->type) { return (u64)0 - LX_EINVAL; }
    if (s->type == 1u) {
        return unix_connect_stream_(s, t);
    }
    s->peer = (u64)(t - g_unix);
    t->peer = (u64)(s - g_unix);
    return 0ull;
}

/* 把一条消息入队；可附带最多 UNIX_PASS_MAX 个 fd 状态副本。 */
static int unix_enq_(UnixSock *s, const u8 *buf, u32 len,
                     const LxFile *pass, u32 nfd)
{
    UnixMsg *m;
    u32 i;

    if (s->count >= UNIX_QDEPTH) { return -LX_EAGAIN; }
    if (len > UNIX_MSG_MAX) { len = UNIX_MSG_MAX; }
    m = &s->q[s->tail];
    for (i = 0u; i < len; i++) { m->data[i] = buf[i]; }
    m->len = len;
    m->off = 0u;
    m->used = 1u;
    m->nfd = (nfd > UNIX_PASS_MAX) ? UNIX_PASS_MAX : nfd;
    for (i = 0u; i < m->nfd; i++) { m->fds[i] = pass[i]; }
    s->tail = (s->tail + 1u) % UNIX_QDEPTH;
    s->count++;
    lx_unix_wake();                        /* 唤醒阻塞在 recvmsg/recvfrom 的对端 */
    return 0;
}

u64 lx_unix_sendto(u64 fd, u64 ubuf, u64 len, u64 uaddr, u64 ulen)
{
    UnixSock *s = unix_fd_(fd);
    UnixSock *t;
    u8 *kbuf;
    int rc;

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (len == 0ull || len > UNIX_MSG_MAX) { return (u64)0 - LX_EINVAL; }
    if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    t = s->peer != ~0ull ? unix_at_(s->peer) : (UnixSock *)0;
    if (t == (UnixSock *)0) { return (u64)0 - LX_ECONNREFUSED; }
    kbuf = (u8 *)port_kcalloc(1ull, len);
    if (kbuf == (u8 *)0) { return (u64)0 - LX_ENOMEM; }
    for (u64 i = 0ull; i < len; i++) { kbuf[i] = *(const u8 *)(ubuf + i); }
    rc = unix_enq_(t, kbuf, (u32)len, (const LxFile *)0, 0u);
    port_kfree((void *)kbuf);
    if (rc != 0) { return (u64)0 - LX_EAGAIN; }
    return len;
}

/* STREAM 字节流取数：跨消息合并，部分读用 m->off 保留剩余。
 * 遇到带 SCM_RIGHTS 的消息：收下其字节与 fd 后停下（cmsg 要与数据对齐）。
 * pass/nfd 输出待安装的 fd；recvfrom 无 cmsg 时忽略即可（旧行为）。 */
static u32 unix_stream_take_(UnixSock *s, u8 *dst, u32 cap,
                             LxFile *pass, u32 *nfd)
{
    u32 total = 0u;

    *nfd = 0u;
    while (total < cap && s->count > 0u) {
        UnixMsg *m = &s->q[s->head];
        u32 avail = m->len - m->off;
        u32 n = (avail < cap - total) ? avail : cap - total;

        for (u32 i = 0u; i < n; i++) { dst[total + i] = m->data[m->off + i]; }
        m->off += n;
        total += n;
        if (m->off >= m->len) {
            if (m->nfd != 0u) {
                for (u32 k = 0u; k < m->nfd; k++) { pass[k] = m->fds[k]; }
                *nfd = m->nfd;
            }
            s->head = (s->head + 1u) % UNIX_QDEPTH;
            s->count--;
            if (m->nfd != 0u) { break; }   /* cmsg 与数据对齐 */
        } else {
            break;                         /* 部分读：剩余留在槽里 */
        }
    }
    return total;
}

u64 lx_unix_recvfrom(u64 fd, u64 ubuf, u64 len, u64 uaddr, u64 uaddrlen)
{
    UnixSock *s = unix_fd_(fd);
    UnixMsg *m;
    u32 n;

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (lx_user_ok(ubuf, len, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
    while (s->count == 0u) {
        LxFile *f = lx_fdtab_get(lx_cur_proc()->fd, fd);

        if (f != (LxFile *)0 && (f->oflags & LX_O_NONBLOCK) != 0u) {
            return (u64)0 - LX_EAGAIN;
        }
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
    }
    if (s->type == 1u) {                   /* STREAM：字节流，跨消息合并 */
        LxFile pass[UNIX_PASS_MAX];
        u32 nfd = 0u;

        n = unix_stream_take_(s, (u8 *)ubuf, (u32)len, pass, &nfd);
        (void)nfd;                         /* recvfrom 不带 cmsg，fd 丢弃 */
        return (u64)n;
    }
    /* DGRAM：一条消息一读，短读丢弃剩余（保持数据报语义） */
    m = &s->q[s->head];
    n = (m->len < len) ? m->len : (u32)len;
    for (u32 i = 0u; i < n; i++) { *(u8 *)(ubuf + i) = m->data[i]; }
    s->head = (s->head + 1u) % UNIX_QDEPTH;
    s->count--;
    return (u64)n;
}

/* SCM_RIGHTS over AF_UNIX：sendmsg 解析 cmsg 中的 fd 数组，随消息入队；
 * recvmsg 把 LxFile 副本安装到接收方 fd 表。x86-64 cmsghdr 16B：
 * cmsg_len@0 cmsg_level@8 cmsg_type@12，数据从 +16 起。 */
#define UNIX_CMSG_DATA 16u

u64 lx_unix_sendmsg(u64 fd, u64 umsg, u64 flags)
{
    UnixSock *s = unix_fd_(fd);
    UnixSock *t;
    u64 iov, niov, ctl, ctllen, total = 0ull;
    u8 *buf;
    LxFile pass[UNIX_PASS_MAX];
    u32 nfd = 0u;
    u32 i;
    int rc;

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    (void)flags;
    if (lx_user_ok(umsg, 56ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    iov = *(const u64 *)(umsg + 16ull);
    niov = *(const u64 *)(umsg + 24ull);
    if (niov > 64ull || (niov != 0ull && lx_user_ok(iov, niov * 16ull, C6OS_PROT_R) == 0)) {
        return (u64)0 - LX_EINVAL;
    }
    for (i = 0u; i < (u32)niov; i++) {
        total += *(const u64 *)(iov + (u64)i * 16ull + 8ull);
        if (total > UNIX_MSG_MAX) { return (u64)0 - LX_EINVAL; }
    }
    /* 解析 control：SOL_SOCKET/SCM_RIGHTS → fd 数组 */
    ctl = *(const u64 *)(umsg + 32ull);
    ctllen = *(const u64 *)(umsg + 40ull);
    if (ctllen != 0ull) {
        if (ctl == 0ull || lx_user_ok(ctl, ctllen, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        {
            u64 c = ctl;
            u64 end = ctl + ctllen;

            while (c + UNIX_CMSG_DATA <= end) {
                u64 clen = *(const u64 *)c;
                u32 lev = *(const u32 *)(c + 8ull);
                u32 typ = *(const u32 *)(c + 12ull);

                if (clen < UNIX_CMSG_DATA || c + clen > end) { break; }
                if (lev == 1u && typ == 1u && clen > UNIX_CMSG_DATA) {
                    u32 nf = (u32)((clen - UNIX_CMSG_DATA) / 4ull);

                    if (nf > UNIX_PASS_MAX) { nf = UNIX_PASS_MAX; }
                    for (i = 0u; i < nf; i++) {
                        u32 fdesc = *(const u32 *)(c + UNIX_CMSG_DATA + (u64)i * 4ull);
                        LxFile *f = lx_fdtab_get(lx_cur_proc()->fd, fdesc);

                        if (f != (LxFile *)0) { pass[nfd++] = *f; }
                    }
                }
                c += clen;
            }
        }
    }
    t = s->peer != ~0ull ? unix_at_(s->peer) : (UnixSock *)0;
    if (t == (UnixSock *)0) { return (u64)0 - LX_ECONNREFUSED; }
    buf = (u8 *)port_kcalloc(1ull, total ? total : 1ull);
    if (buf == (u8 *)0) { return (u64)0 - LX_ENOMEM; }
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
            for (k = 0ull; k < len; k++) { buf[pos + k] = *(const u8 *)(base + k); }
            pos += len;
        }
    }
    rc = unix_enq_(t, buf, (u32)total, pass, nfd);
    port_kfree((void *)buf);
    if (rc != 0) { return (u64)0 - LX_EAGAIN; }
    return total;
}

u64 lx_unix_recvmsg(u64 fd, u64 umsg, u64 flags)
{
    UnixSock *s = unix_fd_(fd);
    u64 iov, niov, ctl, ctllen, total = 0ull;
    u8 *buf;
    u32 i;
    int n;

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    (void)flags;
    if (lx_user_ok(umsg, 56ull, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    iov = *(const u64 *)(umsg + 16ull);
    niov = *(const u64 *)(umsg + 24ull);
    if (niov > 64ull || (niov != 0ull && lx_user_ok(iov, niov * 16ull, C6OS_PROT_R) == 0)) {
        return (u64)0 - LX_EINVAL;
    }
    for (i = 0u; i < (u32)niov; i++) {
        total += *(const u64 *)(iov + (u64)i * 16ull + 8ull);
        if (total > UNIX_MSG_MAX) { return (u64)0 - LX_EINVAL; }
    }
    while (s->count == 0u) {
        LxFile *f = lx_fdtab_get(lx_cur_proc()->fd, fd);

        if (f != (LxFile *)0 && (f->oflags & LX_O_NONBLOCK) != 0u) {
            return (u64)0 - LX_EAGAIN;
        }
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
    }
    buf = (u8 *)port_kcalloc(1ull, total ? total : 1ull);
    if (buf == (u8 *)0) { return (u64)0 - LX_ENOMEM; }
    {
        u64 pos = 0ull;
        u64 got;
        LxFile pass[UNIX_PASS_MAX];
        u32 pass_nfd = 0u;

        if (s->type == 1u) {
            /* STREAM：字节流，跨消息合并；带 fd 的消息收下后停，cmsg 对齐 */
            got = (u64)unix_stream_take_(s, buf, (u32)total, pass, &pass_nfd);
        } else {
            UnixMsg *m = &s->q[s->head];

            got = (m->len < total) ? m->len : total;
            for (i = 0u; i < (u32)got; i++) { buf[i] = m->data[i]; }
            for (i = 0u; i < m->nfd; i++) { pass[i] = m->fds[i]; }
            pass_nfd = m->nfd;
            s->head = (s->head + 1u) % UNIX_QDEPTH;
            s->count--;
        }
        for (i = 0u; i < (u32)niov && pos < got; i++) {
            u64 base = *(const u64 *)(iov + (u64)i * 16ull);
            u64 len = *(const u64 *)(iov + (u64)i * 16ull + 8ull);
            u64 k = (len < got - pos) ? len : got - pos;
            u64 j;

            if (lx_user_ok(base, k, C6OS_PROT_W) == 0) {
                port_kfree((void *)buf);
                return (u64)0 - LX_EFAULT;
            }
            for (j = 0u; j < k; j++) { *(u8 *)(base + j) = buf[pos + j]; }
            pos += k;
        }
        n = (int)got;
        /* SCM_RIGHTS：安装附带的 fd 到接收方表 */
        ctl = *(const u64 *)(umsg + 32ull);
        ctllen = *(const u64 *)(umsg + 40ull);
        if (pass_nfd != 0u && ctl != 0ull &&
            ctllen >= UNIX_CMSG_DATA + pass_nfd * 4ull &&
            lx_user_ok(ctl, ctllen, C6OS_PROT_W) != 0) {
            u8 c[UNIX_CMSG_DATA];
            u32 k;

            for (k = 0u; k < UNIX_CMSG_DATA; k++) { c[k] = 0u; }
            *(u64 *)c = (u64)(UNIX_CMSG_DATA + pass_nfd * 4ull);
            *(u32 *)(c + 8ull) = 1u;        /* SOL_SOCKET */
            *(u32 *)(c + 12ull) = 1u;       /* SCM_RIGHTS */
            for (k = 0u; k < UNIX_CMSG_DATA; k++) { *(u8 *)(ctl + k) = c[k]; }
            for (k = 0u; k < pass_nfd; k++) {
                u64 nfd = lx_unix_fd_install(&pass[k]);

                if ((i64)nfd >= 0) {
                    *(u32 *)(ctl + UNIX_CMSG_DATA + (u64)k * 4ull) = (u32)nfd;
                }
            }
            *(u64 *)(umsg + 40ull) = UNIX_CMSG_DATA + pass_nfd * 4ull;
        }
    }
    port_kfree((void *)buf);
    return (u64)n;
}

u64 lx_unix_close(u64 fd)
{
    UnixSock *s = unix_fd_(fd);

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (s->peer != ~0ull) {
        UnixSock *t = unix_at_(s->peer);
        if (t != (UnixSock *)0) { t->peer = ~0ull; }
    }
    s->used = 0u;
    return 0ull;
}

u16 lx_unix_poll(u64 id)
{
    UnixSock *s = unix_at_(id);

    if (s == (UnixSock *)0) { return LX_POLLNVAL; }
    return (s->count != 0u) ? (u16)LX_POLLIN : 0u;
}
