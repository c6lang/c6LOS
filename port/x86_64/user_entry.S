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

/* ---- 原生 Linux 系统调用通道：syscall 指令（MSR_LSTAR）----
 *
 * 进入时：RAX=nr，RDI/RSI/RDX/R10/R8/R9=参数，RCX=返回 RIP，R11=用户 RFLAGS，
 *         RSP 仍是用户栈（syscall 不换栈），CS/SS 已由 STAR 换成内核段。
 * 返回：统一 iretq（理由见 docs/linux-abi.md §4.3）。
 */
.globl x86_syscall_entry
x86_syscall_entry:
    movq %rsp, x86_user_rsp(%rip)        /* 先记下用户栈 */
    movq x86_kern_stack(%rip), %rsp      /* 换到内核栈 */
    pushq x86_user_rsp(%rip)             /* 返回时要用 */
    pushq %rax
    pushq %rbx
    pushq %rcx                           /* 用户 RIP */
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11                           /* 用户 RFLAGS */
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi                      /* &LxRegs */
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
    popq %rax                            /* 系统调用返回值 */
    popq %r10                            /* 用户 RSP */
    pushq $0x23                          /* SS  = 用户数据 | 3 */
    pushq %r10
    pushq %r11                           /* RFLAGS（用户原值） */
    pushq $0x2b                          /* CS  = 用户代码 | 3 */
    pushq %rcx                           /* RIP */
    iretq

/* ---- 异常（0..31）：观测优先 ----------------------------------------------
 *
 * 没有这些桩，任何内核 trap 都会因为 IDT 里没有门而直接三重故障 ——
 * 在 qemu 里表现为"上一行日志之后整机消失"，是排查成本最高的一种失败。
 * 这里统一压一个帧交给 C：vec, err, rip, cs, rflags, rsp, ss。
 */
.section .text

.irp v, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
.globl x86_exc_\v
x86_exc_\v:
    pushq $0                          /* 无错误码，补一个占位 */
    pushq $\v
    jmp x86_exc_common
.endr

.irp v, 8,10,11,12,13,14,17,21,29,30
.globl x86_exc_\v
x86_exc_\v:
    pushq $\v                         /* CPU 已压入 err */
    jmp x86_exc_common
.endr

x86_exc_common:
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
    movq %cr2, %rsi
    call x86_exc_dump
1:
    cli
    hlt
    jmp 1b

.section .data
.align 8
.globl x86_exc_table
x86_exc_table:
.irp v, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .quad x86_exc_\v
.endr
