/* M35：管道 fd 的 fstat/isatty ABI 语义。
 * 以前 lx_fs_fstat 把管道 fd 的 node（小整数 id）当 initramfs 节点号，
 * busybox/musl 的 cat fstat(0) 会看到普通文件 size=0 而不读 stdin；
 * 另外 ioctl(TCGETS) 只按 fd<=2 判终端，dup2 到 fd0 的管道被误认为 tty。
 * 本测试验这两个语义修正。 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
int main(void)
{
    int p[2];
    struct stat sb;
    ssize_t n;
    char b[16];

    if (pipe(p) != 0) { dprintf(1, "FAIL: pipe errno=%d\n", errno); return 1; }
    if (fstat(p[0], &sb) != 0 || !S_ISFIFO(sb.st_mode)) {
        dprintf(1, "FAIL: fstat(read-end) not FIFO mode=%o\n",
                (unsigned)sb.st_mode);
        return 1;
    }
    if (fstat(p[1], &sb) != 0 || !S_ISFIFO(sb.st_mode)) {
        dprintf(1, "FAIL: fstat(write-end) not FIFO mode=%o\n",
                (unsigned)sb.st_mode);
        return 1;
    }
    if (dup2(p[0], 0) < 0) { dprintf(1, "FAIL: dup2 errno=%d\n", errno); return 1; }
    if (fstat(0, &sb) != 0 || !S_ISFIFO(sb.st_mode)) {
        dprintf(1, "FAIL: fstat(dup'd fd0) not FIFO\n");
        return 1;
    }
    /* dup2 后 fd0 是管道：isatty(0) 必须失败（ENOTTY）。 */
    errno = 0;
    if (ioctl(0, TCGETS, &sb) != -1 || errno != ENOTTY) {
        dprintf(1, "FAIL: ioctl(TCGETS) on pipe fd0 errno=%d\n", errno);
        return 1;
    }
    /* lseek 在管道上必须返回 ESPIPE（busybox cat 探测 stdin 时依赖）。 */
    errno = 0;
    if (lseek(0, 0, SEEK_CUR) != (off_t)-1 || errno != ESPIPE) {
        dprintf(1, "FAIL: lseek(pipe fd0) errno=%d (want ESPIPE)\n", errno);
        return 1;
    }
    /* 管道数据路径本身正常（同进程 dup2 后 read）。 */
    if (write(p[1], "FIFO-OK", 7) != 7) { dprintf(1,"FAIL write\n"); return 1; }
    n = read(0, b, sizeof(b));
    if (n != 7 || memcmp(b, "FIFO-OK", 7) != 0) {
        dprintf(1, "FAIL: pipe read=%zd\n", n); return 1;
    }
    dprintf(1, "PASS: m35fifo (pipe fstat=S_IFIFO, isatty=ENOTTY, read ok)\n");
    return 0;
}
