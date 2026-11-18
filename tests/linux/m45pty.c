/* M45 验收：pty（/dev/ptmx + /dev/pts/N）
 * 验：
 *   1) open /dev/ptmx 成功，TIOCGPTN 拿到从设备编号
 *   2) 从 fd 是 tty（isatty）：TCGETS/TIOCGWINSZ 成功
 *   3) master 写 → slave 读：内容正确
 *   4) slave 写 → master 读：内容正确
 *   5) 第二个 ptmx 拿到不同编号（多实例）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef TIOCGPTN
#define TIOCGPTN 0x80045430u
#endif

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    int mfd, sfd, mfd2;
    unsigned int n = 0, n2 = 0;
    char path[64];
    char buf[32];
    ssize_t nr;

    mfd = open("/dev/ptmx", O_RDWR);
    CHECK(mfd >= 0);
    if (mfd < 0) { goto done; }
    CHECK(ioctl(mfd, TIOCGPTN, &n) == 0);
    dprintf(1, "M45: pty number=%u\n", n);
    snprintf(path, sizeof(path), "/dev/pts/%u", n);
    sfd = open(path, O_RDWR);
    CHECK(sfd >= 0);
    if (sfd < 0) { close(mfd); goto done; }

    /* 从设备是 tty */
    {
        char termios_buf[64];
        struct winsize ws;
        CHECK(ioctl(sfd, 0x5401, termios_buf) == 0);   /* TCGETS */
        CHECK(ioctl(sfd, 0x5413, &ws) == 0);           /* TIOCGWINSZ */
    }

    /* master → slave */
    CHECK(write(mfd, "hello-slave", 11) == 11);
    memset(buf, 0, sizeof(buf));
    nr = read(sfd, buf, sizeof(buf));
    CHECK(nr == 11);
    CHECK(nr == 11 && memcmp(buf, "hello-slave", 11) == 0);

    /* slave → master */
    CHECK(write(sfd, "hello-master", 12) == 12);
    memset(buf, 0, sizeof(buf));
    nr = read(mfd, buf, sizeof(buf));
    CHECK(nr == 12);
    CHECK(nr == 12 && memcmp(buf, "hello-master", 12) == 0);

    /* 多实例：第二个 ptmx 编号不同 */
    mfd2 = open("/dev/ptmx", O_RDWR);
    CHECK(mfd2 >= 0);
    if (mfd2 >= 0) {
        CHECK(ioctl(mfd2, TIOCGPTN, &n2) == 0);
        CHECK(n2 != n);
        close(mfd2);
    }
    close(sfd);
    close(mfd);

done:
    if (failures == 0) {
        dprintf(1, "PASS: m45pty (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m45pty (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
