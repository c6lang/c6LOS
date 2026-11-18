/* M57 验收：chown 族 + symlink/link 创建 + mknod
 * 1) chown/lchown/fchown/fchownat(存在路径) 成功；不存在 ENOENT
 * 2) symlink("/hello.txt","/m57-link") 创建真实符号链接
 * 3) readlink("/m57-link") 读回目标
 * 4) open("/m57-link") 跟随链接读到 hello.txt 内容
 * 5) link("/hello.txt","/m57-hard") 创建硬链接，open 可读
 * 6) mknod 特殊文件 → EOPNOTSUPP
 * 7) symlink 重复 → EEXIST */
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

static int fails;
#define CHECK(x) do { if (!(x)) { \
    char b[128]; int n = __builtin_snprintf(b, sizeof b, \
        "FAIL: %s (line %d, errno=%d)\n", #x, __LINE__, errno); \
    (void)!write(2, b, n); fails++; } } while (0)

int main(void)
{
    char buf[64];
    ssize_t n;
    int fd;

    /* 1) chown 族 */
    errno = 0;
    CHECK(chown("/hello.txt", 0, 0) == 0);
    CHECK(lchown("/hello.txt", 0, 0) == 0);
    CHECK(fchownat(AT_FDCWD, "/hello.txt", 0, 0, 0) == 0);
    errno = 0;
    CHECK(chown("/no-such-file", 0, 0) == -1 && errno == ENOENT);
    fd = open("/hello.txt", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK(fchown(fd, 0, 0) == 0); close(fd); }

    /* 2) symlink 创建 */
    errno = 0;
    CHECK(symlink("/hello.txt", "/m57-link") == 0);

    /* 3) readlink 读回 */
    memset(buf, 0, sizeof buf);
    n = readlink("/m57-link", buf, sizeof buf);
    CHECK(n == 10 && memcmp(buf, "/hello.txt", 10) == 0);

    /* 4) open 跟随 */
    fd = open("/m57-link", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof buf - 1);
        CHECK(n > 0 && memcmp(buf, "hello from", 10) == 0);
        close(fd);
    }

    /* 7) 重复 symlink → EEXIST */
    errno = 0;
    CHECK(symlink("/hello.txt", "/m57-link") == -1 && errno == EEXIST);

    /* 5) link 创建硬链接 + 读取 */
    errno = 0;
    CHECK(link("/hello.txt", "/m57-hard") == 0);
    fd = open("/m57-hard", O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof buf - 1);
        CHECK(n > 0 && memcmp(buf, "hello from", 10) == 0);
        close(fd);
    }

    /* 6) mknod 特殊文件 → EOPNOTSUPP */
    errno = 0;
    CHECK(mknod("/m57-node", S_IFCHR | 0644, 0) == -1 && errno == EOPNOTSUPP);

    if (fails == 0) {
        const char *s = "PASS: m57meta (18/18 checks)\n";
        (void)!write(1, s, 30);
        return 0;
    }
    {
        char b[64];
        int n = __builtin_snprintf(b, sizeof b, "FAIL: m57meta (%d/18)\n", fails);
        (void)!write(2, b, n);
    }
    return 1;
}
