/* L3 验收：一个普通的 Linux 用户态程序，用 opendir/readdir/open/read
 * 走一圈文件语义（内核侧零改造）。它是静态链接的，由宿主 gcc 编译。 */

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
    DIR *d;
    struct dirent *e;
    int fd;
    char buf[512];
    ssize_t n;

    out("[ls] 根目录:\n");
    d = opendir("/");
    if (d == 0) {
        out("[ls] FAIL opendir\n");
        return 1;
    }
    while ((e = readdir(d)) != 0) {
        out("  ");
        out(e->d_name);
        out("\n");
    }
    closedir(d);

    out("[ls] /etc/version:\n");
    fd = open("/etc/version", O_RDONLY);
    if (fd < 0) {
        out("[ls] FAIL open\n");
        return 1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        out("[ls] FAIL read\n");
        return 1;
    }
    buf[n] = 0;
    out(buf);
    close(fd);

    out("[ls] /hello.txt:\n");
    fd = open("/hello.txt", O_RDONLY);
    if (fd < 0) {
        out("[ls] FAIL open hello\n");
        return 1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        out("[ls] FAIL read hello\n");
        return 1;
    }
    buf[n] = 0;
    out(buf);
    close(fd);

    out("[ls] 完成\n");
    return 0;
}
