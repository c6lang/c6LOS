/* M14-1 验收：进程组 / wait4 选项 / SIGPIPE。
 *
 * 都是 musl/glibc 程序在真实系统里依赖的语义：
 *   - setpgid(0,0) 之后 getpgrp/getpgid 必须跟着变（旧实现里 setpgid 与 chdir
 *     共用 case，实际去 chdir(pid) 了）；
 *   - wait4 的 WNOHANG 不能阻塞、pid<0 按进程组收尸；
 *   - 管道读端全关后写必须 -1/EPIPE；SIGPIPE 默认动作是杀进程。
 *
 * 用例之间用 pipe/阻塞读做同步，不靠 sleep，抢占调度下也是确定的。 */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
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

int main(void)
{
    int p[2];
    pid_t c;
    int st;
    ssize_t n;

    /* 1. 自己建组 */
    check(setpgid(0, 0) == 0, "setpgid(0,0)");
    check(getpgrp() == getpid(), "getpgrp after setpgid");
    check(getpgid(getpid()) == getpid(), "getpgid after setpgid");

    /* 2. 父进程把子进程移进它自己的组（子进程先阻塞在 read 上，避免竞态） */
    if (pipe(p) != 0) {
        say("FAIL: pipe\n");
        return 1;
    }
    c = fork();
    if (c == 0) {
        char b = 0;

        close(p[1]);
        (void)!read(p[0], &b, 1);
        _exit(getpgrp() == getpid() ? 0 : 7);
    }
    if (c < 0) {
        check(0, "fork");
    } else {
        check(setpgid(c, c) == 0, "setpgid(child,child)");
        check(getpgid(c) == c, "getpgid(child)");
        close(p[0]);
        (void)!write(p[1], "x", 1);
        st = 0;
        check(waitpid(c, &st, 0) == c, "waitpid child");
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child pgrp");
    }
    close(p[1]);

    /* 3. WNOHANG：子进程还活着时必须立刻返回 0（或已经退出就返回 pid） */
    c = fork();
    if (c == 0) {
        _exit(0);
    }
    if (c < 0) {
        check(0, "fork2");
    } else {
        int r = waitpid(c, &st, WNOHANG);

        check(r == 0 || r == c, "WNOHANG");
        if (r == 0) {
            check(waitpid(c, &st, 0) == c, "wait after WNOHANG");
        }
    }

    /* 4. pid < -1：按进程组收尸 */
    c = fork();
    if (c == 0) {
        _exit(0);
    }
    if (c < 0) {
        check(0, "fork3");
    } else {
        check(setpgid(c, c) == 0, "setpgid for group wait");
        check(waitpid(-c, &st, 0) == c, "waitpid(-pgid)");
    }

    /* 5. 读端全关 + SIGPIPE 忽略：write 返回 -1/EPIPE，进程不死 */
    (void)signal(SIGPIPE, SIG_IGN);
    if (pipe(p) == 0) {
        close(p[0]);
        errno = 0;
        n = write(p[1], "x", 1);
        check(n == -1 && errno == EPIPE, "write EPIPE");
        close(p[1]);
    } else {
        check(0, "pipe2");
    }

    /* 6. 默认 SIGPIPE：子进程写死管道，父进程看到 WIFSIGNALED/SIGPIPE */
    c = fork();
    if (c == 0) {
        int q[2];

        (void)signal(SIGPIPE, SIG_DFL);
        if (pipe(q) != 0) {
            _exit(2);
        }
        close(q[0]);
        (void)!write(q[1], "x", 1);   /* 无读端 -> 默认动作杀进程 */
        _exit(3);
    }
    if (c < 0) {
        check(0, "fork4");
    } else {
        check(waitpid(c, &st, 0) == c, "waitpid sigpipe");
        check(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE,
              "child killed by SIGPIPE");
    }

    if (failures == 0) {
        say("PASS: procgrp\n");
        return 0;
    }
    say("FAIL: procgrp\n");
    return 1;
}
