/* user/c6chrome-display.c —— G3：把 headless Chrome 的帧显示到 /dev/fb0
 *
 * 这个程序是普通静态 Linux ELF，跑在 c6os 的 Linux ABI 里。它通过
 * `--remote-debugging-pipe` 和 Chrome 说话：
 *   fd3 = 命令（父 → Chrome），fd4 = 应答/事件（Chrome → 父）；
 * 每条消息是一个 NUL 结尾的 JSON 对象。
 *
 * 帧走 Page.startScreencast（PNG），这里只用 zlib 解 PNG，不依赖 libpng。
 * 显示刷新用 mmap(/dev/fb0) + C6FBIOFLUSH；输入从 /dev/input/event0 读，
 * 转成 CDP 的 Input.dispatch* 事件。
 *
 * 用法：
 *   c6chrome-display --chrome=/usr/bin/chromium --url=http://example.com \
 *       --fb=/dev/fb0 --width=1280 --height=800 [--display=:0]
 *   c6chrome-display --self-test                 # 只跑 base64/PNG 单测
 *   c6chrome-display ... --output=/tmp/frame.ppm # 宿主侧验证（不写 fb）
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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#ifndef C6FBIOFLUSH
struct c6_fb_rect {
    uint32_t x, y, w, h;
};
#define C6FBIOFLUSH _IOW('F', 0x10, struct c6_fb_rect)
#endif

#define MSG_MAX (16u * 1024u * 1024u)

static int g_cmd = -1;
static int g_resp = -1;
static int g_next_id = 1;

static void die(const char *m)
{
    dprintf(2, "c6chrome-display: %s: %s\n", m, strerror(errno));
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1u);
    if (!p) {
        die("malloc");
    }
    return p;
}

/* ---- 极简 JSON 取值（Chrome 的 CDP 消息结构固定，不引入 JSON 库）---- */

static const char *json_key(const char *s, const char *key)
{
    size_t n = strlen(key);
    const char *p = s;

    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, n) == 0 && p[n] == '"') {
            p += n + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == ':') {
                p++;
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                return p;
            }
        }
        while (*p && *p != '"') {
            p++;
        }
        if (*p == '"') {
            p++;
        }
    }
    return NULL;
}

static int json_str(const char *s, const char *key, char *out, size_t cap)
{
    const char *p = json_key(s, key);
    size_t w = 0;

    if (!p || *p != '"') {
        return -1;
    }
    p++;
    while (*p && *p != '"' && w + 1u < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[w++] = '\n'; break;
            case 'r': out[w++] = '\r'; break;
            case 't': out[w++] = '\t'; break;
            case 'b': out[w++] = '\b'; break;
            case 'f': out[w++] = '\f'; break;
            case '"': out[w++] = '"'; break;
            case '\\': out[w++] = '\\'; break;
            case '/': out[w++] = '/'; break;
            default: out[w++] = *p; break;
            }
            p++;
        } else {
            out[w++] = *p++;
        }
    }
    if (*p != '"') {
        return -1;
    }
    out[w] = 0;
    return 0;
}

static int json_int(const char *s, const char *key, long *out)
{
    const char *p = s;

    /* 同名 key 可能先出现字符串形式（flatten 事件的顶层 sessionId 是目标
     * session，params 里的 sessionId 才是 screencast 帧号），这里找第一个
     * 数字形态的。 */
    while ((p = json_key(p, key)) != NULL) {
        char *e;
        long v = strtol(p, &e, 10);
        if (e != p) {
            *out = v;
            return 0;
        }
        /* 跳过整个值，保证下一轮仍从引号/逗号边界开始；只 p++ 会
         * 落进字符串内部，json_key 就再也认不出后面的同名 key。 */
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
    }
    return -1;
}

static int json_bool(const char *s, const char *key, int *out)
{
    const char *p = json_key(s, key);

    if (!p) {
        return -1;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    while (*in && w + 2u < cap) {
        if (*in == '"' || *in == '\\') {
            out[w++] = '\\';
            out[w++] = *in++;
        } else if ((unsigned char)*in < 0x20) {
            if (w + 6u >= cap) break;
            w += (size_t)snprintf(out + w, cap - w, "\\u%04x", *in++);
        } else {
            out[w++] = *in++;
        }
    }
    out[w] = 0;
}

/* ---- base64 ---- */

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t w = 0;
    int quad[4];
    int qn = 0;

    while (*in) {
        int v;
        while (*in == '\n' || *in == '\r' || *in == ' ' || *in == '\t') {
            in++;
        }
        if (*in == '=') {
            break;
        }
        v = b64v((unsigned char)*in++);
        if (v < 0) {
            break;
        }
        quad[qn++] = v;
        if (qn == 4) {
            uint32_t x = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12) |
                         ((uint32_t)quad[2] << 6) | (uint32_t)quad[3];
            if (w + 3u <= cap) {
                out[w++] = (uint8_t)(x >> 16);
                out[w++] = (uint8_t)(x >> 8);
                out[w++] = (uint8_t)x;
            }
            qn = 0;
        }
    }
    if (qn == 2) {
        if (w < cap) out[w++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
    } else if (qn == 3) {
        if (w < cap) out[w++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
        if (w < cap) out[w++] = (uint8_t)((quad[1] << 4) | (quad[2] >> 2));
    }
    return w;
}

/* ---- PNG：只支持 Chrome screenshot 会产生的 8bit RGB/RGBA、非隔行 ---- */

typedef struct {
    int w;
    int h;
    int bpp;
    uint8_t *rgba;                 /* w*h*4，XRGB 输出用 */
} Png;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int png_decode(const uint8_t *d, size_t n, Png *im)
{
    size_t off = 8u;
    int w = 0, h = 0, ct = 0, bd = 0, inter = 0;
    uint8_t *idat = NULL;
    size_t idn = 0, idcap = 0;
    uint8_t *raw = NULL;
    size_t rawcap = 0;
    z_stream z;
    int zr;
    int ret = -1;

    if (n < 8u || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0) {
        return -1;
    }
    while (off + 12u <= n) {
        uint32_t len = be32(d + off);
        uint32_t typ = be32(d + off + 4u);
        const uint8_t *body = d + off + 8u;
        if (off + 12u + (size_t)len > n) {
            goto out;
        }
        if (typ == 0x49484452u && len >= 13u) {         /* IHDR */
            w = (int)be32(body);
            h = (int)be32(body + 4u);
            bd = body[8];
            ct = body[9];
            inter = body[12];
            if (bd != 8 || (ct != 2 && ct != 6) || inter != 0 ||
                w <= 0 || h <= 0 || w > 16384 || h > 16384) {
                goto out;
            }
        } else if (typ == 0x49444154u) {                /* IDAT */
            if (idn + len > idcap) {
                size_t nc = idcap ? idcap * 2u : 65536u;
                while (nc < idn + len) nc *= 2u;
                idat = realloc(idat, nc);
                if (!idat) die("realloc");
                idcap = nc;
            }
            memcpy(idat + idn, body, len);
            idn += len;
        } else if (typ == 0x49454e44u) {                /* IEND */
            break;
        }
        off += 12u + len;
    }
    if (!idat || w <= 0 || h <= 0) {
        goto out;
    }
    im->bpp = (ct == 6) ? 4 : 3;
    rawcap = (size_t)h * (1u + (size_t)w * (size_t)im->bpp);
    raw = xmalloc(rawcap);
    memset(&z, 0, sizeof(z));
    z.next_in = idat;
    z.avail_in = (uInt)idn;
    z.next_out = raw;
    z.avail_out = (uInt)rawcap;
    if (inflateInit(&z) != Z_OK) {
        goto out;
    }
    zr = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (zr != Z_STREAM_END || z.total_out != rawcap) {
        goto out;
    }
    im->w = w;
    im->h = h;
    im->rgba = xmalloc((size_t)w * (size_t)h * 4u);
    {
        size_t stride = (size_t)w * (size_t)im->bpp;
        int y;
        for (y = 0; y < h; y++) {
            uint8_t *src = raw + (size_t)y * (stride + 1u);
            uint8_t *row = src + 1;
            int f = src[0];
            size_t i;
            if (f == 1) {                               /* Sub */
                for (i = (size_t)im->bpp; i < stride; i++) {
                    row[i] = (uint8_t)(row[i] + row[i - im->bpp]);
                }
            } else if (f == 2) {                        /* Up */
                if (y > 0) {
                    uint8_t *up = raw + (size_t)(y - 1) * (stride + 1u) + 1u;
                    for (i = 0; i < stride; i++) row[i] = (uint8_t)(row[i] + up[i]);
                }
            } else if (f == 3) {                        /* Average */
                uint8_t *up = (y > 0) ? raw + (size_t)(y - 1) * (stride + 1u) + 1u : NULL;
                for (i = 0; i < stride; i++) {
                    unsigned a = (i >= (size_t)im->bpp) ? row[i - im->bpp] : 0u;
                    unsigned b = up ? up[i] : 0u;
                    row[i] = (uint8_t)(row[i] + (uint8_t)((a + b) / 2u));
                }
            } else if (f == 4) {                        /* Paeth */
                uint8_t *up = (y > 0) ? raw + (size_t)(y - 1) * (stride + 1u) + 1u : NULL;
                for (i = 0; i < stride; i++) {
                    int a = (i >= (size_t)im->bpp) ? row[i - im->bpp] : 0;
                    int b = up ? up[i] : 0;
                    int c = (up && i >= (size_t)im->bpp) ? up[i - im->bpp] : 0;
                    int p = a + b - c;
                    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    row[i] = (uint8_t)(row[i] + (uint8_t)pr);
                }
            } else if (f != 0) {
                goto out;
            }
        }
        for (y = 0; y < h; y++) {
            uint8_t *row = raw + (size_t)y * (stride + 1u) + 1u;
            int x;
            for (x = 0; x < w; x++) {
                uint8_t *p = row + (size_t)x * im->bpp;
                uint8_t *o = im->rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o[3] = (im->bpp == 4) ? p[3] : 255;
            }
        }
    }
    ret = 0;
out:
    free(idat);
    free(raw);
    return ret;
}

static void png_free(Png *im)
{
    free(im->rgba);
    im->rgba = NULL;
}

/* ---- CDP 传输 ---- */

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int cdp_send(const char *json)
{
    if (write_all(g_cmd, json, strlen(json)) != 0 ||
        write_all(g_cmd, "", 1) != 0) {
        return -1;
    }
    return 0;
}

static int cdp_send_cmd(int id, const char *method, const char *params,
                        const char *session)
{
    char *buf = xmalloc(strlen(method) + (params ? strlen(params) : 0) +
                        (session ? strlen(session) : 0) + 256u);
    int n;
    int rc;

    if (session) {
        n = sprintf(buf, "{\"id\":%d,\"method\":\"%s\",\"params\":%s,"
                         "\"sessionId\":\"%s\"}",
                    id, method, params ? params : "{}", session);
    } else {
        n = sprintf(buf, "{\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                    id, method, params ? params : "{}");
    }
    (void)n;
    if (getenv("C6CDP_DEBUG")) {
        dprintf(2, "CDP> %.240s\n", buf);
    }
    rc = cdp_send(buf);
    free(buf);
    return rc;
}

static char g_rx[MSG_MAX];
static size_t g_rx_len;

/* 一次 read 里可能同时到达应答和多个事件；必须保留 NUL 之后的残余，
 * 否则后面的 screencastFrame 会被整段丢掉（静态页面不会再发第二帧）。 */
static int cdp_recv(char *buf, size_t cap)
{
    for (;;) {
        char *z = memchr(g_rx, 0, g_rx_len);
        if (z) {
            size_t n = (size_t)(z - g_rx);
            if (n + 1u > cap) {
                return -1;
            }
            memcpy(buf, g_rx, n);
            buf[n] = 0;
            if (getenv("C6CDP_DEBUG")) {
                dprintf(2, "CDP< %.240s\n", buf);
            }
            {
                size_t rest = g_rx_len - n - 1u;
                memmove(g_rx, g_rx + n + 1u, rest);
                g_rx_len = rest;
            }
            return 0;
        }
        if (g_rx_len + 1u >= sizeof(g_rx)) {
            return -1;
        }
        {
            ssize_t r = read(g_resp, g_rx + g_rx_len,
                             sizeof(g_rx) - 1u - g_rx_len);
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (r == 0) {
                return -1;
            }
            g_rx_len += (size_t)r;
        }
    }
}

static int wait_result(int id, char *out, size_t cap)
{
    char *buf = xmalloc(MSG_MAX);
    int rc = -1;

    while (cdp_recv(buf, MSG_MAX) == 0) {
        long rid = -1;
        if (json_int(buf, "id", &rid) == 0 && rid == id) {
            if (json_key(buf, "error")) {
                dprintf(2, "cdp error: %s\n", buf);
                goto out;
            }
            snprintf(out, cap, "%s", buf);
            rc = 0;
            goto out;
        }
    }
out:
    free(buf);
    return rc;
}

static int parse_target(const char *r, char *id, size_t cap)
{
    return json_str(r, "targetId", id, cap);
}

static int parse_session(const char *r, char *id, size_t cap)
{
    return json_str(r, "sessionId", id, cap);
}

/* ---- 帧 → fb ---- */

typedef struct {
    int fd;
    uint8_t *mem;
    size_t len;
    int w, h;
    char out[256];
} Fb;

static int fb_open(Fb *fb, const char *path, int w, int h)
{
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;

    memset(fb, 0, sizeof(*fb));
    fb->w = w;
    fb->h = h;
    fb->fd = -1;
    if (path && *path) {
        fb->fd = open(path, O_RDWR);
        if (fb->fd < 0) {
            return -1;
        }
        if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vi) != 0 ||
            ioctl(fb->fd, FBIOGET_FSCREENINFO, &fi) != 0) {
            close(fb->fd);
            fb->fd = -1;
            return -1;
        }
        fb->w = (int)vi.xres;
        fb->h = (int)vi.yres;
        fb->len = (size_t)fi.line_length * (size_t)fb->h;
        fb->mem = mmap(NULL, fb->len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fb->fd, 0);
        if (fb->mem == MAP_FAILED) {
            fb->mem = NULL;
            close(fb->fd);
            fb->fd = -1;
            return -1;
        }
    }
    return 0;
}

static void fb_close(Fb *fb)
{
    if (fb->mem) {
        munmap(fb->mem, fb->len);
    }
    if (fb->fd >= 0) {
        close(fb->fd);
    }
    fb->mem = NULL;
    fb->fd = -1;
}

static int fb_blit(Fb *fb, const Png *im)
{
    int x, y;

    if (fb->mem) {
        uint32_t *dst = (uint32_t *)fb->mem;
        size_t stride = fb->len / (size_t)fb->h / 4u;
        for (y = 0; y < fb->h; y++) {
            int sy = (int)((long)y * im->h / fb->h);
            for (x = 0; x < fb->w; x++) {
                int sx = (int)((long)x * im->w / fb->w);
                const uint8_t *p = im->rgba + ((size_t)sy * (size_t)im->w +
                                    (size_t)sx) * 4u;
                dst[(size_t)y * stride + (size_t)x] =
                    0x00u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            }
        }
        {
            struct c6_fb_rect r = { 0u, 0u, (uint32_t)fb->w, (uint32_t)fb->h };
            (void)ioctl(fb->fd, C6FBIOFLUSH, &r);
        }
        return 0;
    }
    /* 宿主测试：写 PPM，方便 check 脚本验像素。 */
    if (fb->out[0]) {
        FILE *f = fopen(fb->out, "wb");
        if (!f) return -1;
        fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
        for (y = 0; y < im->h; y++) {
            for (x = 0; x < im->w; x++) {
                const uint8_t *p = im->rgba + ((size_t)y * (size_t)im->w + (size_t)x) * 4u;
                fwrite(p, 1, 3, f);
            }
        }
        fclose(f);
    }
    return 0;
}

/* ---- 输入 ---- */

typedef struct {
    int fd;
    int x, y;
    int left;
} Input;

static uint32_t key_vk(uint16_t code, const char **key, const char **dom)
{
    *key = "";
    *dom = "";
    switch (code) {
    case KEY_ENTER: *key = "Enter"; *dom = "Enter"; return 13;
    case KEY_ESC: *key = "Escape"; *dom = "Escape"; return 27;
    case KEY_BACKSPACE: *key = "Backspace"; *dom = "Backspace"; return 8;
    case KEY_TAB: *key = "Tab"; *dom = "Tab"; return 9;
    case KEY_SPACE: *key = " "; *dom = "Space"; return 32;
    case KEY_UP: *key = "ArrowUp"; *dom = "ArrowUp"; return 38;
    case KEY_DOWN: *key = "ArrowDown"; *dom = "ArrowDown"; return 40;
    case KEY_LEFT: *key = "ArrowLeft"; *dom = "ArrowLeft"; return 37;
    case KEY_RIGHT: *key = "ArrowRight"; *dom = "ArrowRight"; return 39;
    default: return 0;
    }
}

static int input_event(Input *in, const struct input_event *ev)
{
    char params[512];
    int id = g_next_id++;
    int up = ev->value == 0;
    const char *key, *dom;

    if (ev->type == EV_REL) {
        if (ev->code == REL_X) in->x += ev->value;
        if (ev->code == REL_Y) in->y += ev->value;
        snprintf(params, sizeof(params),
                 "{\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d,\"button\":\"none\"}",
                 in->x, in->y);
        return cdp_send_cmd(id, "Input.dispatchMouseEvent", params, NULL);
    }
    if (ev->type == EV_KEY && (ev->code == BTN_LEFT || ev->code == BTN_RIGHT)) {
        if (ev->code == BTN_LEFT) in->left = ev->value;
        snprintf(params, sizeof(params),
                 "{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\",\"clickCount\":1}",
                 up ? "mouseReleased" : "mousePressed", in->x, in->y,
                 ev->code == BTN_RIGHT ? "right" : "left");
        return cdp_send_cmd(id, "Input.dispatchMouseEvent", params, NULL);
    }
    if (ev->type == EV_KEY) {
        uint32_t vk = key_vk(ev->code, &key, &dom);
        if (vk == 0) {
            return 0;
        }
        snprintf(params, sizeof(params),
                 "{\"type\":\"%s\",\"key\":\"%s\",\"code\":\"%s\","
                 "\"windowsVirtualKeyCode\":%u,\"nativeVirtualKeyCode\":%u}",
                 up ? "keyUp" : "keyDown", key, dom, vk, vk);
        return cdp_send_cmd(id, "Input.dispatchKeyEvent", params, NULL);
    }
    return 0;
}

/* ---- 主循环 ---- */

static pid_t spawn_chrome(const char *chrome, const char *url,
                          const char *size, int cmd[2], int resp[2])
{
    pid_t p;

    if (pipe(cmd) != 0 || pipe(resp) != 0) {
        return -1;
    }
    p = fork();
    if (p < 0) {
        return -1;
    }
    if (p == 0) {
        /* pipe() 拿到的 fd 很可能就是 3/4；直接 dup2(cmd[0],3) 后在
         * close(cmd[0]) 里把刚装好的 fd3 关掉，Chrome 会报
         * "Remote debugging pipe file descriptors are not open"。
         * 先把两端复制到高位，再装到 3/4。 */
        int r = fcntl(cmd[0], F_DUPFD, 10);
        int w = fcntl(resp[1], F_DUPFD, 10);
        if (r < 0 || w < 0) {
            _exit(126);
        }
        /* 先关原始 pipe fd 再装 3/4：原始 fd 很可能就占着 4，先装后关
         * 会把刚 dup 到 4 的写端又关掉。 */
        close(cmd[0]); close(cmd[1]); close(resp[0]); close(resp[1]);
        dup2(r, 3);
        dup2(w, 4);
        close(r); close(w);
        execl(chrome, chrome, "--headless=new", "--no-sandbox",
              "--disable-gpu", "--hide-scrollbars",
              "--remote-debugging-pipe", "--remote-allow-origins=*",
              "--disable-crash-reporter", "--disable-breakpad",
              "--user-data-dir=/tmp/c6chrome-profile",
              "--window-size=1280,800", url, (char *)NULL);
        _exit(127);
    }
    close(cmd[0]);
    close(resp[1]);
    return p;
}

static int setup_page(char *session, size_t scap, const char *url)
{
    char r[MSG_MAX];
    char tid[128];
    int id = g_next_id++;

    /* headless=new 只允许一个 target（CLI URL 已经建好），不能 createTarget；
     * 先列出目标，再 attach。 */
    if (cdp_send_cmd(id, "Target.getTargets", "{}", NULL) != 0 ||
        wait_result(id, r, sizeof(r)) != 0 || parse_target(r, tid, sizeof(tid)) != 0) {
        return -1;
    }
    {
        char params[256];
        id = g_next_id++;
        snprintf(params, sizeof(params), "{\"targetId\":\"%s\"}", tid);
        if (cdp_send_cmd(id, "Target.activateTarget", params, NULL) != 0 ||
            wait_result(id, r, sizeof(r)) != 0) {
            return -1;
        }
        snprintf(params, sizeof(params), "{\"targetId\":\"%s\",\"flatten\":true}", tid);
        id = g_next_id++;
        if (cdp_send_cmd(id, "Target.attachToTarget", params, NULL) != 0 ||
            wait_result(id, r, sizeof(r)) != 0 ||
            parse_session(r, session, scap) != 0) {
            return -1;
        }
    }
    {
        char params[1024];
        int id1 = g_next_id++, id2 = g_next_id++;
        if (cdp_send_cmd(id1, "Page.enable", "{}", session) != 0 ||
            wait_result(id1, r, sizeof(r)) != 0) {
            return -1;
        }
        {
            char esc[800];
            json_escape(url, esc, sizeof(esc));
            snprintf(params, sizeof(params), "{\"url\":\"%s\"}", esc);
        }
        if (cdp_send_cmd(id2, "Page.navigate", params, session) != 0 ||
            wait_result(id2, r, sizeof(r)) != 0) {
            return -1;
        }
    }
    {
        int try;
        /* 导航刚结束时 renderer 还在换页；startScreencast 早发会得到
         * "Not attached to an active page"，等半秒并重试。 */
        usleep(500000);
        for (try = 0; try < 8; try++) {
            int id3 = g_next_id++;
            if (cdp_send_cmd(id3, "Page.startScreencast",
                             "{\"format\":\"png\",\"quality\":80,\"maxWidth\":1280,"
                             "\"maxHeight\":800,\"everyNthFrame\":1}", session) != 0 ||
                wait_result(id3, r, sizeof(r)) == 0) {
                break;
            }
            usleep(250000);
        }
        if (try == 8) {
            return -1;
        }
    }
    return 0;
}

#ifndef C6CDP_NO_MAIN
int main(int argc, char **argv)
{
    const char *chrome = "/usr/bin/chromium";
    const char *url = "about:blank";
    const char *fbpath = "/dev/fb0";
    const char *input = "/dev/input/event0";
    const char *output = NULL;
    int width = 1280, height = 800;
    int self = 0;
    int max_frames = 0;
    int i;
    int cmd[2], resp[2];
    pid_t child;
    char session[128] = "";
    Fb fb;
    Input in;
    char *msg = xmalloc(MSG_MAX);
    int frame_count = 0;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--self-test", 12) == 0) self = 1;
        else if (strncmp(argv[i], "--chrome=", 9) == 0) chrome = argv[i] + 9;
        else if (strncmp(argv[i], "--url=", 6) == 0) url = argv[i] + 6;
        else if (strncmp(argv[i], "--fb=", 5) == 0) fbpath = argv[i] + 5;
        else if (strncmp(argv[i], "--input=", 8) == 0) input = argv[i] + 8;
        else if (strncmp(argv[i], "--output=", 9) == 0) { output = argv[i] + 9; fbpath = ""; }
        else if (strncmp(argv[i], "--width=", 8) == 0) width = atoi(argv[i] + 8);
        else if (strncmp(argv[i], "--height=", 9) == 0) height = atoi(argv[i] + 9);
        else if (strncmp(argv[i], "--frames=", 9) == 0) max_frames = atoi(argv[i] + 9);
    }
    if (self) {
        uint8_t out[64];
        const char *b = "aGVsbG8=";
        if (b64_decode(b, out, sizeof(out)) != 5 || memcmp(out, "hello", 5) != 0) {
            dprintf(2, "FAIL: self-test base64\n");
            return 1;
        }
        dprintf(1, "PASS: c6chrome-display self-test (base64)\n");
        return 0;
    }
    memset(&fb, 0, sizeof(fb));
    if (fb_open(&fb, fbpath, width, height) != 0) {
        /* fb 打开失败时仍允许 --output 运行 */
        if (!output) {
            die("open fb");
        }
    }
    if (output) {
        snprintf(fb.out, sizeof(fb.out), "%s", output);
    }
    child = spawn_chrome(chrome, url, output ? "1280,800" : "1280,800", cmd, resp);
    if (child < 0) {
        die("fork chrome");
    }
    g_cmd = cmd[1];
    g_resp = resp[0];
    if (setup_page(session, sizeof(session), url) != 0) {
        dprintf(2, "c6chrome-display: CDP setup failed\n");
        kill(child, SIGTERM);
        return 2;
    }
    in.fd = open(input, O_RDONLY | O_NONBLOCK);
    in.x = width / 2;
    in.y = height / 2;

    for (;;) {
        struct pollfd pfd[2];
        int n = 1;
        pfd[0].fd = g_resp;
        pfd[0].events = POLLIN;
        if (in.fd >= 0) {
            pfd[n].fd = in.fd;
            pfd[n].events = POLLIN;
            n++;
        }
        if (poll(pfd, (nfds_t)n, 1000) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[0].revents & POLLIN) {
            char *data;
            size_t dn;
            char *b64;
            size_t bn;
            Png im;
            int ackid;
            if (cdp_recv(msg, MSG_MAX) != 0) {
                break;
            }
            if (json_str(msg, "method", data = xmalloc(256), 256) == 0 &&
                strcmp(data, "Page.screencastFrame") == 0) {
                free(data);
                /* data 字段可能很长，直接定位后按字符串拷贝。 */
                const char *p = json_key(msg, "data");
                if (p && *p == '"') {
                    p++;
                    b64 = (char *)p;
                    bn = strcspn(b64, "\"");
                    data = xmalloc(bn + 1u);
                    memcpy(data, b64, bn);
                    data[bn] = 0;
                    dn = b64_decode(data, (uint8_t *)data, bn);
                    free(data);
                    memset(&im, 0, sizeof(im));
                    if (png_decode((const uint8_t *)data, dn, &im) == 0) {
                        if (frame_count == 0) {
                            dprintf(1, "frame: %dx%d -> fb %dx%d\n",
                                    im.w, im.h, fb.w, fb.h);
                        }
                        fb_blit(&fb, &im);
                        png_free(&im);
                        frame_count++;
                    }
                }
                if (json_int(msg, "id", (long *)&ackid) == 0) { /* no-op */ }
                {
                    char ack[256];
                    long scid = 0;
                    if (json_int(msg, "sessionId", &scid) == 0) {
                        snprintf(ack, sizeof(ack),
                                 "{\"id\":%d,\"method\":\"Page.screencastFrameAck\","
                                 "\"params\":{\"sessionId\":%ld},\"sessionId\":\"%s\"}",
                                 g_next_id++, scid, session);
                        cdp_send(ack);
                    }
                }
            } else {
                free(data);
            }
        }
        if (in.fd >= 0 && (pfd[1].revents & POLLIN)) {
            struct input_event ev[32];
            ssize_t r = read(in.fd, ev, sizeof(ev));
            ssize_t k;
            for (k = 0; k + (ssize_t)sizeof(ev[0]) <= r; k += (ssize_t)sizeof(ev[0])) {
                input_event(&in, &ev[k / (ssize_t)sizeof(ev[0])]);
            }
        }
        if ((output && frame_count >= 1) ||
            (max_frames > 0 && frame_count >= max_frames)) {
            break;
        }
    }
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    fb_close(&fb);
    close(g_cmd);
    close(g_resp);
    free(msg);
    return frame_count > 0 ? 0 : 1;
}
#endif /* C6CDP_NO_MAIN */
