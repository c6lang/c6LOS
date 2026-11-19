/* M53 验收：getitimer/setitimer
 *  1) setitimer(ITIMER_REAL, {value=2s}) 后 getitimer 读回 value≈2s、interval=0
 *  2) setitimer 带 interval=1s，getitimer 读回 interval=1s
 *  3) setitimer 清零（value=0）后 getitimer 读回 value=0（取消）
 *  4) 非法 which 返回 EINVAL
 *  5) ITIMER_VIRTUAL/PROF 状态往返
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    struct itimerval set, got;

    /* REAL：2 秒一次性 */
    memset(&set, 0, sizeof(set));
    set.it_value.tv_sec = 2;
    CHECK(setitimer(ITIMER_REAL, &set, 0) == 0);
    memset(&got, 0xAA, sizeof(got));
    CHECK(getitimer(ITIMER_REAL, &got) == 0);
    CHECK(got.it_value.tv_sec == 2 || got.it_value.tv_sec == 1); /* 可能已流逝一点 */
    CHECK(got.it_interval.tv_sec == 0 && got.it_interval.tv_usec == 0);

    /* REAL：带 1 秒周期 */
    memset(&set, 0, sizeof(set));
    set.it_interval.tv_sec = 1;
    set.it_value.tv_sec = 5;
    CHECK(setitimer(ITIMER_REAL, &set, &got) == 0);
    /* got 是刚才设的旧值（2s） */
    CHECK(got.it_value.tv_sec == 2 || got.it_value.tv_sec == 1);
    memset(&got, 0, sizeof(got));
    CHECK(getitimer(ITIMER_REAL, &got) == 0);
    CHECK(got.it_interval.tv_sec == 1);
    CHECK(got.it_value.tv_sec == 5 || got.it_value.tv_sec == 4);

    /* 取消 */
    memset(&set, 0, sizeof(set));
    CHECK(setitimer(ITIMER_REAL, &set, 0) == 0);
    memset(&got, 0, sizeof(got));
    CHECK(getitimer(ITIMER_REAL, &got) == 0);
    CHECK(got.it_value.tv_sec == 0 && got.it_value.tv_usec == 0);

    /* VIRTUAL / PROF 状态往返 */
    memset(&set, 0, sizeof(set));
    set.it_value.tv_sec = 3;
    CHECK(setitimer(ITIMER_VIRTUAL, &set, 0) == 0);
    CHECK(setitimer(ITIMER_PROF, &set, 0) == 0);
    memset(&got, 0, sizeof(got));
    CHECK(getitimer(ITIMER_VIRTUAL, &got) == 0);
    CHECK(got.it_value.tv_sec == 3 || got.it_value.tv_sec == 2);
    memset(&got, 0, sizeof(got));
    CHECK(getitimer(ITIMER_PROF, &got) == 0);
    CHECK(got.it_value.tv_sec == 3 || got.it_value.tv_sec == 2);

    /* 非法 which */
    errno = 0;
    CHECK(setitimer((int)99, &set, 0) == -1);
    CHECK(errno == EINVAL);

    if (failures == 0) {
        dprintf(1, "PASS: m53itimer (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m53itimer (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
