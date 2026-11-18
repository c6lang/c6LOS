/* M40 验收：copy_file_range(2)（nr326）—— GNU cp 做内核态文件拷贝。
 * 注意本内核的 /tmp 是 ramfs，用普通 open/write 造源文件。
 * 验：
 *   1) off_in/off_out 都传 NULL：从 fd 当前位置拷贝，推进两端 pos
 *   2) 显式 off_in/off_out 指针：从指定偏移拷贝并回写新偏移
 *   3) 拷贝内容正确、返回拷贝字节数
 *   4) 源到 EOF 时返回剩余字节数
 *   5) 非法 flags → EINVAL
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef SYS_copy_file_range
#define SYS_copy_file_range 326
#endif

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static long cfr(int fin, off_t *oin, int fout, off_t *oout, size_t len,
                unsigned flags)
{
    return syscall(SYS_copy_file_range, fin, oin, fout, oout, len, flags);
}

int main(void)
{
    int src, dst;
    off_t oin, oout;
    char buf[64];
    ssize_t n;

    /* 源文件：/tmp/src-m40，内容 0123456789 */
    src = open("/tmp/src-m40", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(src >= 0);
    CHECK(write(src, "0123456789", 10) == 10);
    close(src);
    src = open("/tmp/src-m40", O_RDONLY);
    CHECK(src >= 0);
    dst = open("/tmp/dst-m40", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(dst >= 0);

    /* 1. NULL offsets：拷 4 字节 */
    n = cfr(src, 0, dst, 0, 4, 0);
    CHECK(n == 4);
    CHECK(lseek(src, 0, SEEK_CUR) == 4);
    CHECK(lseek(dst, 0, SEEK_CUR) == 4);
    CHECK(pread(dst, buf, sizeof(buf), 0) == 4);
    CHECK(memcmp(buf, "0123", 4) == 0);

    /* 2. 显式 offsets：从源偏移 5 拷 5 字节到目标偏移 2 */
    oin = 5;
    oout = 2;
    n = cfr(src, &oin, dst, &oout, 5, 0);
    CHECK(n == 5);
    CHECK(oin == 10);                      /* 回写新偏移 */
    CHECK(oout == 7);
    CHECK(pread(dst, buf, sizeof(buf), 0) == 7);
    CHECK(memcmp(buf, "0156789", 7) == 0);

    /* 3. 源到 EOF：从偏移 9 拷 10 字节 → 只有 1 字节 */
    oin = 9;
    oout = 0;
    n = cfr(src, &oin, dst, &oout, 10, 0);
    CHECK(n == 1);
    CHECK(oin == 10);

    /* 4. 非法 flags */
    errno = 0;
    CHECK(cfr(src, 0, dst, 0, 1, 0xdeadbeefu) == -1 && errno == EINVAL);

    close(src);
    close(dst);

    if (failures == 0) {
        dprintf(1, "PASS: m40cfr (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m40cfr (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
