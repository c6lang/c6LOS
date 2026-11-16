/* 用户态进入与系统调用陷入（L1：int 0x80 通道）。
 *
 * 返回用户态统一走 iretq，不用 sysret：
 *   1) sysret 遇到非规范 RCX 会 #GP，且是历史 CVE 温床；
 *   2) sysret 不恢复 SS:RSP，无法表达"返回时栈被信号改写"（信号投递必需）；
 *   3) 性能差异对兼容层目标可忽略。
 * 详见 docs/linux-abi.md §4.3。 */

.text

/* void port_user_enter(u64 as, u64 rip, u64 rsp, u64 arg)
 *   rdi=as  rsi=rip  rdx=rsp  rcx=arg  —— 不返回 */
.globl port_user_enter
port_user_enter:
    movq %rdi, %cr3              /* 切到进程地址空间 */
    movq %rcx, %rdi              /* 用户程序拿到 arg */
    pushq $0x23                  /* SS  = 用户数据 0x20 | 3 */
    pushq %rdx                   /* RSP */
    pushq $0x2                   /* RFLAGS：IF=0（L1 先屏蔽中断；L2 接 IRQ_USER） */
    pushq $0x2b                  /* CS  = 用户代码 0x28 | 3 */
    pushq %rsi                   /* RIP */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rbp, %rbp
    xorq %r8,  %r8
    xorq %r9,  %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    iretq

/* 中断门入口（DPL3）。CPU 已按 TSS.RSP0 换到内核栈并压入
 * SS / RSP / RFLAGS / CS / RIP，这里只负责保存通用寄存器。
 * 压栈顺序必须与 port/x86_64/linux.c 的 X86Regs 完全一致。 */
.globl x86_int80_entry
x86_int80_entry:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    call x86_syscall_dispatch
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
