/* linux/pty.c —— M45：最小可用 pty（伪终端）
 *
 * 架构：固定数组对象表（fork/dup 后 fd 与对象解耦），每个对象一对环形
 * 缓冲：master 写→slave 读（用户输出）、slave 写→master 读（用户输入的回显/
 * 程序输出喂给 master 的 reader）。参照 AF_UNIX 的对象 id 模型：
 *   LxFile.node = 对象 id，LxFile.pos = 0 表示 master、1 表示 slave。
 *
 * 阻塞读：与 tty.c 一样靠 lx_proc_set_wait/lx_proc_block + 入队时
 * lx_unix_wake()（复用它的 wake_all_）。不引入中断、不用忙等循环。
 *
 * 故意不做（边际项，见文档）：termios 行规程沿用全局 tty 的那套默认
 * termios（TCGETS/TIOCGWINSZ 复用 lx_tty_ioctl 的返回值），不做每个 pty
 * 独立的 ICANON/ECHO；不做 /dev/pts 目录列举（gets 从 ptmx 的 TIOCGPTN
 * 拿编号即可）。这些都不影响"程序能用 pty 双向通信"的核心语义。
 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

#define PTY_MAX      8u
#define PTY_RBUF     4096u

typedef struct {
    u8  buf[PTY_RBUF];
    u32 head;
    u32 tail;
    u32 count;
} PtyRing;

typedef struct {
    u32 used;
    u32 slave_open;                    /* 从设备是否被打开过 */
    PtyRing to_slave;                  /* master 写 → slave 读 */
    PtyRing to_master;                 /* slave 写 → master 读 */
} Pty;

static Pty g_pty[PTY_MAX];

static Pty *pty_at_(u64 id)
{
    if (id >= (u64)PTY_MAX || g_pty[id].used == 0u) {
        return (Pty *)0;
    }
    return &g_pty[id];
}

static void ring_put_(PtyRing *r, const u8 *src, u32 n)
{
    u32 i;

    for (i = 0u; i < n; i++) {
        if (r->count >= PTY_RBUF) { break; }
        r->buf[r->tail] = src[i];
        r->tail = (r->tail + 1u) % PTY_RBUF;
        r->count++;
    }
}

u64 lx_pty_open_master(void)
{
    u32 i;

    for (i = 0u; i < PTY_MAX; i++) {
        if (g_pty[i].used == 0u) { break; }
    }
    if (i >= PTY_MAX) { return (u64)0 - LX_EMFILE; }
    {
        Pty *pt = &g_pty[i];

        pt->used = 1u;
        pt->slave_open = 0u;
        pt->to_slave.head = 0u; pt->to_slave.tail = 0u; pt->to_slave.count = 0u;
        pt->to_master.head = 0u; pt->to_master.tail = 0u; pt->to_master.count = 0u;
    }
    return (u64)i;
}

u64 lx_pty_open_slave(u64 id)
{
    Pty *pt = pty_at_(id);

    if (pt == (Pty *)0) { return (u64)0 - LX_ENOENT; }
    pt->slave_open = 1u;
    return id;
}

static PtyRing *ring_for_(Pty *pt, u32 is_master, int writing)
{
    /* master 写 to_slave、读 to_master；slave 相反 */
    if (is_master != 0u) { return writing ? &pt->to_slave : &pt->to_master; }
    return writing ? &pt->to_master : &pt->to_slave;
}

u64 lx_pty_read(u64 node, u64 ubuf, u64 len, u32 is_master)
{
    Pty *pt = pty_at_(node);
    PtyRing *r;
    u64 n = 0ull;

    if (pt == (Pty *)0) { return (u64)0 - LX_EBADF; }
    if (lx_user_ok(ubuf, len, C6OS_PROT_W) == 0) { return (u64)0 - LX_EFAULT; }
    r = ring_for_(pt, is_master, 0);
    while (r->count == 0u) {
        LxProc *p = lx_cur_proc();
        LxFile *f = (p != (LxProc *)0) ? lx_fdtab_get(p->fd, 0ull) : (LxFile *)0;

        (void)f;
        lx_proc_set_wait(LX_WAIT_PIPE);
        lx_proc_block();
        /* 重新解析：yield 后当前进程可能换过 */
        pt = pty_at_(node);
        if (pt == (Pty *)0) { return (u64)0 - LX_EBADF; }
        r = ring_for_(pt, is_master, 0);
    }
    n = (len < (u64)r->count) ? len : (u64)r->count;
    for (u64 i = 0ull; i < n; i++) {
        *(u8 *)(ubuf + i) = r->buf[r->head];
        r->head = (r->head + 1u) % PTY_RBUF;
        r->count--;
    }
    return n;
}

u64 lx_pty_write(u64 node, u64 ubuf, u64 len, u32 is_master)
{
    Pty *pt = pty_at_(node);
    PtyRing *r;

    if (pt == (Pty *)0) { return (u64)0 - LX_EBADF; }
    if (len == 0ull) { return 0ull; }
    if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) { return (u64)0 - LX_EFAULT; }
    r = ring_for_(pt, is_master, 1);
    {
        u8 *tmp = (u8 *)port_kcalloc(1ull, len);
        u64 i;

        if (tmp == (u8 *)0) { return (u64)0 - LX_ENOMEM; }
        for (i = 0ull; i < len; i++) { tmp[i] = *(u8 *)(ubuf + i); }
        ring_put_(r, tmp, (u32)len);
        port_kfree((void *)tmp);
    }
    lx_unix_wake();                    /* 唤醒对端阻塞的 reader */
    return len;
}

void lx_pty_close(u64 node, u32 is_master)
{
    Pty *pt = pty_at_(node);

    if (pt == (Pty *)0) { return; }
    lx_unix_wake();
    if (is_master != 0u) {
        pt->used = 0u;                 /* master 关 = 整个 pty 销毁 */
    }
}

i64 lx_pty_ioctl(u64 node, u64 req, u64 arg, u32 is_master)
{
    Pty *pt = pty_at_(node);

    if (pt == (Pty *)0) { return (i64)0 - (i64)LX_ENOTTY; }
    /* TIOCGPTN：从 master 取从设备编号（glibc ptsname 用它拼 /dev/pts/N）。 */
    if (is_master != 0u && req == 0x80045430ull) {
        if (arg == 0ull || lx_user_ok(arg, 4ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        *(u32 *)arg = (u32)node;
        return 0;
    }
    /* 自己答 termios，不依赖全局串口 tty（lx_tty_ioctl 在 g_on==0 时
     * 返回 ENOTTY，而 pty 可以在没有控制台行规程的场景里独立使用）。 */
    if (req == 0x5401ull) {                /* TCGETS：最小 termios，60B */
        u8 *t = (u8 *)arg;

        if (t == (u8 *)0 || lx_user_ok(arg, 60ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        for (u32 k = 0u; k < 60u; k++) { t[k] = 0u; }
        *(u32 *)(t + 0)  = 0x00000500u;    /* ICRNL | IXON */
        *(u32 *)(t + 4)  = 0x00000005u;    /* OPOST | ONLCR */
        *(u32 *)(t + 8)  = 0x000000BFu;    /* B38400 | CS8 | CREAD */
        *(u32 *)(t + 12) = 0x00008A3Bu;    /* ISIG|ICANON|ECHO|... */
        t[17] = 1u; t[18] = 4u; t[19] = 0x7Fu;  /* VINTR/VEOF/VERASE */
        t[32] = 1u;                        /* VMIN */
        *(u32 *)(t + 36) = 38400u;
        *(u32 *)(t + 40) = 38400u;
        return 0;
    }
    if (req == 0x5402ull || req == 0x5403ull || req == 0x5404ull) {
        return 0;                          /* TCSETS*：接受但无硬件可设 */
    }
    if (req == 0x5413ull) {                /* TIOCGWINSZ */
        u8 *w = (u8 *)arg;

        if (w == (u8 *)0 || lx_user_ok(arg, 8ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        *(u16 *)(w + 0) = 24u;
        *(u16 *)(w + 2) = 80u;
        *(u16 *)(w + 4) = 0u;
        *(u16 *)(w + 6) = 0u;
        return 0;
    }
    return (i64)0 - (i64)LX_ENOTTY;
}

u32 lx_pty_is_tty(u64 node)
{
    return pty_at_(node) != (Pty *)0 ? 1u : 0u;
}
