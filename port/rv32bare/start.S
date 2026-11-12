/* risc6 bare 模式入口：无系统调用，栈在 .bss。 */
    .section .bss
    .align 4
    .globl rv_bare_stack
rv_bare_stack:
    .space 16384 * 4

    .section .text.init,"ax",@progbits
    .globl _start
_start:
    csrr t1, mhartid
    la sp, rv_bare_stack
    li t0, 16384
    addi t1, t1, 1
    mul t0, t0, t1
    add sp, sp, t0
    andi sp, sp, -16
    call os_main
1:  j 1b

    .section .risc6_bare,"a",@progbits
    .byte 1
