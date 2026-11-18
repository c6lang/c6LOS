/* M44 验收：pwrite64(18) / preadv(295) / pwritev(296) / v2(327/328)
 * 验：位置读写不改 fd pos、多段 iovec 按 off 连续推进、内容正确。
 * 注意：x86-64 上 preadv=295 / pwritev=296；327/328 是 preadv2/pwritev2。 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

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

    /* preadv2/pwritev2(327/328) flags=0 等价基础版 */
    wv[0].iov_base = "XY"; wv[0].iov_len = 2;
    CHECK(syscall(SYS_pwritev2, fd, wv, 1, 8ull, 0ul) == 2);
    memset(buf, 0, sizeof(buf));
    rv[0].iov_base = buf; rv[0].iov_len = 2;
    CHECK(syscall(SYS_preadv2, fd, rv, 1, 8ull, 0ul) == 2);
    CHECK(memcmp(buf, "XY", 2) == 0);
    /* 非零 flags（如 RWF_NOWAIT）当前不支持 */
    CHECK(syscall(SYS_preadv2, fd, rv, 1, 8ull, 1ul) == -1);
    CHECK(errno == EOPNOTSUPP);

    close(fd);
    if (failures == 0) {
        dprintf(1, "PASS: m44posio (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m44posio (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
