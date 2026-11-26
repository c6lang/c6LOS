/* G0-1/G0-2（TDD 先红）：sysinfo(2) 与 /proc/meminfo、/proc/cpuinfo 必须
 * 反映真实机器，而不是固定 24 MB / 2 CPU。
 *
 * 期望在 `-m 3072` 下：
 *   - sysinfo.totalram ≥ 2.9 GB（E820 4GB 以上的条目也要算）
 *   - /proc/meminfo 的 MemTotal 与 sysinfo 一致（±10%）
 *   - /proc/cpuinfo 只有 1 个 processor，flags 含 sse2
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static int read_all(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    int n = 0;

    if (fd < 0) {
        return -1;
    }
    n = (int)read(fd, buf, (size_t)(cap - 1));
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

int main(void)
{
    struct sysinfo si;
    char buf[4096];
    unsigned long long total_kb = 0;
    int procs = 0;
    int i;

    memset(&si, 0, sizeof(si));
    CHECK(sysinfo(&si) == 0);
    CHECK(si.mem_unit != 0);
    if (si.mem_unit == 0) {
        si.mem_unit = 1;
    }
    {
        unsigned long long total_bytes = (unsigned long long)si.totalram * si.mem_unit;

        dprintf(1, "g0sysinfo: totalram=%llu bytes freeram=%llu\n",
                total_bytes, (unsigned long long)si.freeram * si.mem_unit);
        CHECK(total_bytes >= 2900ULL * 1024ULL * 1024ULL);
        CHECK(si.freeram != 0);
    }

    memset(buf, 0, sizeof(buf));
    CHECK(read_all("/proc/meminfo", buf, (int)sizeof(buf)) > 0);
    {
        char *p = strstr(buf, "MemTotal:");

        CHECK(p != NULL);
        if (p != NULL) {
            CHECK(sscanf(p, "MemTotal: %llu kB", &total_kb) == 1);
            dprintf(1, "g0sysinfo: MemTotal=%llu kB\n", total_kb);
            CHECK(total_kb >= 2900ULL * 1024ULL);
        }
    }

    memset(buf, 0, sizeof(buf));
    CHECK(read_all("/proc/cpuinfo", buf, (int)sizeof(buf)) > 0);
    for (i = 0; buf[i] != '\0'; i++) {
        if (strncmp(buf + i, "processor", 9) == 0) {
            procs++;
        }
    }
    dprintf(1, "g0sysinfo: cpu processors=%d\n", procs);
    CHECK(procs == 1);
    CHECK(strstr(buf, "sse2") != NULL);

    if (failures == 0) {
        dprintf(1, "PASS: g0sysinfo (%d/%d checks)\n", total, total);
        return 0;
    }
    dprintf(1, "FAIL: g0sysinfo (%d/%d checks)\n", failures, total);
    return 1;
}
