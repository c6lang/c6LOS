/* 最小执行测试：只自旋，不碰栈/外设/USB。 */
    .section .text.start, "ax", @progbits
    .align 4
    .global _start
    .type _start, @function
_start:
    movi    a2, 0
1:
    addi    a2, a2, 1
    j       1b
