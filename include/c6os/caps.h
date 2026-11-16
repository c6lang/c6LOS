#ifndef C6OS_CAPS_H
#define C6OS_CAPS_H

/* 移植层能力位。
 *
 * 用途：personality（如 Linux ELF 二进制兼容层）在**构建期**按位门禁，
 * 而不是运行期才发现硬件不支持。详见 docs/linux-abi.md §2。
 *
 * 规则：能力位是 port 的编译期属性，由构建系统用
 *   -DC6OS_PORT_CAPS=<bitmask>
 * 声明；默认 0 表示"纯 RTOS，无任何隔离能力"。
 */

#define C6OS_CAP_MMU      (1u << 0) /* 可切换的地址空间（能换页表根） */
#define C6OS_CAP_USER     (1u << 1) /* 用户态/内核态特权边界 + 系统调用陷入 */
#define C6OS_CAP_IRQ_USER (1u << 2) /* 用户态发生中断/异常时的换栈与返回 */
#define C6OS_CAP_SMP      (1u << 3)
#define C6OS_CAP_FPU      (1u << 4)

/* Linux ABI personality 的完整前提：MMU 隔离 + 特权边界 + 用户态可被打断。
 * 注意：只有 MMU 没有 USER（典型 MPU）做不出 syscall 边界；
 *       只有 USER 没有 MMU 只能单进程。两者都有才是宿主。 */
#define C6OS_CAP_LINUX_ABI (C6OS_CAP_MMU | C6OS_CAP_USER | C6OS_CAP_IRQ_USER)

#ifndef C6OS_PORT_CAPS
#define C6OS_PORT_CAPS 0u
#endif

/* 内核与 port 统一用这个名字查询；是宏而不是函数，保证
 * C6OS_LINUX_ABI=0 时不产生任何代码与符号。 */
#define port_caps() (C6OS_PORT_CAPS)

#endif
