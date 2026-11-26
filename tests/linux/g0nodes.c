/* G0-3（TDD 先红）：initramfs 节点表必须超过旧 FS_MAX_FILES=1024。
 * 镜像里放 1500 个空文件 /f0000../f1499，逐个 open/fstat/close。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
    int i;

    for (i = 0; i < 1500; i++) {
        char path[32];
        struct stat st;
        int fd;

        snprintf(path, sizeof(path), "/f%04d", i);
        fd = open(path, O_RDONLY);
        total++;
        if (fd < 0) {
            failures++;
            dprintf(1, "FAIL: open %s errno=%d\n", path, errno);
            if (i > 1030) {
                break;
            }
            continue;
        }
        CHECK(fstat(fd, &st) == 0);
        CHECK(st.st_size == 0);
        close(fd);
    }
    if (failures == 0) {
        dprintf(1, "PASS: g0nodes (1500 files, %d/%d checks)\n", total, total);
        return 0;
    }
    dprintf(1, "FAIL: g0nodes (%d/%d checks)\n", failures, total);
    return 1;
}
