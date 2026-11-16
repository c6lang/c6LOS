/* L6 验收：信号。
 *
 * 全部走宿主 gcc 编出来的**普通 Linux 程序**，内核侧零改造：
 *   1. kill 自投递 SIGUSR1，handler 正常 return（走 rt_sigreturn）
 *   2. SIGSEGV（真的踩空指针）被 handler 捕获，siglongjmp 回到主流程
 *   3. SIG_IGN 生效
 *   4. 屏蔽期间挂起，sigprocmask(SIG_UNBLOCK) 之后立即投递
 *   5. 子进程把 SIGSEGV 恢复成默认动作，父进程 wait4 拿到 WIFSIGNALED
 */
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>

static sigjmp_buf env;
static volatile int got_usr = 0;

static void say(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0') {
        n++;
    }
    write(1, s, (unsigned)n);
}

static void on_usr(int s)
{
    got_usr = s;                     /* 正常 return，由 rt_sigreturn 恢复上下文 */
}

static void on_segv(int s)
{
    (void)s;
    siglongjmp(env, 1);              /* 不走 rt_sigreturn，直接跳回主流程 */
}

int main(void)
{
    struct sigaction sa;
    struct sigaction dfl;
    sigset_t set;
    int st = 0;
    pid_t p;

    /* 1) 自投递 + handler 正常返回 */
    sa.sa_handler = on_usr;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, (struct sigaction *)0);
    kill(getpid(), SIGUSR1);
    say(got_usr == SIGUSR1 ? "sig: SIGUSR1 handler ok\n"
                           : "sig: SIGUSR1 FAIL\n");

    /* 4a) 屏蔽期间只挂起不投递 */
    got_usr = 0;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, (sigset_t *)0);
    kill(getpid(), SIGUSR1);
    say(got_usr == 0 ? "sig: SIGUSR1 blocked ok\n" : "sig: block FAIL\n");

    /* 4b) 解除屏蔽后立刻投递 */
    sigprocmask(SIG_UNBLOCK, &set, (sigset_t *)0);
    say(got_usr == SIGUSR1 ? "sig: delivered on unblock\n"
                           : "sig: unblock FAIL\n");

    /* 3) SIG_IGN */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR2, &sa, (struct sigaction *)0);
    kill(getpid(), SIGUSR2);
    say("sig: SIG_IGN ok\n");

    /* 5) 默认动作：子进程被 SIGSEGV 杀掉，父进程 wait4 看到信号 */
    p = fork();
    if (p == 0) {
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        dfl.sa_flags = 0;
        sigaction(SIGSEGV, &dfl, (struct sigaction *)0);
        *(volatile int *)0 = 1;      /* 这里应该被 SIGSEGV 终止 */
        _exit(0);
    }
    if (p > 0) {
        if (waitpid(p, &st, 0) == p && WIFSIGNALED(st) &&
            WTERMSIG(st) == SIGSEGV) {
            say("sig: child killed by SIGSEGV\n");
        } else {
            say("sig: child status FAIL\n");
        }
    }

    /* 2) 真踩空指针，handler 里 siglongjmp 回来 */
    sa.sa_handler = on_segv;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, (struct sigaction *)0);
    if (sigsetjmp(env, 1) == 0) {
        *(volatile int *)0 = 1;
        say("sig: no fault?!\n");
    } else {
        say("sig: SIGSEGV caught, back via siglongjmp\n");
    }

    say("sig: done\n");
    return 0;
}
