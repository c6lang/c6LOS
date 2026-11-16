/* L2/L3 的验收二进制：普通 Linux 用户态程序，由宿主 gcc 静态链接，
 * 内核侧不做任何改造 —— 它就是"未修改的 Linux ELF 二进制"。
 *
 * startup 会用到：brk / arch_prctl / set_tid_address / set_robust_list /
 * rseq / prlimit64 / readlinkat / getrandom / mprotect / write / exit_group。 */

#include <unistd.h>

static const char msg[] = "[user] hi from an unmodified Linux x86-64 ELF\n";

int main(void)
{
    write(1, msg, sizeof(msg) - 1);
    return 0;
}
