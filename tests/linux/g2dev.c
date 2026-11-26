/* G2（TDD 先红）：设备层最小闭环。
 *
 *   1) /dev/fb0：fb_var/fix_screeninfo 逐字节 ABI、1280x800 XRGB8888、
 *      FBIOGET/PUT_VSCREENINFO、FBIOBLANK、C6FBIOFLUSH；
 *   2) mmap 把扫描缓冲**同一批物理页**映射给用户，写像素后从第二个映射
 *      能读到同样的值；
 *   3) /dev/input/event0：EVIOCGNAME/GID/VERSION/BIT/ABS、C6EVIOCINJECT
 *      注入 24 字节 input_event，poll + read 取回。
 *
 * 全部用标准 Linux 系统调用，内核侧不认测试专用路径。 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

#ifndef C6FBIOFLUSH
struct c6_fb_rect {
    __u32 x, y, w, h;
};
#define C6FBIOFLUSH _IOW('F', 0x10, struct c6_fb_rect)
#endif

/* 私有测试注入（生产内核由 C6OS_TEST_HOOKS 关闭；本任务要求可用）。 */
#ifndef C6EVIOCINJECT
#define C6EVIOCINJECT _IOW('E', 0xf0, struct input_event)
#endif

#define FB_W 1280u
#define FB_H 800u
#define FB_BPP 32u
#define FB_LINE (FB_W * 4u)
#define FB_SIZE (FB_LINE * FB_H)

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; \
    dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); \
} } while (0)

int main(void)
{
    int fd;
    int fd2;
    int ev;
    struct fb_var_screeninfo vi;
    struct fb_var_screeninfo nv;
    struct fb_fix_screeninfo fi;
    unsigned char *fb;
    unsigned char *fb2;
    unsigned int *p;
    unsigned int *q;
    struct c6_fb_rect rect;
    int blank;
    char name[64];
    int ver;
    struct input_id iid;
    unsigned long evbits[1];
    struct input_absinfo ai;
    struct pollfd pfd;
    struct input_event ie;
    struct input_event got[2];
    ssize_t r;
    int fl;

    /* ABI 布局先自检：内核必须和宿主 Linux 头逐字节一致。 */
    _Static_assert(sizeof(struct fb_var_screeninfo) == 160,
                   "fb_var_screeninfo must be 160 B");
    _Static_assert(sizeof(struct fb_fix_screeninfo) == 80,
                   "fb_fix_screeninfo must be 80 B");
    _Static_assert(sizeof(struct input_event) == 24,
                   "input_event must be 24 B");

    /* ---- G2-1 /dev/fb0 ---- */
    fd = open("/dev/fb0", O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        dprintf(1, "FAIL: g2dev: /dev/fb0 open errno=%d\n", errno);
        return 1;
    }

    memset(&vi, 0, sizeof(vi));
    CHECK(ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0);
    CHECK(vi.xres == FB_W);
    CHECK(vi.yres == FB_H);
    CHECK(vi.bits_per_pixel == FB_BPP);
    CHECK(vi.grayscale == 0);
    CHECK(vi.red.offset == 16 && vi.red.length == 8);
    CHECK(vi.green.offset == 8 && vi.green.length == 8);
    CHECK(vi.blue.offset == 0 && vi.blue.length == 8);
    CHECK(vi.transp.offset == 24 && vi.transp.length == 8);

    memset(&fi, 0, sizeof(fi));
    CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0);
    CHECK(fi.smem_start != 0);
    CHECK(fi.smem_len == FB_SIZE);
    CHECK(fi.line_length == FB_LINE);
    CHECK(fi.type == FB_TYPE_PACKED_PIXELS);
    CHECK(fi.visual == FB_VISUAL_TRUECOLOR);
    CHECK(fi.accel == FB_ACCEL_NONE);

    /* 只接受同分辨率/同 bpp；改分辨率必须 EINVAL。 */
    nv = vi;
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &nv) == 0);
    nv.xres = 1024;
    errno = 0;
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &nv) < 0 && errno == EINVAL);

    blank = FB_BLANK_UNBLANK;
    CHECK(ioctl(fd, FBIOBLANK, blank) == 0);
    blank = FB_BLANK_POWERDOWN;
    CHECK(ioctl(fd, FBIOBLANK, blank) == 0);

    fb = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(fb != MAP_FAILED);
    if (fb == MAP_FAILED) {
        dprintf(1, "FAIL: g2dev: fb mmap errno=%d\n", errno);
        close(fd);
        return 1;
    }

    /* 写像素。映射里 XRGB8888 的字节序就是宿主小端，直接按 u32 存取。
     * 先铺四色测试图（验收截图能看见东西），再落三个断言像素。 */
    p = (unsigned int *)fb;
    {
        unsigned int y, x;
        for (y = 0; y < FB_H; y++) {
            for (x = 0; x < FB_W; x++) {
                unsigned int v;
                if (y < FB_H / 4u) {
                    v = 0x00e04040u;
                } else if (y < FB_H / 2u) {
                    v = 0x0040e040u;
                } else if (y < (FB_H * 3u) / 4u) {
                    v = 0x004040e0u;
                } else {
                    v = 0x00e0e040u;
                }
                p[y * FB_W + x] = v;
            }
        }
    }
    p[0] = 0x00112233u;
    p[FB_W * (FB_H / 2u) + FB_W / 2u] = 0x00445566u;
    p[FB_W * (FB_H - 1u) + (FB_W - 1u)] = 0x00778899u;
    CHECK(p[0] == 0x00112233u);
    CHECK(p[FB_W * (FB_H / 2u) + FB_W / 2u] == 0x00445566u);
    CHECK(p[FB_W * (FB_H - 1u) + (FB_W - 1u)] == 0x00778899u);
    /* 最后 4 字节正好是最后一个像素：确认整块 smem_len 都在映射内。 */
    CHECK(((unsigned int *)fb)[(FB_SIZE / 4u) - 1u] == 0x00778899u);

    rect.x = 0;
    rect.y = 0;
    rect.w = FB_W;
    rect.h = FB_H;
    CHECK(ioctl(fd, C6FBIOFLUSH, &rect) == 0);
    rect.x = 10;
    rect.y = 20;
    rect.w = 100;
    rect.h = 50;
    CHECK(ioctl(fd, C6FBIOFLUSH, &rect) == 0);

    /* 第二个 fd + 第二个映射必须是**同一批物理页**。 */
    fd2 = open("/dev/fb0", O_RDWR);
    CHECK(fd2 >= 0);
    if (fd2 >= 0) {
        fb2 = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd2, 0);
        CHECK(fb2 != MAP_FAILED);
        if (fb2 != MAP_FAILED) {
            q = (unsigned int *)fb2;
            CHECK(q[0] == 0x00112233u);
            CHECK(q[FB_W * (FB_H / 2u) + FB_W / 2u] == 0x00445566u);
            CHECK(q[FB_W * (FB_H - 1u) + (FB_W - 1u)] == 0x00778899u);
            q[0] = 0x00aabbccu;
            CHECK(p[0] == 0x00aabbccu);

            /* 超出扫描缓冲的映射长度必须拒绝。 */
            errno = 0;
            CHECK(mmap(NULL, fi.smem_len + 4096u, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd2, 0) == MAP_FAILED && errno == EINVAL);

            /* 非 0 偏移不支持。 */
            errno = 0;
            CHECK(mmap(NULL, 4096u, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd2, 4096) == MAP_FAILED && errno != 0);

            CHECK(munmap(fb2, fi.smem_len) == 0);
        }
        close(fd2);
    }
    CHECK(munmap(fb, fi.smem_len) == 0);
    close(fd);

    /* 关闭所有映射后页面不能把驱动持有的扫描缓冲释放掉：重开再映射，
     * 之前写入的像素必须还在（证明 unmap 只减用户引用，不还帧）。 */
    fd = open("/dev/fb0", O_RDWR);
    CHECK(fd >= 0);
    if (fd >= 0) {
        fb = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK(fb != MAP_FAILED);
        if (fb != MAP_FAILED) {
            p = (unsigned int *)fb;
            CHECK(p[0] == 0x00aabbccu);
            CHECK(munmap(fb, fi.smem_len) == 0);
        }
        close(fd);
    }

    /* ---- G2-2 /dev/input/event0 ---- */
    ev = open("/dev/input/event0", O_RDWR);
    CHECK(ev >= 0);
    if (ev < 0) {
        dprintf(1, "FAIL: g2dev: /dev/input/event0 open errno=%d\n", errno);
    } else {
        memset(name, 0, sizeof(name));
        r = ioctl(ev, EVIOCGNAME(sizeof(name)), name);
        CHECK(r > 0);
        CHECK(strstr(name, "c6os") != NULL);

        ver = 0;
        CHECK(ioctl(ev, EVIOCGVERSION, &ver) == 0);
        CHECK(ver == EV_VERSION);

        memset(&iid, 0, sizeof(iid));
        CHECK(ioctl(ev, EVIOCGID, &iid) == 0);
        CHECK(iid.vendor == 0x0001 && iid.product == 0xc6f0);

        memset(evbits, 0, sizeof(evbits));
        CHECK(ioctl(ev, EVIOCGBIT(0, sizeof(evbits)), evbits) == 0);
        CHECK((evbits[0] & (1ul << EV_SYN)) != 0ul);
        CHECK((evbits[0] & (1ul << EV_KEY)) != 0ul);

        memset(&ai, 0, sizeof(ai));
        CHECK(ioctl(ev, EVIOCGABS(ABS_X), &ai) == 0);
        CHECK(ai.maximum == (int)FB_W - 1);
        memset(&ai, 0, sizeof(ai));
        CHECK(ioctl(ev, EVIOCGABS(ABS_Y), &ai) == 0);
        CHECK(ai.maximum == (int)FB_H - 1);

        /* 空队列：poll 不谎报可读。 */
        pfd.fd = ev;
        pfd.events = POLLIN;
        pfd.revents = 0;
        CHECK(poll(&pfd, 1, 0) == 0);

        /* 注入 KEY_A 按下。 */
        memset(&ie, 0, sizeof(ie));
        ie.type = EV_KEY;
        ie.code = KEY_A;
        ie.value = 1;
        CHECK(ioctl(ev, C6EVIOCINJECT, &ie) == 0);

        pfd.revents = 0;
        CHECK(poll(&pfd, 1, 0) == 1);
        CHECK((pfd.revents & POLLIN) != 0);

        memset(got, 0, sizeof(got));
        r = read(ev, got, sizeof(got));
        CHECK(r == (ssize_t)sizeof(struct input_event));
        CHECK(got[0].type == EV_KEY);
        CHECK(got[0].code == KEY_A);
        CHECK(got[0].value == 1);

        /* 注入 EV_SYN/SYN_REPORT，read 仍能取到。 */
        memset(&ie, 0, sizeof(ie));
        ie.type = EV_SYN;
        ie.code = SYN_REPORT;
        ie.value = 0;
        CHECK(ioctl(ev, C6EVIOCINJECT, &ie) == 0);
        memset(&got, 0, sizeof(got));
        r = read(ev, got, sizeof(got));
        CHECK(r == (ssize_t)sizeof(struct input_event));
        CHECK(got[0].type == EV_SYN);
        CHECK(got[0].code == SYN_REPORT);

        /* 非阻塞 + 空队列 = EAGAIN。 */
        fl = fcntl(ev, F_GETFL, 0);
        CHECK(fl >= 0);
        CHECK(fcntl(ev, F_SETFL, fl | O_NONBLOCK) == 0);
        errno = 0;
        CHECK(read(ev, got, sizeof(got)) < 0 && errno == EAGAIN);

        close(ev);

        /* 关闭并重开后是新的设备对象，事件队列不应串到别的 fd 上。 */
        ev = open("/dev/input/event0", O_RDONLY);
        CHECK(ev >= 0);
        if (ev >= 0) {
            pfd.fd = ev;
            pfd.events = POLLIN;
            pfd.revents = 0;
            CHECK(poll(&pfd, 1, 0) == 0);
            close(ev);
        }
    }

    if (failures == 0) {
        dprintf(1, "PASS: g2dev (%d/%d checks)\n", total, total);
        return 0;
    }
    dprintf(1, "FAIL: g2dev (%d/%d checks)\n", failures, total);
    return 1;
}