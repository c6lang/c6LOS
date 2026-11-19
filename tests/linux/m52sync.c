/* M52 验收：文件同步与咨询锁 syscall
 *  1) fsync/fdatasync 对合法 fd 返回 0，对非法 fd 返回 EBADF
 *  2) flock LOCK_EX/LOCK_SH/LOCK_UN 往返；非法 op 返回 EINVAL
 *  3) sync/syncfs 成功
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    int fd;

    fd = open("/tmp/sync-m52", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) { goto done; }
    CHECK(write(fd, "m52", 3) == 3);

    /* fsync / fdatasync */
    CHECK(fsync(fd) == 0);
    CHECK(fdatasync(fd) == 0);
    errno = 0;
    CHECK(fsync(-1) == -1);
    CHECK(errno == EBADF);

    /* flock 往返 */
    CHECK(flock(fd, LOCK_EX) == 0);
    CHECK(flock(fd, LOCK_SH) == 0);
    CHECK(flock(fd, LOCK_UN) == 0);
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0);
    CHECK(flock(fd, LOCK_UN) == 0);
    errno = 0;
    CHECK(flock(fd, 0) == -1);
    CHECK(errno == EINVAL);

    /* sync / syncfs（syncfs 用 raw syscall） */
    sync();
    CHECK(syscall(306, fd) == 0);       /* syncfs */

    close(fd);
done:
    if (failures == 0) {
        dprintf(1, "PASS: m52sync (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m52sync (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
