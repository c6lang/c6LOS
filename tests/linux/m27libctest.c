/* M27：musl libc-test functional 子集运行器。
 * 每个测试是独立 ELF（宿主 gcc -static 编译，源码来自 musl libc-test repo
 * commit 18e2849 的 src/functional），放在 initramfs 的 /lt/<name>。
 * 本程序 fork+execve 逐个跑，收集退出码：0=PASS，非 0=FAIL。
 * 只把"宿主自身 PASS"的测试纳入，宿主就失败的都是 locale/环境依赖，豁免。 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *tests[] = {
    "string", "string_memcpy", "string_memset", "string_strchr",
    "string_strcspn", "string_strstr", "qsort", "setjmp", "snprintf",
    "search_tsearch", "search_hsearch", "search_lsearch", "search_insque",
    "strtod", "strtof", "udiv", "wcsstr",
};

static void say(const char *s) { (void)!write(1, s, strlen(s)); }

extern char **environ;

int main(void)
{
    int pass = 0, fail = 0;
    unsigned i;

    for (i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
        char path[64];
        int p[2];
        pid_t c;
        int st = 0;
        char out[128];
        int n = 0;

        snprintf(path, sizeof(path), "/lt/%s", tests[i]);
        if (pipe(p) != 0) { say("FAIL: pipe\n"); fail++; continue; }
        c = fork();
        if (c == 0) {
            char *av[2];
            av[0] = (char *)tests[i];
            av[1] = (char *)0;
            dup2(p[1], 1);
            dup2(p[1], 2);
            close(p[0]); close(p[1]);
            execve(path, av, environ);
            _exit(127);
        }
        close(p[1]);
        for (;;) {
            int k = (int)read(p[0], out + n, (size_t)(sizeof(out) - 1 - n));
            if (k <= 0) break;
            n += k;
            if (n >= (int)sizeof(out) - 1) break;
        }
        out[n] = '\0';
        close(p[0]);
        if (waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            pass++;
        } else {
            char m[192];
            int len = snprintf(m, sizeof(m), "FAIL: %s (rc=%d) %s\n",
                               tests[i], WIFEXITED(st) ? WEXITSTATUS(st) : -1, out);

            (void)!write(1, m, (size_t)len);
            fail++;
        }
    }
    {
        char m[96];
        int len = snprintf(m, sizeof(m), "%s: libc-test functional (%d/%d)\n",
                           fail ? "FAIL" : "PASS", pass, pass + fail);

        (void)!write(1, m, (size_t)len);
    }
    return fail ? 1 : 0;
}
