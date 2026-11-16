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
    j   _vec_lvl2
    .org 0x1C0
    j   _vec_lvl3
    .org 0x200
    j   _vec_lvl4
    .org 0x240
    j   _vec_lvl5
    .org 0x280
    j   _vec_lvl6
    .org 0x2C0
    movi a3, 0x07               /* §290 标记：向量 11 = level-7/NMI */
    j   _vec_default
    .org 0x300
    j   _vec_level1             /* kernel exception / level-1 中断 */
    .org 0x340
    movi a3, 0x0D               /* §290 标记：向量 13 = user exception */
    j   _vec_default
    .org 0x380
    movi a3, 0x0E               /* §290 标记：向量 14 */
    j   _vec_default            /* double exception */
    .org 0x3C0
    movi a3, 0x0F               /* §290 标记：向量 15 = double exception */
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
    bnez    a0, 1f                  /* §292：上一级 sp 为 0 就回退到本帧 */
    mov     a0, a9
1:  s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo

    .align 4
_win_unf8:
    /* §299 诊断：**进入处理体时**的 a9 就是那个窗口的 sp。如果高字节是 0
     * （0x00000000~0x00FFFFFF，不可能是 DRAM/flash），说明窗口里的 a1 本身
     * 被写成了小整数。记进 RTC 0x50000160 再继续跑（a2/a3 马上会被 l32e
     * 重新装载，这里借用不影响恢复）。 */
    extui   a2, a9, 24, 8
    bnez    a2, 4f
    movi    a3, 0x50
    slli    a3, a3, 24
    s32i    a9, a3, 0x160
    s32i    a2, a3, 0x164
4:
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    bnez    a1, 1f
    mov     a1, a9
1:  l32e    a2, a9,  -8
    l32e    a7, a1, -12
    bnez    a7, 2f
    mov     a7, a9
2:  l32e    a3, a9,  -4
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

/* level-2..6 的中断：闪背光 N 下（N = 等级）后停住。
 * 这样"闭源库触发了哪个等级的中断"就能用眼睛看出来。 */
    .macro  VEC_LVL n
    .align  4
_vec_lvl\n:
    movi    a2, \n
    j       _vec_lvl_common
    .endm

    .section .text, "ax", @progbits
    .align 4
    VEC_LVL 2
    VEC_LVL 3
    VEC_LVL 4
    VEC_LVL 5
    VEC_LVL 6

    .align  4
_vec_lvl_common:
    /* a2 = 等级。背光在 GPIO13：先配 IO_MUX/OUT_SEL/输出使能。 */
    movi    a3, 0x60009034          /* IO_MUX(13) = 0x60009000+4+4*13 */
    l32i    a4, a3, 0
    movi    a5, -28673              /* 清 MCU_SEL(14:12) */

    and     a4, a4, a5
    movi    a5, 0x1000              /* MCU_SEL = 1 (GPIO) */
    or      a4, a4, a5
    s32i    a4, a3, 0
    movi    a3, 0x60004554          /* GPIO_OUT_SEL(13) = 0x60004000+0x554+4*13 */
    movi    a4, 0x100               /* 256 = 普通 GPIO */
    s32i    a4, a3, 0
    movi    a3, 0x60004024          /* GPIO_ENABLE_W1TS */
    movi    a4, 0x2000
    s32i    a4, a3, 0

    movi    a6, 0                   /* 已闪次数 */
1:
    movi    a3, 0x60004008          /* OUT_W1TS */
    s32i    a4, a3, 0
    movi    a7, 0
2:
    addi    a7, a7, 1
    movi    a3, 5000000
    bltu    a7, a3, 2b
    movi    a3, 0x6000400C          /* OUT_W1TC */
    s32i    a4, a3, 0
    movi    a7, 0
3:
    addi    a7, a7, 1
    movi    a3, 5000000
    bltu    a7, a3, 3b
    addi    a6, a6, 1
    bltu    a6, a2, 1b
4:
    j       4b

    .section .text, "ax", @progbits
    .align 4
_vec_exc:
    /* §297：**先**把 level-1 异常的现场存进 RTC 0x50000140。
     * 因为 s3_trap 自己要用 ets_printf 打印，打印过程中若再出异常（二次异常），
     * 0x50000100 的记录会被第二次覆盖，第一次的线索就丢了。 */
    movi    a2, 0x50
    slli    a2, a2, 24
    addi    a2, a2, 0x140
    rsr     a3, exccause
    s32i    a3, a2, 0
    rsr     a3, excvaddr
    s32i    a3, a2, 4
    rsr     a3, epc1
    s32i    a3, a2, 8
    rsr     a3, ps
    s32i    a3, a2, 12
    s32i    a1, a2, 16
    l32i    a3, a1, 0
    s32i    a3, a2, 20
    rsr     a3, windowbase
    s32i    a3, a2, 24
    rsr     a3, windowstart
    s32i    a3, a2, 28
#ifdef __XTENSA_CALL0_ABI__
    mov     a2, a1
    call0   s3_trap
#else
    mov     a10, a1              /* windowed ABI：参数在 a10~a15 */
    call8   s3_trap
#endif
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
    s32i    a3, a2, 36          /* §290 +0x124：进的是哪个 default 槽位 */
    movi    a3, 0x0000BAD0
    s32i    a3, a2, 0
    rsr     a3, exccause
    s32i    a3, a2, 4
    rsr     a3, excvaddr
    s32i    a3, a2, 8
    rsr     a3, epc1
    s32i    a3, a2, 12
    /* §257 加的现场：陷阱时的 PS / a0（高 2 位是 CALLINC）/ a1 / WINDOWBASE /
     * WINDOWSTART。定位 windowed 的 retw 非法指令必须看这几个值。 */
    rsr     a3, ps
    s32i    a3, a2, 16
    s32i    a0, a2, 20
    s32i    a1, a2, 24
    rsr     a3, windowbase
    s32i    a3, a2, 28
    rsr     a3, windowstart
    s32i    a3, a2, 32
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

#ifdef __XTENSA_CALL0_ABI__
    mov     a2, a1
    call0   s3_timer_irq
    beqz    a2, 2f
    /* a2 = 目标任务 ctx，ctx->sp 指向它自己的陷阱帧。 */
    l32i    a1, a2, 64
    j       2f
#else
    /* windowed：进 C 之前必须把 PS 设成 INTLEVEL=1 | UM | WOE。
     * WOE=1 才能让 call8 正常分配新窗口（否则每次都走窗口溢出向量）。 */
    movi    a2, 0x40021          /* bit18=WOE, bit5=UM, INTLEVEL=1 */
    wsr     a2, ps
    rsync
    mov     a10, a1              /* 参数：陷阱帧指针 */
    call8   s3_timer_irq
    beqz    a10, 2f              /* 返回值也在 a10（callee 的 a2） */
    l32i    a1, a10, 64
    j       2f
#endif

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
#ifdef __XTENSA_CALL0_ABI__
    ret
#else
    retw
#endif

    .global port_irq_restore
    .type   port_irq_restore, @function
port_irq_restore:
    wsr     a2, ps
    rsync
#ifdef __XTENSA_CALL0_ABI__
    ret
#else
    retw
#endif
