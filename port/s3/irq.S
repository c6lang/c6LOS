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
    j   _vec_user               /* 用户异常：Alloca / Syscall / 其它 */
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
    bltui   a5, 0x100, 2f
    s32e    a0, a5, -16
    s32e    a1, a5, -12
    s32e    a2, a5,  -8
    s32e    a3, a5,  -4
2:  rfwo

    .align 4
_win_unf4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5,  -8
    l32e    a3, a5,  -4
    rfwu

    .align 4
_win_ovf8:
    /* §505：不用 [a1-12] 链。真板 `a9` 有效（前四条 store 都过），
     * 但链上的 a0 是垃圾，第 5 条 store 就 StoreProhibited 双异常。
     * 8 个寄存器全部落到 [a9-32,a9) —— 被溢出那一帧的窗口保存区。 */
    bltui   a9, 0x100, 2f
    s32e    a0, a9, -16
    s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    s32e    a4, a9, -32
    s32e    a5, a9, -28
    s32e    a6, a9, -24
    s32e    a7, a9, -20
2:  rfwo

    .align 4
_win_unf8:
    /* §505：和新的 ovf8 布局镜像。 */
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9,  -8
    l32e    a3, a9,  -4
    l32e    a4, a9, -32
    l32e    a5, a9, -28
    l32e    a6, a9, -24
    l32e    a7, a9, -20
    rfwu

    .align 4
_win_ovf12:
    /* §505：同 ovf8，不用 [a1-12] 链。 */
    bltui   a13, 0x100, 2f
    s32e    a0,  a13, -16
    mov     a0, a13
1:  s32e    a1,  a13, -12
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
2:  rfwo

    .align 4
_win_unf12:
    /* §505：和新的 ovf12 布局镜像。 */
    l32e    a0,  a13, -16
    l32e    a1,  a13, -12
    l32e    a2,  a13,  -8
    l32e    a3,  a13,  -4
    l32e    a4,  a13, -48
    l32e    a5,  a13, -44
    l32e    a6,  a13, -40
    l32e    a7,  a13, -36
    l32e    a8,  a13, -32
    l32e    a9,  a13, -28
    l32e    a10, a13, -24
    l32e    a11, a13, -20
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
/* §342：用户异常向量。
 *
 * 以前这里直接 `j _vec_default`（记现场 + 死循环）。但 windowed ABI 下编译器
 * 会用 `movsp` 做动态栈分配，硬件抛 **Alloca 异常(cause=5)** —— 不处理就等于
 * 让被中断的 `movsp` 永远不完成，而且异常入口已经 `rotw -1` 过，
 * **WINDOWBASE 会停在奇数（真板实测 9）**，之后所有 `retw`/`entry` 全部错位
 * （表现就是 `[give]` 之后 a0 变成 PS 值、`retw.n` cause=0）。
 *
 * 处理体照抄 IDF 的 `_xt_alloca_exc`（components/xtensa/xtensa_vectors.S）：
 * 用 PS.OWB 修正窗口基址，再按原 a0 的 CALLINC 位跳到对应的欠载向量，
 * 由那条 `rfwu` 回到被打断的 `movsp` 上重试。 */
_vec_user:
    wsr     a0, excsave1        /* 硬件不会自动存 a0，alloca 处理体要用 */
    rsr     a0, exccause
    bnei    a0, 5, 1f
    j       _xt_alloca_exc
1:
    movi    a3, 0x0D            /* 其余异常：保持老的现场记录 + 停机 */
    j       _vec_default

    .align 4
_xt_alloca_exc:
    rsr     a0, windowbase
    rotw    -1                  /* 原 a0(WINDOWBASE) 落到 a4，a0-a3 变成草稿 */
    rsr     a2, ps
    extui   a3, a2, 8, 4        /* PS.OWB */
    xor     a3, a3, a4          /* 新旧基址的差异位 */
    rsr     a4, excsave1        /* 取回被打断的 a0（现在在 a4） */
    slli    a3, a3, 8
    xor     a2, a2, a3          /* 把 PS.OWB 改成新基址 */
    wsr     a2, ps
    rsync
    bbsi.l  a4, 31, 2f          /* 按 CALLINC 选欠载向量（分支够不着，用 j） */
    j       _win_unf4
2:  rotw    -1
    bbsi.l  a8, 30, 3f
    j       _win_unf8
3:  rotw    -1
    j       _win_unf12

_vec_exc:
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
    /* §499：纯诊断，不改执行路径。双重异常时 EPC1=第一次故障的 PC、
     * DEPC=第二次故障的 PC；后者能直接指出第一次异常的处理体崩在哪条指令。 */
    rsr     a3, depc
    s32i    a3, a2, 40
    rsr     a3, excsave1
    s32i    a3, a2, 44
    s32i    a13, a2, 48          /* §504：_win_ovf12 的 spill 基址就是 a13 */
1:
    j       1b

    .align 4
_vec_level1:
    /* §343：内核模式下 Alloca 异常(cause=5)也走**这个**向量（0x340 只在
     * PS.UM=1 时用），所以必须在这里就把它分出去 —— 否则 `movsp` 永远完不成，
     * 而且异常入口的 `rotw -1` 会把 WINDOWBASE 停在奇数（真板实测 9），
     * 之后所有寄存器都映射错（a1 取到别的物理寄存器、retw 全乱）。
     * 处理体要的是**没被动过**的寄存器现场，所以这里只借 EXCSAVE2 暂存 a3。 */
    wsr     a3, excsave2
    rsr     a3, exccause
    bnei    a3, 5, 1f
    rsr     a3, excsave2
    wsr     a0, excsave1        /* alloca 处理体从这里取回被打断的 a0 */
    j       _xt_alloca_exc
1:
    rsr     a3, excsave2
    /* §486（真 bug）：下面 §364 的栈区校验会**覆盖 a3**（`.Lstklo`/`.Lstkhi`
     * 两条 `l32r`），而帧保存在**之后**（偏移 8 存 a3）—— 于是帧里记下的 a3
     * 永远是 `.Lstkhi` = 0x3FCEF000，真正的 a3 丢了。
     *
     * §485 就被这个假现场带偏过：故障 EPC 正好也是 0x3fcef000，
     * 于是"a3 = 跳转目标"看起来成立了，其实两处同一个来源。
     *
     * EXCSAVE3 全仓库没人用（只有 excsave1/2 在用），借它暂存真值。 */
    wsr     a3, excsave3
    /* §361（真病根）：**中断帧不能再压在被中断的 sp 下面**！
     *
     * 原来这里是 `addi a1, a1, -80`，于是帧里的 PS 槽（帧+64 = 原 sp-16）
     * 正好压在 windowed ABI 的**溢出槽** `[sp-16]` 上 —— 那是被调用者保存
     * caller a0 的地方。中断一来就把 slot 写花，之后窗口欠载从那里把 a0
     * 填回来，a0 就变成了我们的 PS 值（真板现场 a0=0x00060400 ≈ PS
     * 0x00060600），紧接着 `retw.n` cause=0。
     *
     * 改法：切到**专用中断栈**。被打断的 sp 存进 EXCSAVE3，帧里新开一个
     * 槽位（偏移 68）保存它，退出时用它恢复 a1。 */
    /* §364 看门狗式检查：被打断的 sp 必须落在 DRAM 栈区。不落就说明**中断发生前**
     * 栈指针就已经是垃圾了，直接记录 (sp, epc1, ps, a0) 到 0x500018C0 并停住。 */
    l32r    a3, .Lstklo
    bltu    a1, a3, 11f
    l32r    a3, .Lstkhi
    bgeu    a1, a3, 11f
    j       12f
11:
    l32r    a3, .Lsprec
    s32i    a1, a3, 0
    s32i    a0, a3, 4
    rsr     a2, epc1
    s32i    a2, a3, 8
    rsr     a2, ps
    s32i    a2, a3, 12
    rsr     a2, windowbase
    s32i    a2, a3, 16
    rsr     a2, windowstart
    s32i    a2, a3, 20
    movi    a2, 0xBAD2
    s32i    a2, a3, 24
13: j       13b
12:
    /* 帧放在 [sp-128, sp-60)：**必须整体低于 sp-48**，那才是 windowed ABI
     * 溢出槽（call8: [sp-32,sp-20)，call12: [sp-48,sp-20)）的下界。
     * 这样中断帧不会盖住被调用者保存 caller a0..a11 的槽。 */
    rsr     a3, excsave3            /* §486：还原**真实** a3（上面校验把它覆盖了） */
    addi    a1, a1, -128
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
    /* §375（真 bug）：**偏移 68/72 必须写 WINDOWSTART/WINDOWBASE**。
     *
     * `port_irq_fill()` 会把这个帧记成任务的保存点，之后走
     * `RESTORE_FRAME`；而 RESTORE_FRAME 会 `l32i a3, a1, 68` 然后
     * `wsr windowstart`（非 0 时）。这里原来没写这两格，读出来就是**栈上的
     * 旧内存**。真板现场：`windowstart=0x00000200`、`windowbase=9`、
     * `a1=0x0008437f`（垃圾 sp）—— `a1` 就是被 `wsr windowstart` 之后的
     * 窗口重映射搞坏的。ctx.S 的保存路径一直写这两格，只有中断帧漏了。 */
    rsr     a2, windowstart
    s32i    a2, a1, 68
    rsr     a2, windowbase
    s32i    a2, a1, 72
    /* §372：帧自描述。本帧在 [sp-128, sp-4)，所以恢复后的 sp = 帧 + 128。
     * 以前这里没有这个槽，`RESTORE_FRAME` 一律 `addi a1, a1, 80`，而
     * `port_irq_fill` 恰好把 c->sp 记成这个 128 字节的帧 ⇒ 被中断过的任务
     * 恢复后 sp 低 48 字节。 */
    addi    a2, a1, 128
    s32i    a2, a1, 76

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
    /* §460 ★ 还原被中断任务的 WINDOWSTART / WINDOWBASE。
     *
     * 保存段（`irq.S:370-374`）一直在写帧偏移 68/72，但恢复段从来不还原 ——
     * 和 §457 在 `RESTORE_FRAME` 里补的是**同一个缺口**，中断这条路径漏了。
     * 若中断期间有 windowed 调用把 WINDOWBASE 搬走，返回时被中断的
     * windowed 函数 `retw` 就落到错的物理窗口上。
     *
     * 真板症状正是 `ram_chip_i2c_readReg` 里 `callx8` 跳到 `0x164` 去取指
     * （cause=0x1c = InstPIFAddrError），而且**下硬件断点就复现不了**
     * （连命 55 次全正常）—— 典型的时序竞态，只可能是中断插进来的。
     *
     * 两个顺序要求，和 §457 完全一样：
     *   1. **先 `wsr windowstart` 再 `wsr windowbase`** —— `wsr windowbase`
     *      会重映射 aN→物理寄存器，写完 `a2`/`a3` 里就不是原值了；
     *   2. 帧指针必须先藏进 `g_ctx_tmp_sp`，因为重映射会把 `a1` 也换掉。
     * `ctx.S` 那边这段在 `rsil 15` 里，中断上下文里本身就是 INTLEVEL≥1，
     * 定时器是 level-1、不会自我抢占，所以共用这个全局不会打架。 */
    l32i    a2, a1, 68
    beqz    a2, 44f                 /* 68 == 0 ⇒ 没写过，别碰窗口 */
    l32i    a3, a1, 72
    /* §471 ★ 和 ctx.S §470 完全相同的两道校验 —— 这里原来是**裸写**。
     *
     * Xtensa 硬不变式：WINDOWBASE < 16，且必须是 4 的倍数
     * （它同时是 WINDOWSTART 的右移量，要满足
     *  `(WINDOWSTART >> WINDOWBASE) & 1 == 1`）。写不合法的值，
     * **`wsr windowbase` 这条指令自己就进异常** —— 直接掉进
     * `_vec_default` 的 `j .` 死循环，现场全丢。
     *
     * §470 已在 `ctx.S` 的 `RESTORE_FRAME` 里加了同样的校验并实测命中 3 次
     * （`坏WB=0xa`），但 `_vec_default` 照样被进 —— 就是因为**这条路径**
     * 还在裸写。帧偏移 72 里如果是垃圾（`0xa`），这条 `wsr` 立刻炸。 */
    movi    a6, 16
    bgeu    a3, a6, 45f             /* WINDOWBASE >= 16 ⇒ 非法 */
    srli    a6, a2, 16
    bnez    a6, 45f                 /* WINDOWSTART 高位非 0 ⇒ 非法（只有低 16 位有意义） */
    l32r    a6, .Ltmpsp_i
    s32i    a1, a6, 0
    wsr     a2, windowstart
    isync
    wsr     a3, windowbase
    isync
    l32r    a6, .Ltmpsp_i
    l32i    a1, a6, 0
    j       44f
45:
    /* 非法：归一化到 WINDOWSTART=1 / WINDOWBASE=0（满足不变式的最保守状态），
     * 至少保证接下来的 `wsr` 不会再进异常，让中断能正常返回。 */
    l32r    a6, .Ltmpsp_i
    s32i    a1, a6, 0
    movi    a6, 1
    wsr     a6, windowstart
    isync
    movi    a6, 0
    wsr     a6, windowbase
    isync
    l32r    a6, .Ltmpsp_i
    l32i    a1, a6, 0
44:
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
    /* §372：sp 由帧自己给出（偏移 76）—— 本帧是 128 字节，但"从 ISR 切到
     * 另一个任务"时 a1 指向的是 port_task_switch 存的 80 字节帧，写死 128
     * 就会弹错。自加载写法两种情形都对（a1 是旧基址，a0..a15 已恢复好）。 */
    l32i    a1, a1, 76
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

    .section .text, "ax", @progbits
    .align  4
    .literal_position

    .section .text, "ax", @progbits
    .align  4
    .literal_position
    .literal .Lstklo,  0x3FCA0000
    .literal .Lstkhi,  0x3FCEF000
    .literal .Lsprec,  0x500018C0
    .literal .Ltmpsp_i, g_ctx_tmp_sp   /* §460：恢复窗口寄存器时藏帧指针 */
