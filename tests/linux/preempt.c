/* L8 验收：抢占式调度。
 *
 * 子进程是**纯用户态死循环**（一次系统调用都不做）。协作式内核里它会一直占着
 * CPU、父进程永远回不来；抢占式内核里定时器会把它切下去，父进程才能打印。
 * 内核同时会打出 [preempt] ticks=N switch=M 作为定量证据。
 */
#include <unistd.h>

static void say(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0') {
        n++;
    }
    write(1, s, (unsigned)n);
}

int main(void)
{
    pid_t p = fork();

    if (p == 0) {
        volatile unsigned long x = 0;

        for (;;) {
            x = x + 1ul;                 /* 不放弃 CPU，也不进内核 */
        }
    }
    if (p < 0) {
        say("preempt: fork FAIL\n");
        return 1;
    }
    /* 到这里说明子进程死循环期间父进程拿到了 CPU —— 只可能是定时器抢占 */
    say("preempt: parent runs while child spins\n");
    return 0;
}
