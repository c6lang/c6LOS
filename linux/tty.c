/* linux/tty.c —— 串口控制台的行规程（M12）
 *
 * c6os 的串口是**轮询**的（port_getc 直接读 COM1 的 LSR），没有串口中断，
 * 所以这里不可能是"中断驱动 + 环形缓冲 + 唤醒等待者"那套。做成：
 *
 *   每一次系统调用进来（lx_syscall 末尾）泵一次串口，读到字节就喂进行规程；
 *   用户态 read(0) 没数据时在内核里循环泵 + 让出 CPU，直到攒够一行。
 *
 * 这样做的代价是 read(0) 期间 CPU 空转，但在这个项目里没有别的活要干（单
 * 终端、协作式调度），换来的是零中断依赖、零竞态，代码不到 200 行。
 *
 * 行规程（够 busybox sh 用）：
 *   - 回显：可打印字符原样回显，'\r'/'\n' 回显成 "\r\n"；
 *   - 退格：0x7F / 0x08 删掉一个字符并回显 "\b \b"，行首时不删；
 *   - Ctrl-C：发 SIGINT 给前台进程组，丢掉当前行，回显 "^C\r\n"；
 *   - Ctrl-\\：同理发 SIGQUIT（Linux 的 VQUIT），行为对齐；
 *   - Ctrl-D：行首时当 EOF（read 返回 0），行中则丢掉当前行；
 *   - 行结束：'\r' 或 '\n' 都当回车，写回 "\r\n"，并把整行交给读方。
 *
 * 为什么只在 lx_tty_init() 之后才生效：fd 0 以前是"永远 EOF"，已有的一堆
 * 验收（L2..L6）依赖这个行为。tty 是显式打开的，默认关着，旧测试一字不改。
 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

/* 行的编辑语义由 c_lflag 决定（TCSETS 会改它）：
 *   ICANON 开：攒整行，退格/^C/^D 由我们处理（命令行外的普通程序）
 *   ICANON 关：raw 模式，字节直接透传给读者 —— busybox 的 lineedit 就是这么
 *              干的，它要自己回显、自己处理方向键；我们再回一遍就成双回显了 */
#define TTY_ICANON  0x00000002u
#define TTY_ISIG    0x00000001u
#define TTY_ECHO    0x00000008u
static u32  g_lflag = 0x00008A3Bu;      /* 默认 ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN */

#define TTY_LINE 256u                  /* 一行最多多少字符（超出丢弃并响铃） */
#define TTY_PEND 512u                  /* 待读走的数据缓冲（可能攒了多行） */

static u32  g_on;                      /* lx_tty_init() 之后为 1 */
static char g_edit[TTY_LINE];          /* 正在编辑的这一行 */
static u32  g_elen;
static char g_pend[TTY_PEND];          /* 已经回车、等着被读走的字节 */
static u32  g_plen;
static u32  g_ppos;
static u32  g_eof;                     /* Ctrl-D 置位：下一读返回 0 */

void lx_tty_init(void)
{
    g_on = 1u;
    g_lflag = 0x00008A3Bu;
    g_elen = 0u;
    g_plen = 0u;
    g_ppos = 0u;
    g_eof = 0u;
}

u32 lx_tty_enabled(void)
{
    return g_on;
}

/* 回显：'\n' 前面补一个 '\r'，否则终端上是"斜着往下走"的阶梯输出 */
static void echo_(char c)
{
    if (c == '\n') {
        port_putc((int)'\r');
    }
    port_putc((int)c);
}

static void pend_(char c)
{
    if (g_plen < TTY_PEND) {
        g_pend[g_plen] = c;
        g_plen++;
    }
}

/* 把当前行收进待读缓冲（含行尾的 '\n'） */
static void commit_(void)
{
    u32 i = 0u;

    while (i < g_elen) {
        pend_(g_edit[i]);
        i++;
    }
    pend_('\n');
    g_elen = 0u;
}

/* ---- 控制字符：ISIG 语义 ------------------------------------------------
 *
 * Linux 的行规程在 lflag 带 ISIG 时，收到 VINTR/VQUIT 不是"把字节交给读者"，
 * 而是**给前台进程组发信号**并把当前行丢掉。这个内核以前只做了"丢掉当前行"，
 * 于是 `ping 8.8.8.8` 一旦跑起来就再也停不下来（前台进程根本不知道有人
 * 按了 ^C）。现在补上投递这一步。
 *
 * 目标：前台进程组（tcsetpgrp 设的 g_fore，没设过就退回 kill(0) 的语义 =
 * 调用者所在进程组）。整组投递用 lx_proc_send(-pgid, sig)，和 kill(2) 复用
 * 同一套选目标逻辑，避免在这个文件里重新实现一遍进程组遍历。
 *
 * 调用者上下文是**内核、系统调用里**（lx_tty_pump 由系统调用/等待节拍驱动），
 * 所以只能挂 pending，实际投递等系统调用返回到 lx_syscall 出口的
 * lx_sig_deliver_frame —— 这正是 Linux 的语义（信号在系统调用边界生效）。 */
static void isig_(u32 sig)
{
    u64 fore = (u64)lx_proc_fore();

    if (fore != 0ull) {
        (void)lx_proc_send(0 - (i64)fore, sig);
        return;
    }
    /* 还没设过前台组（交互 shell 启动早期）：按"当前进程组"投递。 */
    (void)lx_proc_send(0, sig);
}

/* 吃一个输入字节，喂进行规程 */
static void feed_(u8 c)
{
    if ((g_lflag & TTY_ICANON) == 0u) {
        /* raw：不攒行、不做行编辑，也不替读者回显（除非 ECHO 还开着）。
         *
         * ISIG 在 raw 模式下同样有效（Linux 也是这样）：busybox 的 lineedit
         * 会把终端设成 raw 来自己做行编辑，但 ^C 仍然该是信号，它自己也是
         * 这么期待的（收到 SIGINT 就放弃当前行）。 */
        if ((g_lflag & TTY_ISIG) != 0u && c == 0x03u) {
            echo_('^');
            echo_('C');
            echo_('\n');
            isig_((u32)LX_SIGINT);
            return;
        }
        if (c == '\r') {
            c = '\n';
        } else if (c == '\n' && (g_lflag & TTY_ECHO) == 0u) {
            c = '\r';                    /* 不重复本例：只把 CR 补上 */
        }
        if ((g_lflag & TTY_ECHO) != 0u) {
            echo_((char)c);
        }
        pend_((char)c);
        return;
    }
    if (c == '\r' || c == '\n') {
        echo_('\n');
        commit_();
        return;
    }
    if (c == 0x7Fu || c == 0x08u) {                 /* 退格 */
        if (g_elen > 0u) {
            g_elen--;
            port_putc((int)'\b');
            port_putc((int)' ');
            port_putc((int)'\b');
        }
        return;
    }
    if (c == 0x03u) {                               /* Ctrl-C */
        g_elen = 0u;
        echo_('^');
        echo_('C');
        echo_('\n');
        if ((g_lflag & TTY_ISIG) != 0u) {
            isig_((u32)LX_SIGINT);
        }
        return;
    }
    if (c == 0x1Cu) {                               /* Ctrl-\：SIGQUIT */
        g_elen = 0u;
        echo_('^');
        echo_('\\');
        echo_('\n');
        if ((g_lflag & TTY_ISIG) != 0u) {
            isig_((u32)LX_SIGQUIT);
        }
        return;
    }
    if (c == 0x04u) {                               /* Ctrl-D */
        if (g_elen == 0u) {
            g_eof = 1u;
        } else {
            g_elen = 0u;
        }
        return;
    }
    if (c < 0x20u) {                                /* 其它控制字符：忽略 */
        return;
    }
    if (g_elen >= TTY_LINE - 1u) {
        return;
    }
    g_edit[g_elen] = (char)c;
    g_elen++;
    echo_((char)c);
}

/* ---- 输出侧：终端要自己应答光标位置查询 ----
 *
 * busybox 的 lineedit 启动时往 stdout 写 ESC[6n（DSR：Device Status Report），
 * 期待终端从**输入方向**回 ESC[行;列R。我们原来把 ESC[6n 原样打到串口上
 * （用户看到屏幕上多出 "\e[6n" 这种乱码），而且永远没人回话，ash 等不到就
 * 退出——表现为"打了提示符 / # 然后立刻 exit_group(0)"。
 *
 * 真终端（xterm/linux console）都是这么做的：解析输出流里的 CSI 序列，
 * 遇到 DSR 就往输入方向塞应答。这里只实现 6n 这一条，别的序列照原样透传
 * （串口对面的宿主终端自然会处理颜色/清屏那些）。
 *
 * 光标位置固定报 1;1：我们没有屏幕模型，而 lineedit 拿到任何合法应答都会
 * 走"终端支持 DSR"的分支，报第一行第一列最安全。 */
static u32  g_esc;                     /* 0=正常 1=收到 ESC 2=ESC[ 3=ESC[6 */
static char g_hold[4];                 /* 疑似 DSR 的字节先扣住，不是原样吐出 */
static u32  g_hlen;

/* 把应答塞进输入缓冲（用户下一次 read(0) 就能读到） */
static void reply_(const char *s)
{
    while (*s != '\0') {
        pend_(*s);
        s++;
    }
}

/* 吐出去。**逐字节原样透传**，唯一的例外是 DSR 询问（ESC[6n）：
 * 那个不能让用户看到，要在输入方向回 ESC[1;1R。所以只把"疑似 DSR 的前 4 个
 * 字节"扣住，凑齐发现不是就原样吐出 —— 别的做法（边解析边丢参数）会把颜色
 * 序列和紧跟着的文件名吃掉：深渊实测 `ls /` 会显示成 `in tc ib nt roc`。 */
static void flush_hold_(void)
{
    u32 i;

    for (i = 0u; i < g_hlen; i++) {
        port_putc((int)g_hold[i]);
    }
    g_hlen = 0u;
}

u64 lx_tty_write(u64 ubuf, u64 len)
{
    u64 i;

    for (i = 0ull; i < len; i++) {
        u8 c = *(const u8 *)(ubuf + i);

        if (g_hlen == 0u && c != 0x1Bu) {
            port_putc((int)c);
            continue;
        }
        g_hold[g_hlen] = (char)c;
        g_hlen++;
        if (g_hlen == 1u) {
            continue;                       /* 只可能是 ESC */
        }
        if (g_hlen == 2u && c != '[') {
            flush_hold_();                  /* 不是 CSI：ESC + 这个字节都吐出去 */
            continue;
        }
        if (g_hlen == 3u && c != '6') {
            flush_hold_();
            continue;
        }
        if (g_hlen == 4u) {
            if (c == 'n') {
                reply_("\x1b[1;1R");        /* DSR：回第 1 行第 1 列，询问本身吞掉 */
                g_hlen = 0u;
            } else {
                flush_hold_();              /* ESC[6 后面不是 n：原样透传 */
            }
        }
    }
    return len;
}

/* 泵一次串口。返回这次吃到几个字节。 */
u32 lx_tty_pump(void)
{
    u32 n = 0u;

    if (g_on == 0u) {
        return 0u;
    }
    for (;;) {
        int c = port_getc();

        if (c < 0) {
            break;
        }
        feed_((u8)c);
        n++;
        if (n >= 64u) {                             /* 别在中断上下文里待太久 */
            break;
        }
    }
    return n;
}

u32 lx_tty_ready(void)
{
    if (g_on == 0u) {
        return 0u;
    }
    if (g_ppos < g_plen || g_eof != 0u) {
        return 1u;
    }
    (void)lx_tty_pump();
    return (g_ppos < g_plen || g_eof != 0u) ? 1u : 0u;
}

/* 把待读数据搬给用户（ubuf 已经过 lx_user_ok 校验）。
 * 没有数据就在内核里泵串口 + 让出 CPU，直到至少有一行。
 *
 * M60：等待循环里也要问一次"有信号了吗"。以前这里死等整行，连 SIGINT 都
 * 打断不了 —— 交互 shell 上跑的前台程序（比如 ping）一旦开始读终端就再也
 * 停不下来。按 Linux 语义：被信号打断时返回 -EINTR（带 SA_RESTART 的
 * handler 则继续等）。 */
u64 lx_tty_read(u64 ubuf, u64 len, void (*yield_fn)(void))
{
    u64 i = 0ull;

    while (g_ppos >= g_plen && g_eof == 0u) {
        (void)lx_proc_wait_tick();       /* 串口 + 到点的闹钟/定时器 */
        if (g_ppos < g_plen || g_eof != 0u) {
            break;
        }
        if (lx_proc_wait_intr() != 0 || lx_proc_sig_pending() != 0) {
            return (u64)0 - (u64)LX_EINTR;
        }
        if (yield_fn != (void (*)(void))0) {
            yield_fn();                             /* 让别的进程跑一会儿 */
        }
    }
    if (g_ppos >= g_plen) {
        g_eof = 0u;                                 /* EOF 只报一次 */
        return 0ull;
    }
    while (i < len && g_ppos < g_plen) {
        *(u8 *)(ubuf + i) = (u8)g_pend[g_ppos];
        g_ppos++;
        i++;
    }
    if (g_ppos >= g_plen) {
        g_ppos = 0u;
        g_plen = 0u;
    }
    /* 还有整行没读走时先泵一次，免得交互输入要等下一次系统调用 */
    (void)lx_tty_pump();
    return i;
}

/* ---- ioctl：让 isatty() 认账，并且给出一个像样的 termios ----
 *
 * busybox 的 sh 启动时会 tcgetattr(0) 判断自己是不是交互式；返回 ENOTTY 的话
 * 它会退化成非交互模式（不打印提示符、不处理 Ctrl-C）。所以这里要装得像
 * 一个真的串口终端。字段值按 Linux 的 x86_64 struct termios（60 字节）
 * 摆：c_iflag/c_oflag/c_cflag/c_lflag 各 u32，c_line u8，c_cc[19]，
 * 末尾两个 u32 是 ispeed/ospeed。 */
#define TCGETS      0x5401u
#define TCSETS      0x5402u
#define TCSETSW     0x5403u
#define TCSETSF     0x5404u
#define TIOCGPGRP   0x540Fu
#define TIOCSPGRP   0x5410u
#define TIOCGWINSZ  0x5413u
#define FIONREAD    0x541Bu

i64 lx_tty_ioctl(u64 fd, u64 req, u64 arg)
{
    (void)fd;
    if (g_on == 0u) {
        return (i64)0 - (i64)LX_ENOTTY;
    }
    if (req == (u64)TCGETS) {
        u8 *t = (u8 *)arg;

        if (t == (u8 *)0 || lx_user_ok(arg, 60ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        *(u32 *)(t + 0)  = 0x00000500u;    /* ICRNL | IXON */
        *(u32 *)(t + 4)  = 0x00000005u;    /* OPOST | ONLCR */
        *(u32 *)(t + 8)  = 0x000000BFu;    /* B38400 | CS8 | CREAD | HUPCL */
        *(u32 *)(t + 12) = g_lflag;        /* 程序 tcsetattr 过的值要能读回来 */
        t[16] = 0u;                        /* c_line */
        t[17] = 1u;                        /* c_cc[VINTR]  = ^C */
        t[18] = 4u;                        /* c_cc[VEOF]   = ^D */
        t[19] = 0x7Fu;                     /* c_cc[VERASE] = DEL */
        t[20] = 3u;                        /* c_cc[VKILL]  = ^C */
        t[21] = 0u;                        /* c_cc[VSTART] */
        t[22] = 0u;                        /* c_cc[VSTOP]  */
        t[23] = 0x15u;                     /* c_cc[VSUSP]  = ^U */
        t[24] = 0u;
        t[25] = 0u;
        t[26] = 0u;
        t[27] = 0u;
        t[28] = 0u;
        t[29] = 0u;
        t[30] = 0u;
        t[31] = 0u;
        t[32] = 1u;                        /* c_cc[VMIN]  = 1 */
        t[33] = 0u;                        /* c_cc[VTIME] = 0 */
        t[34] = 0u;
        t[35] = 0u;
        *(u32 *)(t + 36) = 38400u;         /* ispeed */
        *(u32 *)(t + 40) = 38400u;         /* ospeed */
        return 0;
    }
    if (req == (u64)TCSETS || req == (u64)TCSETSW || req == (u64)TCSETSF) {
        if (arg == 0ull || lx_user_ok(arg, 60ull, C6OS_PROT_R) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        /* 只认 c_lflag：ICANON / ECHO 决定行规程怎么走，别的标志位
         * （波特率、校验位）我们没有硬件可设，收下不回话 */
        g_lflag = *(u32 *)((u8 *)arg + 12);
        return 0;
    }
    if (req == (u64)TIOCGWINSZ) {
        u8 *w = (u8 *)arg;

        if (w == (u8 *)0 || lx_user_ok(arg, 8ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        *(u16 *)(w + 0) = 24u;             /* ws_row */
        *(u16 *)(w + 2) = 80u;             /* ws_col */
        *(u16 *)(w + 4) = 0u;
        *(u16 *)(w + 6) = 0u;
        return 0;
    }
    if (req == (u64)FIONREAD) {
        if (arg == 0ull || lx_user_ok(arg, 4ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        (void)lx_tty_pump();
        *(u32 *)arg = (g_ppos < g_plen) ? (g_plen - g_ppos) : 0u;
        return 0;
    }
    if (req == (u64)TIOCGPGRP) {
        /* 必须和 getpgrp()/getpgid() 回 **同一个值**（都是 1）。ash 启动时做：
         *     if (tcgetpgrp(0) != getpgrp()) killpg(pgrp, SIGTTIN);
         * 只要有一个对不上，它就把自己停掉（表现为"进了用户态立刻 sig=21"）。 */
        if (arg == 0ull || lx_user_ok(arg, 4ull, C6OS_PROT_W) == 0) {
            return (i64)0 - (i64)LX_EFAULT;
        }
        *(u32 *)arg = 1u;
        return 0;
    }
    if (req == (u64)TIOCSPGRP) {
        return 0;                          /* 只有一个前台组：接受，不改 */
    }
    return (i64)0 - (i64)LX_ENOTTY;
}
