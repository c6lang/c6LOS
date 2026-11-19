/* M50 验收：pty per-instance termios / winsize
 *  1) tcsetattr 后 tcgetattr 读回（此前 TCSETS 直接丢弃）
 *  2) 两个 pty 实例的 termios 互相独立
 *  3) TIOCSWINSZ 设置后 TIOCGWINSZ 读回
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

static int open_pty(unsigned int *n)
{
    int mfd = open("/dev/ptmx", O_RDWR);
    char path[64];
    int sfd;

    if (mfd < 0) { return -1; }
    if (ioctl(mfd, TIOCGPTN, n) != 0) { close(mfd); return -1; }
    snprintf(path, sizeof(path), "/dev/pts/%u", *n);
    sfd = open(path, O_RDWR);
    if (sfd < 0) { close(mfd); return -1; }
    close(sfd);                          /* 只需 master 做 ioctl */
    return mfd;
}

int main(void)
{
    unsigned int n1 = 0, n2 = 0;
    int m1, m2;
    char t1[64], t2[64];
    struct winsize ws;

    m1 = open_pty(&n1);
    m2 = open_pty(&n2);
    CHECK(m1 >= 0 && m2 >= 0);
    CHECK(n1 != n2);
    if (m1 < 0 || m2 < 0) { goto done; }

    /* 默认 termios 非空 */
    memset(t1, 0, sizeof(t1));
    CHECK(ioctl(m1, 0x5401, t1) == 0);   /* TCGETS */
    CHECK(*(unsigned int *)t1 != 0u);

    /* 改 m1 的本地标志（lflag 在偏移 12），m2 默认保持 */
    memset(t2, 0, sizeof(t2));
    CHECK(ioctl(m2, 0x5401, t2) == 0);
    CHECK(*(unsigned int *)(t1 + 12) == *(unsigned int *)(t2 + 12));

    *(unsigned int *)(t1 + 12) = 0x8A3Bu & ~(1u << 3);   /* 关掉 ECHO */
    CHECK(ioctl(m1, 0x5402, t1) == 0);   /* TCSETS */
    memset(t2, 0, sizeof(t2));
    CHECK(ioctl(m1, 0x5401, t2) == 0);
    CHECK(*(unsigned int *)(t2 + 12) == *(unsigned int *)(t1 + 12));  /* 读回一致 */
    memset(t2, 0, sizeof(t2));
    CHECK(ioctl(m2, 0x5401, t2) == 0);
    CHECK(*(unsigned int *)(t2 + 12) == 0x8A3Bu);            /* m2 不受影响 */

    /* winsize per-instance */
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 40; ws.ws_col = 132;
    CHECK(ioctl(m1, 0x5414, &ws) == 0);  /* TIOCSWINSZ */
    memset(&ws, 0, sizeof(ws));
    CHECK(ioctl(m1, 0x5413, &ws) == 0);  /* TIOCGWINSZ */
    CHECK(ws.ws_row == 40 && ws.ws_col == 132);
    memset(&ws, 0, sizeof(ws));
    CHECK(ioctl(m2, 0x5413, &ws) == 0);
    CHECK(ws.ws_row == 24 && ws.ws_col == 80);      /* m2 默认 */

done:
    if (m1 >= 0) { close(m1); }
    if (m2 >= 0) { close(m2); }
    if (failures == 0) {
        dprintf(1, "PASS: m50ptytermios (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m50ptytermios (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
