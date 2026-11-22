/* M15 验收：VFS/设备 syscall 语义矩阵。宿主 gcc 编译的普通 Linux 程序，
 * 内核侧零专用接口。覆盖：
 *   1. 写类：mkdir/rmdir/unlink/rename/truncate/ftruncate（/tmp 的 ramfs）
 *   2. *at 相对路径：openat/fstatat(dirfd, 相对名) —— B7
 *   3. 挂载点 stat 可见：stat("/tmp/...") —— B8
 *   4. statfs：df 读的那组字段非零
 *   5. devfs：/dev/null 吞写、/dev/zero 全 0、/dev/urandom 有数据、/dev/full ENOSPC
 * 用例之间不依赖 sleep，抢占下确定。 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;

static void say(const char *s)
{
    (void)!write(1, s, strlen(s));
}

static void check(int cond, const char *what)
{
    if (!cond) {
        failures++;
        say("FAIL: ");
        say(what);
        say("\n");
    }
}

/* glibc 未必导出 renameat/truncate 的 wrapper，用原始 syscall 兜底。 */
static int raw_renameat(int od, const char *o, int nd, const char *n)
{
#ifdef SYS_renameat
    return (int)syscall(SYS_renameat, od, o, nd, n);
#else
    (void)od; (void)o; (void)nd; (void)n;
    return -1;
#endif
}

int main(void)
{
    int dfd;
    int fd;
    char buf[64];
    char mbuf[512];
    struct stat st;
    struct statfs sfs;
    ssize_t n;

    /* ---- 1. mkdir / rmdir / unlink / rename / truncate ---- */
    check(mkdir("/tmp/m15dir", 0755) == 0 || errno == EEXIST, "mkdir");
    dfd = open("/tmp/m15dir", O_RDONLY | O_DIRECTORY);
    check(dfd >= 0, "open dir");

    fd = openat(dfd, "a.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "openat create");
    if (fd >= 0) {
        check(write(fd, "hello", 5) == 5, "write a.txt");
        close(fd);
    }
    check(fstatat(dfd, "a.txt", &st, 0) == 0 && st.st_size == 5,
          "fstatat dirfd relative");

    /* stat 挂载点可见（走 VFS，不是 initramfs 扁平表） */
    check(stat("/tmp/m15dir/a.txt", &st) == 0 && st.st_size == 5,
          "stat mountpoint visible");

    check(raw_renameat(AT_FDCWD, "/tmp/m15dir/a.txt",
                       AT_FDCWD, "/tmp/m15dir/b.txt") == 0, "renameat");
    check(fstatat(dfd, "b.txt", &st, 0) == 0, "fstatat after rename");
    check(fstatat(dfd, "a.txt", &st, 0) != 0, "old name gone");

    fd = openat(dfd, "b.txt", O_RDWR);
    if (fd >= 0) {
        check(ftruncate(fd, 3) == 0, "ftruncate");
        close(fd);
        check(stat("/tmp/m15dir/b.txt", &st) == 0 && st.st_size == 3,
              "size after ftruncate");
    }
    check(truncate("/tmp/m15dir/b.txt", 1) == 0, "truncate");
    check(unlinkat(dfd, "b.txt", 0) == 0, "unlinkat");
    check(fstatat(dfd, "b.txt", &st, 0) != 0, "unlinked gone");
    close(dfd);
    check(rmdir("/tmp/m15dir") == 0, "rmdir");

    /* ---- 2. statfs：/tmp 是 ramfs 挂载点，报 arena 大小而不是 256MB 兜底值 ---- */
    memset(&sfs, 0, sizeof(sfs));
    check(statfs("/tmp", &sfs) == 0, "statfs");
    check(sfs.f_bsize > 0 && sfs.f_blocks > 0 && sfs.f_bavail > 0,
          "statfs fields nonzero");
    check((unsigned long long)sfs.f_bsize * (unsigned long long)sfs.f_blocks
              <= 1024ull * 1024ull,
          "statfs /tmp uses ramfs arena size");

    /* ---- 2.5 挂载点 chdir：/tmp 是 ramfs 挂载点，m15 的 initramfs 里没有
     * 这个目录项。以前 chdir 只查 initramfs 扁平表，cd /tmp 直接 ENOENT。 ---- */
    check(chdir("/tmp") == 0, "chdir mountpoint");
    memset(mbuf, 0, sizeof(mbuf));
    check(getcwd(mbuf, sizeof(mbuf)) != NULL && strcmp(mbuf, "/tmp") == 0,
          "getcwd mountpoint");
    fd = open("mntrel.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open relative after chdir mountpoint");
    if (fd >= 0) {
        check(write(fd, "rel", 3) == 3, "write relative after chdir");
        close(fd);
    }
    check(stat("mntrel.txt", &st) == 0 && st.st_size == 3,
          "stat relative after chdir");
    check(chdir("/") == 0, "chdir back to root");
    check(stat("/tmp/mntrel.txt", &st) == 0 && st.st_size == 3,
          "relative file landed in /tmp");
    check(unlink("/tmp/mntrel.txt") == 0, "unlink relative file");

    /* ---- 2.6 /proc/mounts 要列出真实挂载点（df -h 读它） ---- */
    fd = open("/proc/mounts", O_RDONLY);
    check(fd >= 0, "open /proc/mounts");
    if (fd >= 0) {
        memset(mbuf, 0, sizeof(mbuf));
        n = read(fd, mbuf, sizeof(mbuf) - 1);
        check(n > 0, "read /proc/mounts");
        if (n > 0) {
            mbuf[n] = '\0';
            check(strstr(mbuf, "/tmp ramfs") != NULL,
                  "/proc/mounts lists /tmp mount");
        }
        close(fd);
    }

    /* ---- 3. devfs ---- */
    fd = open("/dev/null", O_WRONLY);
    check(fd >= 0, "open /dev/null");
    if (fd >= 0) {
        check(write(fd, "anything", 8) == 8, "write /dev/null");
        close(fd);
    }
    fd = open("/dev/zero", O_RDONLY);
    check(fd >= 0, "open /dev/zero");
    if (fd >= 0) {
        memset(buf, 0x5a, sizeof(buf));
        n = read(fd, buf, sizeof(buf));
        check(n == (ssize_t)sizeof(buf), "read /dev/zero len");
        {
            int i, allzero = 1;
            for (i = 0; i < (int)sizeof(buf); i++) {
                if (buf[i] != 0) { allzero = 0; break; }
            }
            check(allzero, "read /dev/zero all zero");
        }
        close(fd);
    }
    fd = open("/dev/urandom", O_RDONLY);
    check(fd >= 0, "open /dev/urandom");
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf));
        check(n == (ssize_t)sizeof(buf), "read /dev/urandom len");
        close(fd);
    }
    fd = open("/dev/full", O_WRONLY);
    check(fd >= 0, "open /dev/full");
    if (fd >= 0) {
        errno = 0;
        check(write(fd, "x", 1) == -1 && errno == ENOSPC, "write /dev/full ENOSPC");
        close(fd);
    }

    if (failures == 0) {
        say("PASS: m15fs\n");
        return 0;
    }
    say("FAIL: m15fs\n");
    return 1;
}
