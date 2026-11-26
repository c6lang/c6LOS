/* user/c6wl/c6wl.c —— G4 最小自包含 Wayland 合成器
 *
 * 不链接 libwayland-server/pixman：socket、wire 解包、wl_shm、xdg_shell、
 * 合成全部在这里。支持一个客户端（足够 Chrome/演示客户端），把 XRGB/ARGB
 * buffer 拷进 /dev/fb0 或 PPM（测试用）。
 *
 * 用法：
 *   c6wl [--socket=PATH] [--fb=/dev/fb0] [--output=/tmp/frame.ppm]
 *        [--width=1280] [--height=800] [--frames=N] [--input=/dev/input/event0]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include "wire.h"

#ifndef C6FBIOFLUSH
struct c6_fb_rect { uint32_t x, y, w, h; };
#define C6FBIOFLUSH _IOW('F', 0x10, struct c6_fb_rect)
#endif

typedef struct {
    uint32_t id;
    uint32_t type;
    uint32_t parent;
    int      fd;
    size_t   fd_size;
    size_t   offset;
    int      w, h, stride, format;
    int      has_buffer;
    uint32_t buffer_id;
    int      configured;
    uint32_t pending_callbacks[8];
    int      ncallbacks;
} WlObj;

static WlObj g_obj[512];
static int g_nobj;
static uint32_t g_serial;
static int g_input_fd = -1;
static uint32_t g_pointer_id;
static uint32_t g_keyboard_id;
static uint32_t g_surface_id;
static int g_ptr_x = 640;
static int g_ptr_y = 400;
static int g_fd = -1;
static int g_client = -1;
static int g_running = 1;
static uint32_t g_width = 1280, g_height = 800;
static uint32_t *g_canvas;
static uint32_t *g_fb;
static size_t g_fb_len;
static int g_fb_fd = -1;
static const char *g_out;
static int g_frames;
static int g_max_frames;

static void die(const char *m)
{
    dprintf(2, "c6wl: %s: %s\n", m, strerror(errno));
    exit(2);
}

static WlObj *obj_new(uint32_t id, uint32_t type, uint32_t parent)
{
    WlObj *o;
    if (g_nobj >= (int)(sizeof(g_obj) / sizeof(g_obj[0]))) {
        return NULL;
    }
    o = &g_obj[g_nobj++];
    memset(o, 0, sizeof(*o));
    o->id = id;
    o->type = type;
    o->parent = parent;
    o->fd = -1;
    return o;
}

static WlObj *obj_get(uint32_t id)
{
    int i;
    for (i = 0; i < g_nobj; i++) {
        if (g_obj[i].id == id) return &g_obj[i];
    }
    return NULL;
}

static void obj_del(uint32_t id)
{
    int i;
    for (i = 0; i < g_nobj; i++) {
        if (g_obj[i].id == id) {
            if (g_obj[i].fd >= 0) close(g_obj[i].fd);
            g_obj[i] = g_obj[g_nobj - 1];
            g_nobj--;
            return;
        }
    }
}

/* ---- wire ---- */

typedef struct {
    uint8_t *p;
    size_t n;
    size_t off;
} Reader;

static uint32_t r_u32(Reader *r)
{
    uint32_t v = 0;
    if (r->off + 4u <= r->n) {
        memcpy(&v, r->p + r->off, 4u);
    }
    r->off += 4u;
    return v;
}

static int32_t r_i32(Reader *r)
{
    return (int32_t)r_u32(r);
}

static void r_str(Reader *r, char *out, size_t cap)
{
    uint32_t len = r_u32(r);
    size_t i;
    if (r->off + len > r->n) len = 0;
    for (i = 0; i + 1u < cap && i < len; i++) {
        out[i] = (char)r->p[r->off + i];
    }
    out[cap ? cap - 1u : 0] = 0;
    r->off += (len + 3u) & ~3u;
}

static uint32_t r_array_len(Reader *r)
{
    uint32_t n = r_u32(r);
    r->off += (n + 3u) & ~3u;
    return n;
}

static void send_msg(uint32_t oid, uint16_t opcode, const void *body, size_t n)
{
    uint8_t buf[WL_MAX_MSG];
    uint32_t hdr[2];
    size_t total = 8u + n;
    if (total > sizeof(buf)) return;
    hdr[0] = oid;
    hdr[1] = wl_word((uint32_t)total, opcode);
    memcpy(buf, hdr, 8u);
    if (n) memcpy(buf + 8u, body, n);
    {
        size_t off = 0;
        while (off < total) {
            ssize_t w = write(g_client, buf + off, total - off);
            if (w <= 0) {
                if (errno == EINTR) continue;
                g_running = 0;
                return;
            }
            off += (size_t)w;
        }
    }
}

static void send_msg_fd(uint32_t oid, uint16_t opcode, const void *body, size_t n, int fd)
{
    uint8_t buf[WL_MAX_MSG];
    uint32_t hdr[2];
    size_t total = 8u + n;
    struct iovec iov;
    struct msghdr mh;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;

    if (total > sizeof(buf)) return;
    hdr[0] = oid;
    hdr[1] = wl_word((uint32_t)total, opcode);
    memcpy(buf, hdr, 8u);
    if (n) memcpy(buf + 8u, body, n);
    memset(&mh, 0, sizeof(mh));
    iov.iov_base = buf;
    iov.iov_len = total;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    if (sendmsg(g_client, &mh, 0) < 0) {
        g_running = 0;
    }
}

static void put_u32(uint8_t *b, size_t *o, uint32_t v) { memcpy(b + *o, &v, 4); *o += 4; }
static void put_i32(uint8_t *b, size_t *o, int32_t v) { memcpy(b + *o, &v, 4); *o += 4; }
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
static void put_array_empty(uint8_t *b, size_t *o)
{
    put_u32(b, o, 0);
}

static void ev_global(uint32_t registry, uint32_t name, const char *iface, uint32_t ver)
{
    uint8_t b[256];
    size_t o = 0;
    put_u32(b, &o, name);
    put_str(b, &o, iface);
    put_u32(b, &o, ver);
    send_msg(registry, 0, b, o);
}

static void ev_done(uint32_t callback)
{
    uint8_t b[4];
    size_t o = 0;
    put_u32(b, &o, ++g_serial);
    send_msg(callback, 0, b, o);
}

static void ev_configure(WlObj *xs, WlObj *tl)
{
    if (getenv("C6WL_DEBUG")) dprintf(2, "EV configure xs=%u tl=%u\n", xs?xs->id:0, tl?tl->id:0);
    uint8_t b[128];
    size_t o = 0;
    put_u32(b, &o, ++g_serial);
    send_msg(xs ? xs->id : 0, 0, b, o);           /* xdg_surface.configure */
    if (tl) {
        o = 0;
        put_i32(b, &o, (int32_t)g_width);
        put_i32(b, &o, (int32_t)g_height);
        put_array_empty(b, &o);
        send_msg(tl->id, 0, b, o);                /* xdg_toplevel.configure */
    }
}

static void ev_seat_caps(WlObj *seat)
{
    uint8_t b[4];
    size_t o = 0;
    put_u32(b, &o, 3u);                           /* pointer + keyboard */
    send_msg(seat->id, 0, b, o);
}

static void ev_output(WlObj *out)
{
    uint8_t b[256];
    size_t o = 0;
    put_i32(b, &o, 0); put_i32(b, &o, 0);         /* x,y */
    put_i32(b, &o, (int32_t)g_width); put_i32(b, &o, (int32_t)g_height);
    put_i32(b, &o, 0);                            /* subpixel */
    put_str(b, &o, "c6wl");
    put_str(b, &o, "c6wl");
    put_i32(b, &o, 1);                            /* transform normal */
    send_msg(out->id, 0, b, o);                   /* geometry */
    o = 0;
    put_u32(b, &o, 0);                            /* flags current */
    put_i32(b, &o, (int32_t)g_width); put_i32(b, &o, (int32_t)g_height);
    put_i32(b, &o, 60000);
    send_msg(out->id, 1, b, o);                   /* mode */
    o = 0; put_u32(b, &o, 1); send_msg(out->id, 2, b, o); /* done */
}

/* ---- 合成 ---- */

static void ppm_write(void)
{
    FILE *f;
    uint32_t x, y;
    if (!g_out) return;
    f = fopen(g_out, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", g_width, g_height);
    for (y = 0; y < g_height; y++) {
        for (x = 0; x < g_width; x++) {
            uint32_t p = g_canvas[y * g_width + x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void fb_present(void)
{
    if (g_fb) {
        uint32_t y;
        size_t stride = g_fb_len / g_height / 4u;
        for (y = 0; y < g_height; y++) {
            memcpy(g_fb + y * stride, g_canvas + y * g_width, (size_t)g_width * 4u);
        }
        {
            struct c6_fb_rect r = { 0, 0, g_width, g_height };
            if (g_fb_fd >= 0) (void)ioctl(g_fb_fd, C6FBIOFLUSH, &r);
        }
    } else {
        ppm_write();
    }
}

static WlObj *surface_xdg(WlObj *s)
{
    int i;
    for (i = 0; i < g_nobj; i++) {
        if (g_obj[i].type == C6WL_T_XDG_SURFACE && g_obj[i].parent == s->id) {
            return &g_obj[i];
        }
    }
    return NULL;
}

static WlObj *surface_toplevel(WlObj *s)
{
    WlObj *xs = surface_xdg(s);
    int i;
    if (!xs) return NULL;
    for (i = 0; i < g_nobj; i++) {
        if (g_obj[i].type == C6WL_T_XDG_TOPLEVEL && g_obj[i].parent == xs->id) {
            return &g_obj[i];
        }
    }
    return NULL;
}

static void composite(WlObj *surf)
{
    WlObj *buf = obj_get(surf->buffer_id);
    if (!buf || buf->fd < 0) return;
    {
        uint32_t x, y;
        size_t map_len = (size_t)buf->stride * (size_t)buf->h + buf->offset;
        uint8_t *src = mmap(NULL, map_len ? map_len : 1u, PROT_READ, MAP_SHARED,
                            buf->fd, 0);
        if (src == MAP_FAILED) return;
        for (y = 0; y < g_height && y < (uint32_t)buf->h; y++) {
            for (x = 0; x < g_width && x < (uint32_t)buf->w; x++) {
                uint32_t p;
                memcpy(&p, src + buf->offset + y * (size_t)buf->stride + x * 4u, 4u);
                if (buf->format == WL_SHM_FORMAT_ARGB8888) {
                    uint32_t a = p >> 24;
                    if (a == 0) continue;
                    if (a == 255) {
                        g_canvas[y * g_width + x] = p & 0xFFFFFFu;
                    } else {
                        uint32_t d = g_canvas[y * g_width + x];
                        uint32_t r = (((p >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * (255u - a)) / 255u;
                        uint32_t g = (((p >> 8) & 0xFFu) * a + ((d >> 8) & 0xFFu) * (255u - a)) / 255u;
                        uint32_t b = ((p & 0xFFu) * a + (d & 0xFFu) * (255u - a)) / 255u;
                        g_canvas[y * g_width + x] = (r << 16) | (g << 8) | b;
                    }
                } else {
                    g_canvas[y * g_width + x] = p & 0xFFFFFFu;
                }
            }
        }
        munmap(src, map_len ? map_len : 1u);
    }
    fb_present();
    g_frames++;
    if (g_max_frames > 0 && g_frames >= g_max_frames) {
        g_running = 0;
    }
}

/* ---- 请求分发 ---- */

static void on_bind(WlObj *reg, Reader *r)
{
    uint32_t name = r_u32(r);
    char iface[128];
    uint32_t ver = 0;
    uint32_t id = 0;
    WlObj *o;
    r_str(r, iface, sizeof(iface));
    ver = r_u32(r);
    id = r_u32(r);
    (void)reg; (void)name; (void)ver;
    if (strcmp(iface, "wl_compositor") == 0) {
        o = obj_new(id, C6WL_T_COMPOSITOR, 0);
    } else if (strcmp(iface, "wl_shm") == 0) {
        o = obj_new(id, C6WL_T_SHM, 0);
        {
            uint8_t b[4]; size_t k = 0;
            put_u32(b, &k, WL_SHM_FORMAT_ARGB8888);
            send_msg(id, 0, b, k);
            k = 0;
            put_u32(b, &k, WL_SHM_FORMAT_XRGB8888);
            send_msg(id, 0, b, k);
        }
    } else if (strcmp(iface, "wl_seat") == 0) {
        o = obj_new(id, C6WL_T_SEAT, 0);
        ev_seat_caps(o);
    } else if (strcmp(iface, "wl_output") == 0) {
        o = obj_new(id, C6WL_T_OUTPUT, 0);
        ev_output(o);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        obj_new(id, C6WL_T_XDG_WM, 0);
    } else if (strcmp(iface, "wl_subcompositor") == 0) {
        obj_new(id, C6WL_T_SUBCOMPOSITOR, 0);
    } else if (strcmp(iface, "wl_data_device_manager") == 0) {
        obj_new(id, C6WL_T_DATA_MGR, 0);
    } else {
        return;
    }
}

static void dispatch(uint32_t oid, uint16_t opcode, Reader *r, int fd)
{
    WlObj *o = obj_get(oid);
    if (getenv("C6WL_DEBUG")) dprintf(2, "REQ oid=%u op=%u type=%u\n", oid, opcode, o ? o->type : 0);
    if (!o) return;
    switch (o->type) {
    case C6WL_T_DISPLAY:
        if (opcode == 0) {                           /* sync */
            uint32_t id = r_u32(r);
            WlObj *cb = obj_new(id, C6WL_T_CALLBACK, 0);
            if (cb) ev_done(cb->id);
        } else if (opcode == 1) {                    /* get_registry */
            uint32_t id = r_u32(r);
            WlObj *reg;
            if (id == 0) return;
            reg = obj_new(id, C6WL_T_REGISTRY, 0);
            if (reg) {
                ev_global(id, C6WL_NAME_COMPOSITOR, "wl_compositor", 4);
                ev_global(id, C6WL_NAME_SHM, "wl_shm", 1);
                ev_global(id, C6WL_NAME_SEAT, "wl_seat", 5);
                ev_global(id, C6WL_NAME_OUTPUT, "wl_output", 3);
                ev_global(id, C6WL_NAME_XDG_WM, "xdg_wm_base", 2);
                ev_global(id, C6WL_NAME_SUBCOMP, "wl_subcompositor", 1);
                ev_global(id, C6WL_NAME_DATA_MGR, "wl_data_device_manager", 3);
            }
        }
        break;
    case C6WL_T_REGISTRY:
        if (opcode == 0) on_bind(o, r);
        break;
    case C6WL_T_COMPOSITOR:
        if (opcode == 0) {                           /* create_surface */
            uint32_t id = r_u32(r);
            obj_new(id, C6WL_T_SURFACE, o->id);
        }
        break;
    case C6WL_T_SHM:
        if (opcode == 0) {                           /* create_pool */
            uint32_t id = r_u32(r);
            uint32_t size = r_u32(r);
            WlObj *p = obj_new(id, C6WL_T_SHM_POOL, o->id);
            if (p) {
                p->fd = fd;
                p->fd_size = size;
            }
        }
        break;
    case C6WL_T_SHM_POOL:
        if (opcode == 0) {                           /* create_buffer */
            uint32_t id = r_u32(r);
            uint32_t off = r_u32(r);
            uint32_t w = r_u32(r);
            uint32_t h = r_u32(r);
            uint32_t stride = r_u32(r);
            uint32_t fmt = r_u32(r);
            WlObj *b = obj_new(id, C6WL_T_BUFFER, o->id);
            if (b) {
                b->fd = -1;
                b->fd_size = o->fd_size;
                b->offset = off;
                b->w = (int)w; b->h = (int)h;
                b->stride = (int)stride; b->format = (int)fmt;
                /* dup pool fd，pool 销毁后 buffer 仍可读 */
                if (o->fd >= 0) {
                    b->fd = dup(o->fd);
                }
            }
        }
        break;
    case C6WL_T_XDG_WM:
        if (opcode == 2) {                           /* get_xdg_surface */
            uint32_t id = r_u32(r);
            uint32_t sid = r_u32(r);
            obj_new(id, C6WL_T_XDG_SURFACE, sid);
        } else if (opcode == 3) {                    /* pong */
            r_u32(r);
        }
        break;
    case C6WL_T_XDG_SURFACE:
        if (opcode == 1) {                           /* get_toplevel */
            uint32_t id = r_u32(r);
            obj_new(id, C6WL_T_XDG_TOPLEVEL, o->id);
            {
                WlObj *s = obj_get(o->parent);
                WlObj *tl = obj_get(id);
                if (s && tl) ev_configure(o, tl);
            }
        } else if (opcode == 4) {                    /* ack_configure */
            r_u32(r);
        }
        break;
    case C6WL_T_XDG_TOPLEVEL:
        /* set_title/app_id/... 忽略；move/resize 也无窗口管理器 */
        break;
    case C6WL_T_SURFACE:
        if (opcode == 1) {                           /* attach */
            uint32_t bid = r_u32(r);
            r_i32(r); r_i32(r);
            o->buffer_id = bid;
            o->has_buffer = bid != 0;
        } else if (opcode == 2) {                    /* damage */
            r_i32(r); r_i32(r); r_i32(r); r_i32(r);
        } else if (opcode == 3) {                    /* frame */
            uint32_t id = r_u32(r);
            if (o->ncallbacks < (int)(sizeof(o->pending_callbacks) / sizeof(uint32_t))) {
                o->pending_callbacks[o->ncallbacks++] = id;
            }
            obj_new(id, C6WL_T_CALLBACK, o->id);
        } else if (opcode == 6) {                    /* commit */
            WlObj *xs = surface_xdg(o);
            WlObj *tl = surface_toplevel(o);
            if (xs && !o->configured) {
                ev_configure(xs, tl);
                o->configured = 1;
            }
            if (o->has_buffer) {
                g_surface_id = o->id;
                if (g_pointer_id != 0u) {
                    uint8_t pb[20]; size_t po = 0;
                    put_u32(pb, &po, ++g_serial);
                    put_u32(pb, &po, o->id);
                    put_i32(pb, &po, (int32_t)(g_ptr_x << 8));
                    put_i32(pb, &po, (int32_t)(g_ptr_y << 8));
                    send_msg(g_pointer_id, 0, pb, po);   /* enter */
                }
                if (g_keyboard_id != 0u) {
                    uint8_t kb[16]; size_t ko = 0;
                    put_u32(kb, &ko, ++g_serial);
                    put_u32(kb, &ko, o->id);
                    put_array_empty(kb, &ko);
                    send_msg(g_keyboard_id, 1, kb, ko);  /* enter */
                }
                composite(o);
                {
                    int i;
                    for (i = 0; i < o->ncallbacks; i++) {
                        WlObj *cb = obj_get(o->pending_callbacks[i]);
                        if (cb) ev_done(cb->id);
                    }
                    o->ncallbacks = 0;
                }
            }
        }
        break;
    case C6WL_T_BUFFER:
        if (opcode == 0) obj_del(o->id);
        break;
    case C6WL_T_SUBCOMPOSITOR:
        if (opcode == 0) {
            uint32_t id = r_u32(r), surf = r_u32(r), parent = r_u32(r);
            (void)surf; (void)parent;
            obj_new(id, C6WL_T_DUMMY, o->id);
        }
        break;
    case C6WL_T_DATA_MGR:
        if (opcode == 0) {
            uint32_t id = r_u32(r), seat = r_u32(r);
            (void)seat;
            obj_new(id, C6WL_T_DUMMY, o->id);
        }
        break;
    case C6WL_T_DUMMY:
        break;
    case C6WL_T_SEAT:
        if (opcode == 0) {                           /* get_pointer */
            uint32_t id = r_u32(r);
            obj_new(id, C6WL_T_POINTER, o->id);
            g_pointer_id = id;
        } else if (opcode == 1) {                    /* get_keyboard */
            uint32_t id = r_u32(r);
            WlObj *k = obj_new(id, C6WL_T_KEYBOARD, o->id);
            g_keyboard_id = id;
            if (k) {
                /* 内置 US keymap：EVDEV 的 keycode 偏移 8 后映射到 XKB keycode，
                 * 这里只发一个最小 keymap，Chrome 拿它做键位解释。 */
                static const char map[] =
                    "xkb_keymap {\n"
                    "xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
                    "xkb_types { include \"complete\" };\n"
                    "xkb_compat { include \"complete\" };\n"
                    "xkb_symbols { include \"pc+us\" };\n"
                    "};\n";
                int fd = memfd_create("c6wl-keymap", 0);
                if (fd >= 0) {
                    uint8_t b[1024]; size_t z = 0;
                    off_t sz = (off_t)sizeof(map);
                    if (write(fd, map, sizeof(map)) < 0) { /* ignore */ }
                    /* wl_keyboard.keymap(format, fd, size)：fd 走 SCM_RIGHTS */
                    put_u32(b, &z, 1u);            /* XKB_V1 */
                    put_u32(b, &z, (uint32_t)sz);
                    send_msg_fd(k->id, 0, b, z, fd);
                    close(fd);
                }
            }
        }
        break;
    default:
        break;
    }
}

/* ---- socket ---- */

static void server_socket(const char *path)
{
    struct sockaddr_un a;
    int one = 1;
    if (strlen(path) >= sizeof(a.sun_path)) die("socket path too long");
    unlink(path);
    g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_fd < 0) die("socket");
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    setsockopt(g_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(g_fd, (struct sockaddr *)&a, sizeof(a)) != 0) die("bind");
    if (listen(g_fd, 1) != 0) die("listen");
    chmod(path, 0600);
}

static int open_fb(const char *path)
{
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (!path || !*path) return 0;
    g_fb_fd = open(path, O_RDWR);
    if (g_fb_fd < 0) return -1;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        return -1;
    }
    g_width = vi.xres;
    g_height = vi.yres;
    g_fb_len = (size_t)fi.line_length * g_height;
    g_fb = mmap(NULL, g_fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb == MAP_FAILED) { g_fb = NULL; return -1; }
    return 0;
}

static void input_handle(const struct input_event *ev)
{
    if (ev->type == EV_REL && g_pointer_id != 0u) {
        if (ev->code == REL_X) g_ptr_x += ev->value;
        if (ev->code == REL_Y) g_ptr_y += ev->value;
        if (g_ptr_x < 0) g_ptr_x = 0;
        if (g_ptr_y < 0) g_ptr_y = 0;
        if ((uint32_t)g_ptr_x >= g_width) g_ptr_x = (int)g_width - 1;
        if ((uint32_t)g_ptr_y >= g_height) g_ptr_y = (int)g_height - 1;
        {
            uint8_t b[16]; size_t o = 0;
            put_u32(b, &o, ++g_serial);
            put_u32(b, &o, 0u);                  /* time */
            put_i32(b, &o, (int32_t)(g_ptr_x << 8));
            put_i32(b, &o, (int32_t)(g_ptr_y << 8));
            send_msg(g_pointer_id, 2, b, o);     /* wl_pointer.motion */
        }
        return;
    }
    if (ev->type == EV_KEY && ev->code == BTN_LEFT && g_pointer_id != 0u) {
        uint8_t b[20]; size_t o = 0;
        put_u32(b, &o, ++g_serial);
        put_u32(b, &o, 0u);
        put_u32(b, &o, (uint32_t)BTN_LEFT);
        put_u32(b, &o, ev->value ? 1u : 0u);
        send_msg(g_pointer_id, 3, b, o);         /* wl_pointer.button */
        return;
    }
    if (ev->type == EV_KEY && g_keyboard_id != 0u) {
        uint8_t b[16]; size_t o = 0;
        put_u32(b, &o, ++g_serial);
        put_u32(b, &o, 0u);
        put_u32(b, &o, (uint32_t)ev->code);
        put_u32(b, &o, ev->value ? 1u : 0u);
        send_msg(g_keyboard_id, 3, b, o);        /* wl_keyboard.key */
    }
}

static int client_read_(void)
{
    uint8_t buf[WL_MAX_MSG * 2];
    uint8_t cmsgbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr mh;
    ssize_t n;
    int fd = -1;
    struct cmsghdr *cm;

    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsgbuf;
    mh.msg_controllen = sizeof(cmsgbuf);
    n = recvmsg(g_client, &mh, 0);
    if (n <= 0) {
        return -1;
    }
    for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
        }
    }
    {
        size_t off = 0;
        while (off + 8u <= (size_t)n) {
            uint32_t oid, w;
            uint16_t op;
            size_t msz;
            Reader rr;
            memcpy(&oid, buf + off, 4u);
            memcpy(&w, buf + off + 4u, 4u);
            op = (uint16_t)(w & 0xFFFFu);
            msz = (size_t)(w >> 16);
            if (msz < 8u || off + msz > (size_t)n) break;
            rr.p = buf + off + 8u;
            rr.n = msz - 8u;
            rr.off = 0;
            dispatch(oid, op, &rr, fd);
            fd = -1;
            off += msz;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *sockpath = NULL;
    const char *fbpath = "/dev/fb0";
    char sockbuf[256];
    int i;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--socket=", 9)) sockpath = argv[i] + 9;
        else if (!strncmp(argv[i], "--fb=", 5)) fbpath = argv[i] + 5;
        else if (!strncmp(argv[i], "--output=", 9)) { g_out = argv[i] + 9; fbpath = ""; }
        else if (!strncmp(argv[i], "--width=", 8)) g_width = (uint32_t)atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--height=", 9)) g_height = (uint32_t)atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--frames=", 9)) g_max_frames = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--input=", 8)) g_input_fd = open(argv[i] + 8, O_RDONLY | O_NONBLOCK);
    }
    if (!sockpath) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        snprintf(sockbuf, sizeof(sockbuf), "%s/wayland-0",
                 (xdg && *xdg) ? xdg : "/tmp");
        sockpath = sockbuf;
    }
    signal(SIGPIPE, SIG_IGN);
    (void)obj_new(1u, C6WL_T_DISPLAY, 0u);
    g_canvas = calloc((size_t)g_width * g_height, 4u);
    if (!g_canvas) die("calloc");
    if (open_fb(fbpath) != 0 && !g_out) {
        die("open fb");
    }
    server_socket(sockpath);
    dprintf(1, "c6wl: listening on %s (%ux%u)\n", sockpath, g_width, g_height);
    g_client = accept(g_fd, NULL, NULL);
    if (g_client < 0) die("accept");
    dprintf(1, "c6wl: client connected\n");

    while (g_running) {
        struct pollfd pf[2];
        int nf = 1;
        int pr;

        pf[0].fd = g_client;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        if (g_input_fd >= 0) {
            pf[nf].fd = g_input_fd;
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            nf++;
        }
        pr = poll(pf, (nfds_t)nf, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if ((pf[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            if (client_read_() != 0) break;
        }
        if (nf > 1 && (pf[1].revents & POLLIN) != 0) {
            struct input_event ev[32];
            ssize_t r = read(g_input_fd, ev, sizeof(ev));
            ssize_t k;
            for (k = 0; k + (ssize_t)sizeof(ev[0]) <= r; k += (ssize_t)sizeof(ev[0])) {
                input_handle(&ev[k / (ssize_t)sizeof(ev[0])]);
            }
        }
    }
    if (g_out) {
        ppm_write();
        dprintf(1, "c6wl: output %s (%d frames)\n", g_out, g_frames);
    }
    if (g_fb) munmap(g_fb, g_fb_len);
    close(g_client);
    close(g_fd);
    unlink(sockpath);
    free(g_canvas);
    return 0;
}
