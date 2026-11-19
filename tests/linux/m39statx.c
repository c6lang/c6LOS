/* M39 验收：statx(2)（nr332）—— 现代 glibc 的 ls/stat 会用它。
 * 验：
 *   1) statx(AT_FDCWD, /hello.txt, 0, STATX_BASIC_STATS, &sx) → 0
 *      stx_mask 含 BASIC_STATS、stx_mode 是普通文件、size=31、ino 非 0
 *   2) statx 目录 → stx_mode 是目录位
 *   3) 不存在的路径 → ENOENT
 *   4) 相对路径 + dirfd（open "/" 后取 "hello.txt"）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    struct statx sx;
    int fd;

    /* 1. 文件：initramfs 里的 /hello.txt（31 字节） */
    memset(&sx, 0, sizeof(sx));
    errno = 0;
    CHECK(statx(AT_FDCWD, "/hello.txt", 0, STATX_BASIC_STATS, &sx) == 0);
    CHECK((sx.stx_mask & STATX_BASIC_STATS) != 0);
    CHECK(S_ISREG(sx.stx_mode));
    CHECK(sx.stx_size == 31);
    CHECK(sx.stx_ino != 0);
    CHECK(sx.stx_blksize == 4096);

    /* 2. 目录 */
    memset(&sx, 0, sizeof(sx));
    CHECK(statx(AT_FDCWD, "/", 0, STATX_BASIC_STATS, &sx) == 0);
    CHECK(S_ISDIR(sx.stx_mode));

    /* 3. 不存在 */
    memset(&sx, 0, sizeof(sx));
    errno = 0;
    CHECK(statx(AT_FDCWD, "/no-such-file-m39", 0,
                STATX_BASIC_STATS, &sx) == -1 && errno == ENOENT);

    /* 4. 相对路径 + dirfd */
    fd = open("/", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    memset(&sx, 0, sizeof(sx));
    CHECK(statx(fd, "hello.txt", 0, STATX_BASIC_STATS, &sx) == 0);
    CHECK(sx.stx_size == 31);
    close(fd);

    if (failures == 0) {
        dprintf(1, "PASS: m39statx (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m39statx (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
