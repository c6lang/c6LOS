/* Xtensa LX7 向量表 + level-1 定时器中断入口。
 * VECBASE 默认 0x40000000；内核异常向量在 +0x300，level-1 中断也从这里进。 */

    .section .vector, "ax", @progbits
    .balign 1024
    .global _vecbase
_vecbase:
    .rept 256
    j   _vec_default
    .endr
    j   _vec_level1          /* offset 0x300：kernel exception / level-1 */
    .rept 84
    j   _vec_default
    .endr

    .section .text, "ax", @progbits
    .align 4
_vec_default:
    movi    a2, 0x3FCF0004
    movi    a3, 0xBAD0
    s32i    a3, a2, 0
1:
    j       1b

    .align 4
_vec_level1:
    addi    a1, a1, -80
    s32i    a0, a1, 0
    s32i    a2, a1, 4
    s32i    a3, a1, 8
    s32i    a4, a1, 12
    s32i    a5, a1, 16
    s32i    a6, a1, 20
    s32i    a7, a1, 24
    s32i    a8, a1, 28
    s32i    a9, a1, 32
    s32i    a10, a1, 36
    s32i    a11, a1, 40
    s32i    a12, a1, 44
    s32i    a13, a1, 48
    s32i    a14, a1, 52
    s32i    a15, a1, 56
    rsr     a2, epc1
    s32i    a2, a1, 60
    /* level-1 没有 EPS1：PS 由软件保存，去掉 EXCM/INTLEVEL 后回写。 */
    rsr     a2, ps
    movi    a3, -32
    and     a2, a2, a3
    s32i    a2, a1, 64

    mov     a2, a1
    call0   s3_timer_irq
    beqz    a2, 2f
    /* a2 = 目标任务 ctx，ctx->sp 指向它自己的陷阱帧。 */
    l32i    a1, a2, 64
    j       2f

2:
    /* 恢复 a1 指向的陷阱帧并用 rfe 返回。 */
    l32i    a2, a1, 60
    wsr     a2, epc1
    l32i    a2, a1, 64
    movi    a3, 16
    or      a2, a2, a3
    wsr     a2, ps
    rsync
    l32i    a0, a1, 0
    l32i    a2, a1, 4
    l32i    a3, a1, 8
    l32i    a4, a1, 12
    l32i    a5, a1, 16
    l32i    a6, a1, 20
    l32i    a7, a1, 24
    l32i    a8, a1, 28
    l32i    a9, a1, 32
    l32i    a10, a1, 36
    l32i    a11, a1, 40
    l32i    a12, a1, 44
    l32i    a13, a1, 48
    l32i    a14, a1, 52
    l32i    a15, a1, 56
    addi    a1, a1, 80
    rfe
