/* x86-64 SysV 协作上下文：保存 rbx/rbp/r12-r15 + rsp + 返回地址。 */
.globl port_task_switch
port_task_switch:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, g_sched_sp(%rip)
    movq %rdi, g_cur_ctx(%rip)
    movq (%rdi), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

.globl port_task_yield
port_task_yield:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq g_cur_ctx(%rip), %rax
    movq %rsp, (%rax)
    movq g_sched_sp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

.globl x86_tramp
x86_tramp:
    subq $8, %rsp
    movq g_cur_ctx(%rip), %rax
    movq 8(%rax), %rdx      /* entry */
    movq 16(%rax), %rdi     /* arg */
    call *%rdx
    call os_task_exit
1:
    hlt
    jmp 1b
