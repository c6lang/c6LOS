/* tests/linux/wl_demo.c —— G4：最小 Wayland 客户端（自包含 wire，不链接 libwayland）
 *
 * 连接 c6wl，走 wl_shm + xdg_shell：全屏红底 + 三帧移动的绿色方块。
 * 退出后由 check 脚本读 c6wl 的 PPM。
 *
 * 用法：wl_demo --socket=/run/user/0/wayland-0 [--width=1280 --height=800]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "../../user/c6wl/wire.h"

static int g_fd = -1;
static uint32_t g_w = 1280, g_h = 800;
static uint32_t g_serial;
static int g_configured;
static int g_frame_done;

static void die(const char *m)
{
    dprintf(2, "wl_demo: %s: %s\n", m, strerror(errno));
    exit(2);
}

static void put_u32(uint8_t *b, size_t *o, uint32_t v) { memcpy(b + *o, &v, 4u); *o += 4u; }
static void put_str(uint8_t *b, size_t *o, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;
    size_t pad = (4u - (n & 3u)) & 3u;
    put_u32(b, o, n);
    memcpy(b + *o, s, n);
    *o += n;
    memset(b + *o, 0, pad);
    *o += pad;
}

static void send_msg(uint32_t oid, uint16_t opcode, const void *body, size_t n)
{
    uint8_t buf[WL_MAX_MSG];
    uint32_t hdr[2];
    size_t total = 8u + n;
    size_t off = 0;
    hdr[0] = oid;
    hdr[1] = wl_word((uint32_t)total, opcode);
    memcpy(buf, hdr, 8u);
    memcpy(buf + 8u, body, n);
    while (off < total) {
        ssize_t w = write(g_fd, buf + off, total - off);
        if (w <= 0) {
            if (errno == EINTR) continue;
            die("write");
        }
        off += (size_t)w;
    }
}

static void send_fd_msg(uint32_t oid, uint16_t opcode, const void *body, size_t n, int fd)
{
    uint8_t buf[WL_MAX_MSG];
    uint32_t hdr[2];
    size_t total = 8u + n;
    struct iovec iov;
    struct msghdr mh;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;

    hdr[0] = oid;
    hdr[1] = wl_word((uint32_t)total, opcode);
    memcpy(buf, hdr, 8u);
    memcpy(buf + 8u, body, n);
    memset(&mh, 0, sizeof(mh));
    iov.iov_base = buf;
    iov.iov_len = total;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    if (sendmsg(g_fd, &mh, 0) < 0) die("sendmsg");
}

typedef struct { uint8_t b[WL_MAX_MSG]; size_t n; size_t off; } Rx;

static uint32_t r_u32(Rx *r)
{
    uint32_t v = 0;
    if (r->off + 4u <= r->n) memcpy(&v, r->b + r->off, 4u);
    r->off += 4u;
    return v;
}
static void r_str(Rx *r, char *s, size_t cap)
{
    uint32_t n = r_u32(r);
    size_t i;
    for (i = 0; i + 1u < cap && i < n; i++) s[i] = (char)r->b[r->off + i];
    s[cap - 1u] = 0;
    r->off += (n + 3u) & ~3u;
}

/* 读取并处理事件；返回 -1 断开，0 正常。最多等到 want 条件成立。 */
static int pump(int want_configure, int want_frame)
{
    for (;;) {
        uint8_t buf[WL_MAX_MSG * 2];
        ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
        if (n <= 0) return -1;
        {
            size_t off = 0;
            while (off + 8u <= (size_t)n) {
                uint32_t oid, w;
                uint16_t op;
                size_t msz;
                Rx r;
                memcpy(&oid, buf + off, 4u);
                memcpy(&w, buf + off + 4u, 4u);
                op = (uint16_t)(w & 0xFFFFu);
                msz = (size_t)(w >> 16);
                if (msz < 8u || off + msz > (size_t)n) break;
                r.b[0] = 0;
                r.n = msz - 8u;
                r.off = 0;
                memcpy(r.b, buf + off + 8u, r.n);
                if (getenv("WL_DEBUG")) dprintf(2, "EV oid=%u op=%u sz=%zu\n", oid, op, msz);
                if (oid == 8u && op == 0) {              /* xdg_surface.configure */
                    uint32_t serial = r_u32(&r);
                    uint8_t ab[4]; size_t ao = 0;
                    put_u32(ab, &ao, serial);
                    send_msg(8u, 4u, ab, ao);            /* ack_configure */
                    g_configured = 1;
                } else if (oid == 9u && op == 0) {       /* xdg_toplevel.configure */
                    (void)r_u32(&r); (void)r_u32(&r);
                } else if (oid == 12u && op == 0) {      /* frame done */
                    g_serial = r_u32(&r);
                    g_frame_done = 1;
                }
                off += msz;
            }
        }
        if ((!want_configure || g_configured) && (!want_frame || g_frame_done)) {
            return 0;
        }
    }
}

int main(int argc, char **argv)
{
    const char *sockpath = NULL;
    char sockbuf[256];
    struct sockaddr_un a;
    int i;
    uint8_t b[256];
    size_t o;
    int mfd;
    uint32_t *pix;
    int frame;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--socket=", 9)) sockpath = argv[i] + 9;
        else if (!strncmp(argv[i], "--width=", 8)) g_w = (uint32_t)atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--height=", 9)) g_h = (uint32_t)atoi(argv[i] + 9);
    }
    if (!sockpath) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        snprintf(sockbuf, sizeof(sockbuf), "%s/wayland-0", (xdg && *xdg) ? xdg : "/tmp");
        sockpath = sockbuf;
    }
    g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_fd < 0) die("socket");
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sockpath, sizeof(a.sun_path) - 1u);
    {
        int try;
        for (try = 0; try < 2000; try++) {
            if (connect(g_fd, (struct sockaddr *)&a, sizeof(a)) == 0) break;
            sched_yield();
        }
        if (try == 2000) die("connect");
    }

    /* 1. registry */
    o = 0; put_u32(b, &o, 2u);
    send_msg(1u, 1u, b, o);                          /* wl_display.get_registry */
    {
        /* 读五条 global，绑定 compositor/shm/seat/xdg */
        int got = 0;
        while (got < 5) {
            uint8_t buf[WL_MAX_MSG];
            ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
            if (n <= 0) die("recv globals");
            {
                size_t off = 0;
                while (off + 8u <= (size_t)n) {
                    uint32_t oid, w;
                    uint16_t op;
                    size_t msz;
                    Rx r;
                    char iface[128];
                    uint32_t name;
                    memcpy(&oid, buf + off, 4u);
                    memcpy(&w, buf + off + 4u, 4u);
                    op = (uint16_t)(w & 0xFFFFu);
                    msz = (size_t)(w >> 16);
                    if (msz < 8u || off + msz > (size_t)n) break;
                    r.n = msz - 8u; r.off = 0;
                    memcpy(r.b, buf + off + 8u, r.n);
                    if (oid == 2u && op == 0) {
                        name = r_u32(&r);
                        r_str(&r, iface, sizeof(iface));
                        (void)r_u32(&r);                /* version */
                        if (!strcmp(iface, "wl_compositor")) {
                            size_t x = 0; put_u32(b, &x, name); put_str(b, &x, iface);
                            put_u32(b, &x, 4u); put_u32(b, &x, 3u);
                            send_msg(2u, 0u, b, x); got++;
                        } else if (!strcmp(iface, "wl_shm")) {
                            size_t x = 0; put_u32(b, &x, name); put_str(b, &x, iface);
                            put_u32(b, &x, 1u); put_u32(b, &x, 4u);
                            send_msg(2u, 0u, b, x); got++;
                        } else if (!strcmp(iface, "wl_seat")) {
                            size_t x = 0; put_u32(b, &x, name); put_str(b, &x, iface);
                            put_u32(b, &x, 5u); put_u32(b, &x, 5u);
                            send_msg(2u, 0u, b, x); got++;
                        } else if (!strcmp(iface, "xdg_wm_base")) {
                            size_t x = 0; put_u32(b, &x, name); put_str(b, &x, iface);
                            put_u32(b, &x, 2u); put_u32(b, &x, 6u);
                            send_msg(2u, 0u, b, x); got++;
                        } else {
                            got++;                  /* output 不 bind 也算收到 */
                        }
                    }
                    off += msz;
                }
            }
        }
    }

    /* 2. surface / xdg_surface / toplevel */
    o = 0; put_u32(b, &o, 7u);
    send_msg(3u, 0u, b, o);                          /* create_surface */
    o = 0; put_u32(b, &o, 8u); put_u32(b, &o, 7u);
    send_msg(6u, 2u, b, o);                          /* get_xdg_surface */
    o = 0; put_u32(b, &o, 9u);
    send_msg(8u, 1u, b, o);                          /* get_toplevel */
    send_msg(7u, 6u, NULL, 0u);                      /* commit -> configure */
    if (pump(1, 0) != 0) die("configure");

    /* 3. shm pool + buffer */
    mfd = memfd_create("wl-demo", 0);
    if (mfd < 0) die("memfd");
    if (ftruncate(mfd, (off_t)g_w * g_h * 4) != 0) die("ftruncate");
    pix = mmap(NULL, (size_t)g_w * g_h * 4u, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (pix == MAP_FAILED) die("mmap");
    for (i = 0; i < (int)(g_w * g_h); i++) pix[i] = 0x00ff0000u;
    o = 0; put_u32(b, &o, 10u); put_u32(b, &o, g_w * g_h * 4u);
    send_fd_msg(4u, 0u, b, o, mfd);                  /* create_pool */
    o = 0;
    put_u32(b, &o, 11u); put_u32(b, &o, 0u);
    put_u32(b, &o, g_w); put_u32(b, &o, g_h);
    put_u32(b, &o, g_w * 4u); put_u32(b, &o, WL_SHM_FORMAT_XRGB8888);
    send_msg(10u, 0u, b, o);                         /* create_buffer */

    /* 4. 三帧移动方块：每帧 attach/damage/frame/commit，等 frame done */
    for (frame = 0; frame < 3; frame++) {
        int x0 = 100 + frame * 200;
        int y0 = 250;
        for (int y = 0; y < (int)g_h; y++) {
            for (int x = 0; x < (int)g_w; x++) {
                int in = x >= x0 && x < x0 + 120 && y >= y0 && y < y0 + 120;
                pix[y * (int)g_w + x] = in ? 0x0000ff00u : 0x00ff0000u;
            }
        }
        o = 0; put_u32(b, &o, 11u); put_u32(b, &o, 0u); put_u32(b, &o, 0u);
        send_msg(7u, 1u, b, o);                      /* attach */
        o = 0; put_u32(b, &o, 0u); put_u32(b, &o, 0u);
        put_u32(b, &o, g_w); put_u32(b, &o, g_h);
        send_msg(7u, 2u, b, o);                      /* damage */
        o = 0; put_u32(b, &o, 12u);
        send_msg(7u, 3u, b, o);                      /* frame */
        g_frame_done = 0;
        send_msg(7u, 6u, NULL, 0u);                  /* commit */
        if (pump(0, 1) != 0) die("frame done");
    }
    send_msg(1u, 0u, NULL, 0u);                      /* wl_display.sync */
    close(g_fd);
    return 0;
}
