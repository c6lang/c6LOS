/* M44 验收：pwrite64(18) / preadv(327) / pwritev(328)
 * 验：位置读写不改 fd pos、多段 iovec 按 off 连续推进、内容正确。 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef SYS_pwritev
#define SYS_pwritev 328
#endif
#ifndef SYS_preadv
#define SYS_preadv 327
#endif

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    int fd;
    char buf[32];
    struct iovec wv[2], rv[2];

    fd = open("/tmp/posio-m44", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);

    /* pwrite 到偏移 4，不改 pos */
    CHECK(pwrite(fd, "WXYZ", 4, 4) == 4);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    CHECK(pread(fd, buf, sizeof(buf), 0) == 8);
    CHECK(buf[0] == 0 && memcmp(buf + 4, "WXYZ", 4) == 0);

    /* pwritev 两段到偏移 0："ab" + "cdef" */
    wv[0].iov_base = "ab"; wv[0].iov_len = 2;
    wv[1].iov_base = "cdef"; wv[1].iov_len = 4;
    CHECK(syscall(SYS_pwritev, fd, wv, 2, 0ull) == 6);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);

    /* preadv 两段从偏移 0：第一段 3 字节，第二段 3 字节 */
    memset(buf, 0, sizeof(buf));
    rv[0].iov_base = buf; rv[0].iov_len = 3;
    rv[1].iov_base = buf + 3; rv[1].iov_len = 3;
    CHECK(syscall(SYS_preadv, fd, rv, 2, 0ull) == 6);
    CHECK(memcmp(buf, "abcdef", 6) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);

    close(fd);
    if (failures == 0) {
        dprintf(1, "PASS: m44posio (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m44posio (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
