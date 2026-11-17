/* M13-2a 验收：fork 的写时复制。
 *
 * 判据不是"能跑"（全量复制也能跑），而是**内存**：先匿名映射 48MB 并写两页，
 * 再连续 fork 6 个（进程表 8 槽：init + 6 子 + 留 1 个空位）。全量复制要
 * 6×48MB = 288MB，256MB 内存必然在半路 ENOMEM；真 COW 只多页表页，6 个都能
 * 起来。同时验证隔离：子进程看得到父进程的数据，写的是自己的副本，父进程的
 * 数据不受影响。
 */
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define BIG     (48u * 1024u * 1024u)
#define NCHILD  6

static int failures;

static void say(const char *s)
{
    (void)!write(1, s, strlen(s));
}

static void say_num(int v)
{
    char b[16];
    char c[2];
    unsigned u;
    int i = 0;
    int j;

    if (v < 0) {
        say("-");
        u = (unsigned)(-(v + 1)) + 1u;
    } else {
        u = (unsigned)v;
    }
    if (u == 0u) {
        say("0");
        return;
    }
    while (u > 0u && i < 15) {
        b[i] = (char)('0' + (int)(u % 10u));
        u /= 10u;
        i++;
    }
    for (j = i - 1; j >= 0; j--) {
        c[0] = b[j];
        c[1] = '\0';
        say(c);
    }
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
    unsigned char *big;
    int i;
    int made = 0;
    int st;

    big = (unsigned char *)mmap((void *)0, (size_t)BIG, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (big == MAP_FAILED) {
        say("FAIL: mmap 48MB\n");
        return 1;
    }
    big[0] = 0x11u;
    big[BIG - 1u] = 0x22u;

    for (i = 0; i < NCHILD; i++) {
        pid_t c = fork();

        if (c == 0) {
            if (big[0] != 0x11u || big[BIG - 1u] != 0x22u) {
                _exit(1);                  /* 没看到父进程的数据 */
            }
            big[0] = 0x33u;                /* 写 -> COW 私有化 */
            big[BIG - 1u] = 0x44u;
            if (big[0] != 0x33u || big[BIG - 1u] != 0x44u) {
                _exit(2);
            }
            _exit(0);
        }
        if (c < 0) {
            check(0, "fork (COW 内存没省下来?)");
            break;
        }
        made++;
    }
    check(made == NCHILD, "all forks succeeded");
    check(big[0] == 0x11u && big[BIG - 1u] == 0x22u,
          "parent pages isolated");

    for (i = 0; i < made; i++) {
        if (wait(&st) < 0) {
            int e = errno;

            say("wait failed i=");
            say_num(i);
            say(" errno=");
            say_num(e);
            say(" made=");
            say_num(made);
            say("\n");
            check(0, "wait");
            break;
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            say("bad child i=");
            say_num(i);
            say(" st=");
            say_num(st);
            say("\n");
            check(0, "child content");
        }
    }

    (void)munmap(big, (size_t)BIG);
    if (failures == 0) {
        say("PASS: cow\n");
        return 0;
    }
    say("FAIL: cow\n");
    return 1;
}
