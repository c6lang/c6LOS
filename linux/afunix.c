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

typedef struct {
    u8  data[UNIX_MSG_MAX];
    u32 len;
    u32 used;
} UnixMsg;

#define UNIX_BACKLOG 8u

typedef struct {
    char path[UNIX_PATH_MAX];
    u32  bound;
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

static UnixSock *unix_by_path_(const char *path, u32 len)
{
    u32 i;

    if (g_unix == (UnixSock *)0) { return (UnixSock *)0; }
    for (i = 0u; i < g_unix_cap; i++) {
        UnixSock *s = &g_unix[i];

        if (s->used != 0u && s->bound != 0u &&
            lx_str_len_(s->path, UNIX_PATH_MAX) == len &&
            lx_str_eq_(s->path, path, len)) {
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

/* 读 sockaddr_un：family(u16)@0 + path@2，最多 106 字节。 */
static int unix_addr_(u64 uaddr, u64 ulen, char *path, u32 *outlen)
{
    u8 *p;
    u32 len;

    if (lx_user_ok(uaddr, 2ull, C6OS_PROT_R) == 0) { return -1; }
    p = (u8 *)uaddr;
    if (((u16)p[0] | ((u16)p[1] << 8)) != 1u) { return -1; }  /* AF_UNIX=1 */
    len = lx_str_len_((const char *)(p + 2u), UNIX_PATH_MAX - 2u);
    if (len == 0u || len > UNIX_PATH_MAX - 2u) { return -1; }
    if (ulen > 110ull) { ulen = 110ull; }
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

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (unix_addr_(uaddr, ulen, path, &len) != 0) { return (u64)0 - LX_EINVAL; }
    if (unix_by_path_(path, len) != (UnixSock *)0) {
        return (u64)0 - LX_EADDRINUSE;
    }
    for (len = 0u; len < UNIX_PATH_MAX - 1u && path[len] != '\0'; len++) {
        s->path[len] = path[len];
    }
    s->path[len] = '\0';
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

    if (s == (UnixSock *)0) { return (u64)0 - LX_EBADF; }
    if (unix_addr_(uaddr, ulen, path, &len) != 0) { return (u64)0 - LX_EINVAL; }
    t = unix_by_path_(path, len);
    if (t == (UnixSock *)0) { return (u64)0 - LX_ECONNREFUSED; }
    if (s->type != t->type) { return (u64)0 - LX_EINVAL; }
    if (s->type == 1u) {
        return unix_connect_stream_(s, t);
    }
    s->peer = (u64)(t - g_unix);
    t->peer = (u64)(s - g_unix);
    return 0ull;
}

/* 把一条消息入队；返回 0 成功、-EAGAIN 队列满。 */
static int unix_enq_(UnixSock *s, const u8 *buf, u32 len)
{
    UnixMsg *m;

    if (s->count >= UNIX_QDEPTH) { return -LX_EAGAIN; }
    if (len > UNIX_MSG_MAX) { len = UNIX_MSG_MAX; }
    m = &s->q[s->tail];
    for (u32 i = 0u; i < len; i++) { m->data[i] = buf[i]; }
    m->len = len;
    m->used = 1u;
    s->tail = (s->tail + 1u) % UNIX_QDEPTH;
    s->count++;
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
    rc = unix_enq_(t, kbuf, (u32)len);
    port_kfree((void *)kbuf);
    if (rc != 0) { return (u64)0 - LX_EAGAIN; }
    return len;
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
    m = &s->q[s->head];
    n = (m->len < len) ? m->len : (u32)len;
    for (u32 i = 0u; i < n; i++) { *(u8 *)(ubuf + i) = m->data[i]; }
    s->head = (s->head + 1u) % UNIX_QDEPTH;
    s->count--;
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
