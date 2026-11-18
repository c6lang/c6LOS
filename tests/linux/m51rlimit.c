/* M51 验收：资源/调度类 syscall
 *  1) getrlimit 默认 INFINITY；setrlimit 后 getrlimit 读回；fork 继承
 *  2) prlimit64(pid=0) set/get 往返
 *  3) getpriority 默认 20；setpriority 改变后读回
 *  4) getrusage 全零成功；mlock/munlock 成功；sethostname 成功
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    struct rlimit rl, back;
    struct rusage ru;
    int prio;
    pid_t kid;
    int status = 0;

    /* getrlimit 默认：RLIM_INFINITY */
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    CHECK(rl.rlim_cur == RLIM_INFINITY && rl.rlim_max == RLIM_INFINITY);

    /* setrlimit 后读回 */
    rl.rlim_cur = 64; rl.rlim_max = 128;
    CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0);
    memset(&back, 0, sizeof(back));
    CHECK(getrlimit(RLIMIT_NOFILE, &back) == 0);
    CHECK(back.rlim_cur == 64 && back.rlim_max == 128);

    /* prlimit64(pid=0=self) 往返 */
    rl.rlim_cur = 256; rl.rlim_max = 512;
    CHECK(syscall(SYS_prlimit64, 0, RLIMIT_STACK, &rl, 0) == 0);
    memset(&back, 0, sizeof(back));
    CHECK(syscall(SYS_prlimit64, 0, RLIMIT_STACK, 0, &back) == 0);
    CHECK(back.rlim_cur == 256 && back.rlim_max == 512);

    /* fork 继承 rlimit */
    kid = fork();
    if (kid == 0) {
        struct rlimit c;
        if (getrlimit(RLIMIT_NOFILE, &c) == 0 && c.rlim_cur == 64) {
            _exit(0);
        }
        _exit(7);
    }
    CHECK(kid > 0);
    CHECK(waitpid(kid, &status, 0) == kid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* getpriority 默认 20；setpriority 改 */
    errno = 0;
    prio = getpriority(PRIO_PROCESS, 0);
    CHECK(errno == 0);
    CHECK(prio == 0);                       /* glibc：默认 nice=0 */
    CHECK(setpriority(PRIO_PROCESS, 0, 10) == 0);
    errno = 0;
    prio = getpriority(PRIO_PROCESS, 0);
    CHECK(errno == 0);
    CHECK(prio == 10);

    /* getrusage / mlock / munlock / sethostname */
    memset(&ru, 0, sizeof(ru));
    CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
    CHECK(ru.ru_utime.tv_sec == 0 && ru.ru_maxrss == 0);
    CHECK(syscall(SYS_mlock, 0ull, 0ull) == 0);
    CHECK(syscall(SYS_munlock, 0ull, 0ull) == 0);
    CHECK(sethostname("c6os-test", 9) == 0);

    if (failures == 0) {
        dprintf(1, "PASS: m51rlimit (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m51rlimit (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
