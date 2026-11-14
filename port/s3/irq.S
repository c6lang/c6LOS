/* Xtensa LX7 向量表 + level-1 定时器中断入口。
 * VECBASE 默认 0x40000000；内核异常向量在 +0x300，level-1 中断也从这里进。 */

    .section .vector, "ax", @progbits
    .balign 1024
    .global _vecbase
_vecbase:
    /* Xtensa 的向量表是每 0x40 一个槽。windowed ABI 下前面 6 个槽是寄存器窗口
     * 的溢出/欠载异常（寄存器文件只有 4 个窗口，调用链一深就会溢出，必须靠这组
     * 向量把窗口 spill 到栈上/从栈上 fill 回来）。call0 下它们不会被触发，
     * 放在这里没有副作用。0x300 是内核异常向量，level-1 中断也从这里进。 */
    .org 0x000
    j   _win_ovf4
    .org 0x040
    j   _win_unf4
    .org 0x080
    j   _win_ovf8
    .org 0x0C0
    j   _win_unf8
    .org 0x100
    j   _win_ovf12
    .org 0x140
    j   _win_unf12
    .org 0x180
    j   _vec_default            /* level-2 */
    .org 0x1C0
    j   _vec_default            /* level-3 */
    .org 0x200
    j   _vec_default            /* level-4 */
    .org 0x240
    j   _vec_default            /* level-5 */
    .org 0x280
    j   _vec_default            /* level-6 */
    .org 0x2C0
    j   _vec_default            /* level-7 / NMI */
    .org 0x300
    j   _vec_level1             /* kernel exception / level-1 中断 */
    .org 0x340
    j   _vec_default            /* user exception */
    .org 0x380
    j   _vec_default            /* double exception */
    .org 0x3C0
    j   _vec_default


/* ---- 寄存器窗口溢出/欠载（windowed ABI 必需）------------------------------
 * 语义见 Xtensa ISA：溢出时把被调用者的 a0-a7/a0-a11 spill 到它的栈帧里，
 * 然后 rfwo 回到出错点重试；欠载时反过来 fill 再 rfwu。
 * 这几个处理体不能改寄存器（除规定的那几个），否则栈帧内容会错。
 */
    .align 4
_win_ovf4:
    s32e    a0, a5, -16
    s32e    a1, a5, -12
    s32e    a2, a5,  -8
    s32e    a3, a5,  -4
    rfwo

    .align 4
_win_unf4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5,  -8
    l32e    a3, a5,  -4
    rfwu

    .align 4
_win_ovf8:
    s32e    a0, a9, -16
    l32e    a0, a1, -12
    s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo

    .align 4
_win_unf8:
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9,  -8
    l32e    a7, a1, -12
    l32e    a3, a9,  -4
    l32e    a4, a7, -32
    l32e    a5, a7, -28
    l32e    a6, a7, -24
    l32e    a7, a7, -20
    rfwu

    .align 4
_win_ovf12:
    s32e    a0,  a13, -16
    l32e    a0,  a1,  -12
    s32e    a1,  a13, -12
    s32e    a2,  a13,  -8
    s32e    a3,  a13,  -4
    s32e    a4,  a0,  -48
    s32e    a5,  a0,  -44
    s32e    a6,  a0,  -40
    s32e    a7,  a0,  -36
    s32e    a8,  a0,  -32
    s32e    a9,  a0,  -28
    s32e    a10, a0,  -24
    s32e    a11, a0,  -20
    rfwo

    .align 4
_win_unf12:
    l32e    a0,  a13, -16
    l32e    a1,  a13, -12
    l32e    a2,  a13,  -8
    l32e    a11, a1,  -12
    l32e    a3,  a13,  -4
    l32e    a4,  a11, -48
    l32e    a5,  a11, -44
    l32e    a6,  a11, -40
    l32e    a7,  a11, -36
    l32e    a8,  a11, -32
    l32e    a9,  a11, -28
    l32e    a10, a11, -24
    l32e    a11, a11, -20
    rfwu

    .section .text, "ax", @progbits
    .align 4
_vec_exc:
    mov     a2, a1
    call0   s3_trap
1:
    j       1b

    .align 4
_vec_default:
    /* 未处理向量/双重异常现场存进 RTC 慢速内存（CPU 复位后不丢，esptool
     * read-mem 0x50000100 起 16 字节即可读到）：
     *   +0x100 标记 0xBAD0 / +0x104 EXCCAUSE / +0x108 EXCVADDR / +0x10C EPC1
     * 注意别往 DRAM 写（原来写的 0x3FCF0004 已经超出 DRAM 末端 0x3FCEF000，
     * 一写就是二次异常，什么线索都留不下来）。 */
    movi    a2, 0x50000100
    movi    a3, 0x0000BAD0
    s32i    a3, a2, 0
    rsr     a3, exccause
    s32i    a3, a2, 4
    rsr     a3, excvaddr
    s32i    a3, a2, 8
    rsr     a3, epc1
    s32i    a3, a2, 12
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

    /* level-1 定时器中断的 EXCCAUSE 为 4；其它异常走陷阱打印。 */
    rsr     a3, exccause
    movi    a4, 4
    bne     a3, a4, _vec_exc

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

    .section .text, "ax", @progbits
    .align 4
    .global s3_test_trap
    .type   s3_test_trap, @function
s3_test_trap:
    ill

    .global port_irq_save
    .type   port_irq_save, @function
port_irq_save:
    rsil    a2, 15
    ret

    .global port_irq_restore
    .type   port_irq_restore, @function
port_irq_restore:
    wsr     a2, ps
    rsync
    ret
