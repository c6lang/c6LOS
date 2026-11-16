/* L5 验收：fork + execve + pipe + wait4，全部由**未修改的 busybox sh** 驱动。
 *
 * 只用 sh 的内建命令（echo / read），不依赖 initramfs 里有没有 applet 符号链接。
 * 内核侧看到的是：sh 自己 fork 出管道两端、两边都 execve 回 /bin/busybox。
 */
#include <unistd.h>

int main(void)
{
    execl("/bin/busybox", "sh", "-c",
          "echo hello-from-pipe | while read l; do echo GOT:$l; done",
          (char *)0);
    write(1, "exec FAIL\n", 10);
    return 1;
}
