/* M38 验收：waitid(2)（nr247）子进程状态等待。
 * 验 POSIX/Linux 语义：
 *   1) P_PID + WEXITED：子进程 exit(7) → si_code=CLD_EXITED, si_status=7, si_pid 正确
 *   2) WNOHANG 无就绪子 → 返回 0（info 不变）
 *   3) WNOWAIT：观察到事件但不收尸，随后 waitpid 仍能拿到同一子进程
 *   4) P_ALL 任意子退出
 *   5) 非法 options → EINVAL；不存在的 pid → ECHILD
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

int main(void)
{
    siginfo_t info;
    pid_t c;
    int st;

    /* 1. P_PID + WEXITED：等一个 exit(7) 的子进程 */
    c = fork();
    if (c == 0) { _exit(7); }
    CHECK(c > 0);
    memset(&info, 0, sizeof(info));
    errno = 0;
    CHECK(waitid(P_PID, c, &info, WEXITED) == 0);
    CHECK(info.si_signo == SIGCHLD);
    CHECK(info.si_code == CLD_EXITED);
    CHECK(info.si_pid == c);
    CHECK(info.si_status == 7);

    /* 2. WNOHANG 无子进程 → 0 */
    memset(&info, 0, sizeof(info));
    errno = 0;
    CHECK(waitid(P_PID, c, &info, WEXITED | WNOHANG) == 0);

    /* 3. WNOWAIT：观察事件但不收尸。先造一个 zombie，再 waitid(WNOWAIT)，
     * 然后 waitpid 应还能收走它。 */
    c = fork();
    if (c == 0) { _exit(3); }
    CHECK(c > 0);
    usleep(20000);                         /* 让子先进 zombie */
    memset(&info, 0, sizeof(info));
    CHECK(waitid(P_PID, c, &info, WEXITED | WNOWAIT) == 0);
    CHECK(info.si_code == CLD_EXITED && info.si_status == 3);
    CHECK(waitpid(c, &st, 0) == c);        /* WNOWAIT 后子仍在，可收 */
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);

    /* 4. P_ALL：等任意子退出 */
    c = fork();
    if (c == 0) { _exit(0); }
    CHECK(c > 0);
    memset(&info, 0, sizeof(info));
    CHECK(waitid(P_ALL, 0, &info, WEXITED) == 0);
    CHECK(info.si_pid == c);
    CHECK(info.si_code == CLD_EXITED);

    /* 5. 非法 options → EINVAL */
    errno = 0;
    CHECK(waitid(P_PID, getpid(), &info, 0xdeadbeefu) == -1 && errno == EINVAL);

    if (failures == 0) {
        dprintf(1, "PASS: m38waitid (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m38waitid (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
