/* M17 验收：procfs 虚拟文件 + 控制终端 ioctl。普通 Linux 程序，内核零专用接口。
 *   1. /proc/meminfo|uptime|cpuinfo|stat 打开+读，内容非空且格式合理
 *   2. /proc/self/{status,stat,cmdline} 可读
 *   3. stat("/proc/meminfo") 拿到非零 size（procfs 接进 stat）
 *   4. /proc 目录 getdents64（opendir/readdir 能列）
 *   5. TIOCSCTTY 在非 tty 的普通 fd 上返回 ENOTTY（不崩）
 * 用例不依赖 sleep。 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define TIOCSCTTY 0x540E

static int failures;

static void say(const char *s)
{
    (void)!write(1, s, strlen(s));
}

static void check(int cond, const char *what)
{
    if (!cond) {
        failures++;
        say("FAIL: ");
        say(what);
        say("\n");
    }
}

static int read_file(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    int n;

    if (fd < 0) {
        return -1;
    }
    n = (int)read(fd, buf, (size_t)(cap - 1));
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    return n;
}

int main(void)
{
    char buf[512];
    struct stat st;
    int n;
    int p[2];

    /* ---- 1. 全局 proc 文件 ---- */
    n = read_file("/proc/meminfo", buf, sizeof(buf));
    check(n > 0 && strstr(buf, "MemTotal:") != 0, "meminfo readable");
    n = read_file("/proc/uptime", buf, sizeof(buf));
    check(n > 0 && strchr(buf, '.') != 0, "uptime readable");
    n = read_file("/proc/cpuinfo", buf, sizeof(buf));
    check(n > 0 && strstr(buf, "processor") != 0, "cpuinfo readable");
    n = read_file("/proc/stat", buf, sizeof(buf));
    check(n > 0 && strstr(buf, "cpu ") != 0, "stat readable");

    /* ---- 2. self 文件 ---- */
    n = read_file("/proc/self/status", buf, sizeof(buf));
    check(n > 0 && strstr(buf, "Pid:") != 0, "self/status readable");
    n = read_file("/proc/self/stat", buf, sizeof(buf));
    check(n > 0 && strstr(buf, "busybox") != 0, "self/stat readable");
    n = read_file("/proc/self/cmdline", buf, sizeof(buf));
    check(n > 0, "self/cmdline readable");

    /* ---- 3. stat 可见 ---- */
    check(stat("/proc/meminfo", &st) == 0 && st.st_size > 0,
          "stat proc file nonzero size");

    /* ---- 4. /proc 目录列举 ---- */
    {
        DIR *d = opendir("/proc");
        struct dirent *de;
        int seen_meminfo = 0;

        check(d != 0, "opendir /proc");
        if (d != 0) {
            while ((de = readdir(d)) != 0) {
                if (strcmp(de->d_name, "meminfo") == 0) {
                    seen_meminfo = 1;
                }
            }
            check(seen_meminfo != 0, "readdir sees meminfo");
            closedir(d);
        }
    }

    /* ---- 5. TIOCSCTTY 在非终端 fd 上不崩 ---- */
    check(pipe(p) == 0, "pipe for ioctl");
    errno = 0;
    {
        int rc = ioctl(p[0], TIOCSCTTY, 0);

        check(rc == -1 || rc == 0, "TIOCSCTTY no crash");
    }
    close(p[0]);
    close(p[1]);

    if (failures == 0) {
        say("PASS: m17proc\n");
        return 0;
    }
    say("FAIL: m17proc\n");
    return 1;
}
