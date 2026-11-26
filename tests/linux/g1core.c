/* G1-1/G1-3/G1-4/G1-5（TDD 先红）：memfd/seals、mremap、madvise、
 * 1TB PROT_NONE 映射。只用标准 Linux 系统调用，内核侧不认专用接口。 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002u
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004u
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001u
#endif
#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
#endif

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static void fill(unsigned char *p, int n, int seed)
{
    int i;
    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(seed + i);
    }
}

int main(void)
{
    int fd;
    unsigned char buf[64];
    unsigned char *p;
    int i;
    long seals;
    void *huge;
    void *m;
    void *m2;

    fd = memfd_create("g1memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0);
    if (fd < 0) {
        dprintf(1, "FAIL: g1core (%d/%d checks)\n", failures, total);
        return 1;
    }
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    fill(buf, sizeof(buf), 7);
    CHECK(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
    for (i = 0; i < (int)sizeof(buf); i++) {
        CHECK(buf[i] == (unsigned char)(7 + i));
    }
    CHECK(ftruncate(fd, 8192) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(pread(fd, buf, sizeof(buf), 4096) == (ssize_t)sizeof(buf));
    for (i = 0; i < (int)sizeof(buf); i++) {
        CHECK(buf[i] == 0);
    }

    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) == 0);
    seals = fcntl(fd, F_GET_SEALS);
    CHECK(seals >= 0 && (seals & F_SEAL_GROW));
    errno = 0;
    CHECK(ftruncate(fd, 16384) == -1 && errno == EPERM);

    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) {
        CHECK(p[0] == 7);
        p[0] = 99;
        memset(buf, 0, sizeof(buf));
        CHECK(pread(fd, buf, 1, 0) == 1);
        CHECK(buf[0] == 7);                /* 私有映射不反写文件 */
        CHECK(munmap(p, 4096) == 0);
    }

    /* 1TB PROT_NONE：只占地址空间，不占物理页；munmap 必须快速返回。 */
    huge = mmap(NULL, 1ULL << 40, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(huge != MAP_FAILED);
    if (huge != MAP_FAILED) {
        CHECK(munmap(huge, 1ULL << 40) == 0);
    }

    /* V8 的 32 GB pointer cage 会落在高地址并真实访问；只 mmap 不触碰会漏掉
     * "高地址 VMA 补页"这条路径。Chrome 崩溃现场就在这种区间里。 */
    huge = mmap(NULL, 32ULL << 30, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(huge != MAP_FAILED);
    if (huge != MAP_FAILED) {
        volatile unsigned char *hp = (volatile unsigned char *)huge;
        hp[0] = 0x5a;
        hp[1ULL << 30] = 0xa5;
        hp[(32ULL << 30) - 1ULL] = 0x3c;
        CHECK(hp[0] == 0x5a);
        CHECK(hp[1ULL << 30] == 0xa5);
        CHECK(hp[(32ULL << 30) - 1ULL] == 0x3c);
        CHECK(munmap(huge, 32ULL << 30) == 0);
    }

    m = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        fill((unsigned char *)m, 8192, 3);
        m2 = mremap(m, 8192, 16384, MREMAP_MAYMOVE);
        CHECK(m2 != MAP_FAILED);
        if (m2 != MAP_FAILED) {
            CHECK(((unsigned char *)m2)[0] == 3);
            CHECK(((unsigned char *)m2)[8191] == (unsigned char)(3 + 8191));
            ((unsigned char *)m2)[8192] = 55;
            CHECK(((unsigned char *)m2)[8192] == 55);
            CHECK(munmap(m2, 16384) == 0);
        }
    }

    m = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        fill((unsigned char *)m, 8192, 11);
        CHECK(madvise(m, 8192, MADV_DONTNEED) == 0);
        CHECK(((unsigned char *)m)[0] == 0);
        CHECK(((unsigned char *)m)[4096] == 0);
        CHECK(munmap(m, 8192) == 0);
    }

    close(fd);
    if (failures == 0) {
        dprintf(1, "PASS: g1core (%d/%d checks)\n", total, total);
        return 0;
    }
    dprintf(1, "FAIL: g1core (%d/%d checks)\n", failures, total);
    return 1;
}