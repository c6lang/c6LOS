/* M43 验收：fallocate(2)（nr285）
 * 验 mode=0 扩展文件大小、写入读回、offset 不变；
 * FALLOC_FL_KEEP_SIZE no-op、PUNCH_HOLE=EOPNOTSUPP、非法 flags=EINVAL。 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef SYS_fallocate
#define SYS_fallocate 285
#endif

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    int fd;
    struct stat st;
    char buf[64];
    long r;

    fd = open("/tmp/falloc-m43", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "AB", 2) == 2);

    /* mode=0 扩展到 32 字节 */
    r = syscall(SYS_fallocate, fd, 0, 0ull, 32ull);
    CHECK(r == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 2);       /* fallocate 不改 pos */
    CHECK(fstat(fd, &st) == 0 && st.st_size == 32);
    /* 扩展区读回全 0 */
    CHECK(pread(fd, buf, sizeof(buf), 0) == 32);
    CHECK(memcmp(buf, "AB", 2) == 0);
    CHECK(buf[2] == 0 && buf[31] == 0);

    /* KEEP_SIZE：no-op，不改大小 */
    r = syscall(SYS_fallocate, fd, 0x01, 0ull, 64ull);
    CHECK(r == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 32);

    /* PUNCH_HOLE → EOPNOTSUPP */
    errno = 0;
    CHECK(syscall(SYS_fallocate, fd, 0x03, 0ull, 8ull) == -1 && errno == EOPNOTSUPP);
    /* 非法 flag → EINVAL */
    errno = 0;
    CHECK(syscall(SYS_fallocate, fd, 0x04, 0ull, 8ull) == -1 && errno == EINVAL);

    close(fd);
    if (failures == 0) {
        dprintf(1, "PASS: m43fallocate (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m43fallocate (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
