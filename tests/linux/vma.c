/* M13-2b 验收：按需分页 / 栈向下增长 / munmap 真回收。
 *
 * 为什么用这些判据（而不是"能跑"）：
 *   - 256MB 匿名 mmap 必须成功：256MB 机器上急切分配必失败，成功即说明
 *     "只记账、碰了才给页"。
 *   - 未触碰区间 mprotect 两次必须成功：惰性页没有 PTE，旧实现会 ENOMEM。
 *   - munmap 后能立刻再映射同样大小：页真回 free list 了，不是只改了记账。
 *   - 4MB+ 深递归：初始栈只有 128KB，不靠栈 VMA 向下增长就会踩坏。
 */
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIG   (256u * 1024u * 1024u)
#define PAGE  4096u

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

static volatile unsigned g_sink;

/* 每层一个 volatile 4KB 页：编译器不能把它优化掉，也没法尾递归消除。 */
static int deep(int n, int acc)
{
    volatile unsigned char pad[PAGE];

    pad[0] = (unsigned char)(n & 0xff);
    pad[PAGE - 1u] = (unsigned char)(acc & 0xff);
    if (n <= 0) {
        g_sink += (unsigned)(pad[PAGE - 1u] | 1u);   /* 非零：证明真跑到了底 */
        return (int)pad[0];
    }
    return deep(n - 1, acc + 1) + (int)pad[PAGE - 1u];
}

int main(void)
{
    unsigned char *p;
    int sum;

    /* 1) 惰性映射：只记账 */
    p = mmap((void *)0, (size_t)BIG, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        say("FAIL: 256MB mmap（惰性映射没生效？）\n");
        return 1;
    }
    check(p[0] == 0u && p[BIG / 2u] == 0u && p[BIG - 1u] == 0u, "零页初值");
    p[0] = 0x5au;
    p[BIG / 2u] = 0x5bu;
    p[BIG - 1u] = 0x5cu;
    check(p[0] == 0x5au && p[BIG / 2u] == 0x5bu && p[BIG - 1u] == 0x5cu,
          "触碰页读写");

    /* 2) 未触碰区间改权限：不能因为没有 PTE 就 ENOMEM */
    if (mprotect(p + PAGE * 16u, PAGE * 4u, PROT_NONE) != 0) {
        check(0, "mprotect PROT_NONE（未触碰页）");
    }
    if (mprotect(p + PAGE * 16u, PAGE * 4u, PROT_READ | PROT_WRITE) != 0) {
        check(0, "mprotect 恢复（未触碰页）");
    }
    p[PAGE * 16u] = 0x77u;
    check(p[PAGE * 16u] == 0x77u, "mprotect 后写回");

    /* 3) msync 匿名页：Linux 语义是成功 */
    check(msync(p, PAGE, MS_ASYNC) == 0, "msync 匿名页");

    check(munmap(p, (size_t)BIG) == 0, "munmap 256MB");

    /* 4) 再映射一次：证明页真还回去了 */
    p = mmap((void *)0, (size_t)BIG, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != MAP_FAILED, "munmap 后可再映射 256MB");
    if (p != MAP_FAILED) {
        p[0] = 0x11u;
        check(p[0] == 0x11u, "再映射可写");
        (void)munmap(p, (size_t)BIG);
    }

    /* 5) 深递归：4.8MB 用户栈，初始只有 128KB */
    sum = deep(1200, 0);
    check(sum >= 0 && g_sink != 0u, "深递归（栈向下增长）");

    if (failures == 0) {
        say("PASS: vma\n");
        return 0;
    }
    say("FAIL: vma\n");
    return 1;
}
