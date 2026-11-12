/* IRQ0 抢占：保存完整陷阱帧；需要抢占时把帧当作任务上下文切回调度器。 */
.globl irq0_stub
irq0_stub:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq $irq_resume
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    call x86_irq_handler
    testl %eax, %eax
    jz 1f
    movq g_cur_ctx(%rip), %rdx
    testq %rdx, %rdx
    jz 1f
    movq %rsp, (%rdx)
    movq g_sched_sp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    movl $0, g_in_task(%rip)
    sti
    ret
1:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    movl $0, g_in_task(%rip)
    sti
    ret
irq_resume:
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
    iretq

.globl port_irq_save
port_irq_save:
    pushfq
    popq %rax
    cli
    ret

.globl port_irq_restore
port_irq_restore:
    testl $0x200, %edi
    jz 1f
    sti
1:
    ret
