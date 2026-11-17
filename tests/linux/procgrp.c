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

/* M14-2/M14-3 追加：作业控制（SIGSTOP/SIGCONT + waitpid 选项）、tcsetpgrp、
 * EINTR/SA_RESTART。这三块是 shell（ash/bash）做 job control 和 readline
 * 中断的必要条件。 */
static volatile int got_alrm;

static void on_alrm(int s)
{
    got_alrm = s;
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

    /* ---- M14-2：作业控制 -------------------------------------------------- */

    /* 7. SIGSTOP 停下 → waitpid(WUNTRACED) 报 WIFSTOPPED，SIGCONT 之后能继续跑 */
    if (pipe(p) != 0) {
        say("FAIL: pipe3\n");
        return 1;
    }
    c = fork();
    if (c == 0) {
        char b = 0;

        close(p[1]);
        (void)!read(p[0], &b, 1);      /* 被停在这里，SIGCONT 之后才往下走 */
        _exit(42);
    }
    if (c < 0) {
        check(0, "fork5");
    } else {
        close(p[0]);
        check(kill(c, SIGSTOP) == 0, "kill SIGSTOP");
        st = 0;
        check(waitpid(c, &st, WUNTRACED) == c, "waitpid WUNTRACED");
        check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "WIFSTOPPED SIGSTOP");
        /* 停着的进程不该自己前进：再等一次仍未退出 */
        check(waitpid(c, &st, WNOHANG) == 0, "stopped child not reapable");
        check(kill(c, SIGCONT) == 0, "kill SIGCONT");
        (void)!write(p[1], "x", 1);    /* 放它读完退出 */
        close(p[1]);
        st = 0;
        check(waitpid(c, &st, 0) == c, "waitpid after CONT");
        check(WIFEXITED(st) && WEXITSTATUS(st) == 42, "child ran after CONT");
    }

    /* 8. WCONTINUED：续行事件单独上报 */
    if (pipe(p) != 0) {
        say("FAIL: pipe4\n");
        return 1;
    }
    c = fork();
    if (c == 0) {
        char b = 0;

        close(p[1]);
        (void)!read(p[0], &b, 1);
        _exit(0);
    }
    if (c < 0) {
        check(0, "fork6");
    } else {
        close(p[0]);
        check(kill(c, SIGSTOP) == 0, "kill SIGSTOP 2");
        st = 0;
        check(waitpid(c, &st, WUNTRACED) == c && WIFSTOPPED(st),
              "WUNTRACED 2");
        check(kill(c, SIGCONT) == 0, "kill SIGCONT 2");
        st = 0;
        check(waitpid(c, &st, WCONTINUED) == c, "waitpid WCONTINUED");
        check(WIFCONTINUED(st), "WIFCONTINUED");
        (void)!write(p[1], "x", 1);
        close(p[1]);
        check(waitpid(c, &st, 0) == c, "reap after WCONTINUED");
    }

    /* 9. tcsetpgrp/tcgetpgrp：非终端 fd 必须 ENOTTY */
    if (pipe(p) == 0) {
        errno = 0;
        check(tcgetpgrp(p[0]) == -1 && errno == ENOTTY, "tcgetpgrp ENOTTY");
        close(p[0]);
        close(p[1]);
    }

    /* ---- M14-3：EINTR / SA_RESTART --------------------------------------- */

    /* 10. 装了 handler 的信号打断阻塞 read → read 返回 -1/EINTR，handler 跑过 */
    if (pipe(p) == 0) {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alrm;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;               /* 不带 SA_RESTART：必须 EINTR */
        (void)sigaction(SIGALRM, &sa, NULL);
        got_alrm = 0;
        (void)alarm(1);                /* 没人写这个管道：只能靠 SIGALRM 出来 */
        errno = 0;
        n = read(p[0], &st, 1);
        check(n == -1 && errno == EINTR, "read EINTR");
        check(got_alrm == SIGALRM, "handler ran on EINTR");
        (void)alarm(0);
        close(p[0]);
        close(p[1]);
    }

    /* 11. SA_RESTART：read 被信号打断后必须**继续等**，而不是返回 EINTR。
     *     用子进程显式 kill(getppid(), SIGALRM) —— 比 alarm(2) 可靠：闹钟
     *     在内核忙等期间不会 tick（可观测时钟是 rdtsc，但阻塞进程不占 CPU），
     *     而子进程直接发信号能保证"父进程确实正在 read 里睡着"时被叫醒。 */
    if (pipe(p) == 0) {
        struct sigaction sa;
        char b = 0;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alrm;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        (void)sigaction(SIGALRM, &sa, NULL);
        got_alrm = 0;
        c = fork();
        if (c == 0) {
            close(p[0]);
            sleep(1);                       /* 给父进程时间进到 read() 里 */
            (void)!kill(getppid(), SIGALRM); /* 打断它；带 SA_RESTART 要重试 */
            sleep(1);
            (void)!write(p[1], "z", 1);     /* 2 秒后才来数据 */
            close(p[1]);
            _exit(0);
        }
        close(p[1]);
        if (c > 0) {
            errno = 0;
            n = read(p[0], &b, 1);
            check(n == 1 && b == 'z', "read restarted by SA_RESTART");
            check(got_alrm == SIGALRM, "handler ran before restart");
            check(waitpid(c, &st, 0) == c, "reap writer");
        } else {
            check(0, "fork7");
        }
        close(p[0]);
    }

    /* 12. waitpid(-1) 必须每次收不同的孩子 —— 僵尸要真的从表里清掉。
     *     以前 wait4 只把"被信号杀"的设成 FREE，正常退出的永远是 ZOMBIE，
     *     waitpid(-1) 会一遍遍拿到同一个死孩子（alpine 的 sh 就卡在这）。 */
    {
        pid_t a = fork();

        if (a == 0) {
            _exit(11);
        }
        if (a > 0) {
            check(waitpid(-1, &st, 0) == a, "waitpid(-1) first child");
            a = fork();
            if (a == 0) {
                _exit(22);
            }
            if (a > 0) {
                check(waitpid(-1, &st, 0) == a, "waitpid(-1) next child");
            }
        }
    }

    if (failures == 0) {
        say("PASS: procgrp\n");
        return 0;
    }
    say("FAIL: procgrp\n");
    return 1;
}
