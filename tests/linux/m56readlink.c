/* M56 验收：readlink/readlinkat 读取真实 initramfs 符号链接
 * initramfs 里 /hello-link 是 S_IFLNK，目标字符串 "/hello.txt"。
 * 1) readlink("/hello-link") 返回目标字符串长度与内容
 * 2) bufsiz 太小时截断
 * 3) 非符号链接 /hello.txt → EINVAL
 * 4) 不存在的路径 → ENOENT
 * 5) readlinkat(AT_FDCWD, ...) 等价
 * 6) /proc/self/exe 特例仍返回 "/hello" */
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { \
    char b[128]; int n = __builtin_snprintf(b, sizeof b, \
        "FAIL: %s (line %d, errno=%d)\n", #x, __LINE__, errno); \
    (void)!write(2, b, n); fails++; } } while (0)

int main(void)
{
    char buf[64];
    ssize_t n;

    /* 1) 真实符号链接 */
    memset(buf, 0, sizeof buf);
    errno = 0;
    n = readlink("/hello-link", buf, sizeof buf);
    CHECK(n == 10);
    CHECK(n > 0 && memcmp(buf, "/hello.txt", 10) == 0);

    /* 2) bufsiz 截断 */
    memset(buf, 0, sizeof buf);
    n = readlink("/hello-link", buf, 4);
    CHECK(n == 4);
    CHECK(memcmp(buf, "/hel", 4) == 0);

    /* 3) 非符号链接 */
    errno = 0;
    n = readlink("/hello.txt", buf, sizeof buf);
    CHECK(n == -1 && errno == EINVAL);

    /* 4) 不存在 */
    errno = 0;
    n = readlink("/no-such-link", buf, sizeof buf);
    CHECK(n == -1 && errno == ENOENT);

    /* 5) readlinkat */
    memset(buf, 0, sizeof buf);
    n = readlinkat(AT_FDCWD, "/hello-link", buf, sizeof buf);
    CHECK(n == 10 && memcmp(buf, "/hello.txt", 10) == 0);

    /* 6) /proc/self/exe 特例 */
    memset(buf, 0, sizeof buf);
    n = readlink("/proc/self/exe", buf, sizeof buf);
    CHECK(n == 6 && memcmp(buf, "/hello", 6) == 0);

    if (fails == 0) {
        const char *s = "PASS: m56readlink (12/12 checks)\n";
        (void)!write(1, s, 34);
        return 0;
    }
    {
        char b[64];
        int n = __builtin_snprintf(b, sizeof b, "FAIL: m56readlink (%d/12)\n", fails);
        (void)!write(2, b, n);
    }
    return 1;
}
