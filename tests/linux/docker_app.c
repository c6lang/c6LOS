/* tests/linux/docker_app.c —— 镜像里的"应用"（M11-3）
 *
 * 刻意**不用 libc**（-nostdlib 直接 syscall），编出来只有几 KB：
 *   - rootfs 小到能整段放在 1MB 以下的常规内存里，引导块不用切 unreal mode；
 *   - 也顺带证明 Linux ABI 的 open/write/read/close 是真的可用，不是靠 libc 兜着。
 * 它做容器镜像里最典型的一件事：往可写数据盘写文件，再读回自证。
 * PASS/FAIL 由它自己 write(1) 打出来，宿主侧 grep 这一行。
 */
typedef unsigned long u64;

static long sys3(long nr, long a, long b, long c)
{
    long r = 0;

    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(nr), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

static long sys1(long nr, long a)
{
    return sys3(nr, a, 0, 0);
}

#define SYS_read  0
#define SYS_write 1
#define SYS_open  2
#define SYS_close 3
#define SYS_exit_group 231

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  64
#define O_TRUNC  512

static int slen(const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void quit(int code)
{
    (void)sys1(SYS_exit_group, (long)code);
    for (;;) {
        /* 不该回来 */
    }
}

static void say(const char *s)
{
    (void)sys3(SYS_write, 1, (long)s, (long)slen(s));
}

int _start(void)
{
    const char *path = "/data/out.txt";
    const char *msg = "written by the docker image program\n";
    char buf[64];
    long fd = 0;
    long n = 0;
    int len = slen(msg);

    fd = sys3(SYS_open, (long)path, (long)(O_CREAT | O_WRONLY | O_TRUNC), 0644);
    if (fd < 0) {
        say("FAIL: docker app: open /data/out.txt\n");
        quit(1);
    }
    n = sys3(SYS_write, fd, (long)msg, (long)len);
    if (n != (long)len) {
        say("FAIL: docker app: write\n");
        quit(1);
    }
    (void)sys1(SYS_close, fd);

    fd = sys3(SYS_open, (long)path, (long)O_RDONLY, 0);
    if (fd < 0) {
        say("FAIL: docker app: reopen\n");
        quit(1);
    }
    n = sys3(SYS_read, fd, (long)&buf[0], 32);
    (void)sys1(SYS_close, fd);
    if (n <= 0) {
        say("FAIL: docker app: read back\n");
        quit(1);
    }
    say("PASS: docker M11-3（镜像里的程序跑起来，写 /data/out.txt 并读回）\n");
    quit(0);
    return 0;
}
