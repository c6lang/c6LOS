/* M9-4 验收：把 /tmp 挂成 ramfs，用户态用普通 Linux 系统调用走一圈
 * create/write/lseek/read/getdents64 —— 内核侧只多一个"路径前缀 → VFS
 * 后端"的分发，用户程序不需要任何 c6OS 专有接口。 */

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void out(const char *s)
{
    write(1, s, strlen(s));
}

int main(void)
{
    static const char *msg = "hello tmp\n";
    char buf[64];
    int fd;
    ssize_t n;
    DIR *d;
    struct dirent *e;
    int seen = 0;

    fd = open("/tmp/a.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        out("FAIL: m9 open /tmp/a.txt\n");
        return 1;
    }
    if (write(fd, msg, 10) != 10) {
        out("FAIL: m9 write\n");
        return 1;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        out("FAIL: m9 lseek\n");
        return 1;
    }
    n = read(fd, buf, sizeof(buf));
    if (n != 10 || memcmp(buf, msg, 10) != 0) {
        out("FAIL: m9 read back\n");
        return 1;
    }
    if (lseek(fd, 0, SEEK_END) != 10) {
        out("FAIL: m9 lseek end\n");
        return 1;
    }
    close(fd);

    d = opendir("/tmp");
    if (d == 0) {
        out("FAIL: m9 opendir /tmp\n");
        return 1;
    }
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, "a.txt") == 0) {
            seen++;
        }
    }
    closedir(d);
    if (seen != 1) {
        out("FAIL: m9 getdents64\n");
        return 1;
    }
    out("PASS: linux-abi M9-4（/tmp = ramfs：open/write/lseek/read/getdents64）\n");
    return 0;
}
