/* ESP32-S3 call0 上下文：统一用 80 字节陷阱帧保存 a0/a2..a15 + EPC + PS。
 * S3Ctx: r[0..15]@0..60（未用）、sp@64（指向帧）、stack@68、size@72。 */

    .section .text.ctx, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lg_sched, g_sched
    .literal .Lg_cur,   g_s3_cur
    .literal .Lprid_base, g_prid_base
    .literal .Lrtc_dbg, 0x50000A00

/* 把当前帧（a1 指向）恢复：EPC/PS + a0/a2..a15，然后 rfe。 */
    .macro  RESTORE_FRAME
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
#ifdef __XTENSA_CALL0_ABI__
    rfe
#else
    /* windowed：只让当前窗口 live，祖先窗口由欠载向量按需 fill 回来 */
    movi    a2, 1
    wsr     a2, windowstart
    isync
    rfe
#endif
    .endm

    .global port_task_switch
    .type   port_task_switch, @function
port_task_switch:
#ifndef __XTENSA_CALL0_ABI__
    /* windowed ABI：先把祖先窗口全部 spill 到当前任务的栈上，
     * 之后只保存当前窗口的 a0..a15 就够了（恢复时靠欠载向量 fill）。 */
    movi    a10, 0
    call8   xthal_window_spill
#endif
    /* 双核各用 g_sched[core]/g_s3_cur[core]。
     *
     * 顺序很关键：**必须先把现场（a0..a15/EPC/PS）原样压栈**，再去算本核
     * 下标——算下标要用 a3/a5/a6/a12 这些寄存器，先动它们会把被打断任务的
     * 寄存器值毁掉（现象是随机的 TRAP，如 cause 28）。
     *
     * 核号不能问 port_core_id()：切换路径上一次 `callx0` 进 C 函数不转窗口，
     * 会把 a12-a15 冲掉；这里直接读 PRID 和 g_prid_base[0] 比较（PRID 本身
     * 在这颗 S3 上是脏值，但两核不同且稳定）。 */
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
    s32i    a0, a1, 60
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64

    /* 现场已保存，下面随便用寄存器 */
    rsr     a5, prid
    l32r    a3, .Lprid_base
    l32i    a6, a3, 0
    movi    a12, 0
    beq     a5, a6, 1f
    movi    a12, 1
1:
    movi    a6, 76
    mull    a5, a12, a6
    l32r    a3, .Lg_sched
    add     a3, a3, a5
    s32i    a1, a3, 64

    l32i    a4, a1, 4               /* 目标任务 ctx（参数先前存在帧里） */
    l32r    a3, .Lg_cur
    slli    a5, a12, 2
    add     a3, a3, a5
    s32i    a4, a3, 0
    l32i    a1, a4, 64
    RESTORE_FRAME

    .global port_task_yield
    .type   port_task_yield, @function
port_task_yield:
    rsr     a5, prid
    l32r    a3, .Lprid_base
    l32i    a6, a3, 0
    movi    a11, 0
    beq     a5, a6, 2f
    movi    a11, 1
2:
    l32r    a4, .Lg_cur
    slli    a5, a11, 2
    add     a4, a4, a5
    l32i    a4, a4, 0
    beqz    a4, 1f
#ifndef __XTENSA_CALL0_ABI__
    /* windowed ABI：先把祖先窗口全部 spill 到当前任务的栈上，
     * 之后只保存当前窗口的 a0..a15 就够了（恢复时靠欠载向量 fill）。 */
    movi    a10, 0
    call8   xthal_window_spill
#endif

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
    s32i    a0, a1, 60
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64
    s32i    a1, a4, 64

    l32r    a3, .Lg_sched
    movi    a6, 76
    mull    a5, a11, a6
    add     a3, a3, a5
    l32i    a1, a3, 64
    RESTORE_FRAME
1:
#ifdef __XTENSA_CALL0_ABI__
    ret
#else
    retw
#endif

    .global port_ps_get
    .type   port_ps_get, @function
port_ps_get:
    rsr     a2, ps
#ifdef __XTENSA_CALL0_ABI__
    ret
#else
    retw
#endif

    .weak os_task_exit
    .global s3_task_tramp
    .type   s3_task_tramp, @function
s3_task_tramp:
    /* 初始帧把 entry 放在 a2、arg 放在 a3。 */
    mov     a12, a2
    mov     a2, a3
    /* 诊断：记录任务启动次数 / 入口地址 / 该任务起始 sp（RTC 0x50000A00 起） */
    l32r    a3, .Lrtc_dbg
    l32i    a4, a3, 0
    addi    a4, a4, 1
    s32i    a4, a3, 0
    s32i    a12, a3, 4
    s32i    a1, a3, 8
    /* 入口地址也按序号存一份，便于确认是不是同一个入口 */
    extui   a5, a4, 0, 2
    slli    a5, a5, 2
    add     a5, a3, a5
    s32i    a12, a5, 16
#ifdef __XTENSA_CALL0_ABI__
    callx0  a12
    call0   os_task_exit
#else
    callx8  a12
    call8   os_task_exit
#endif
1:  j       1b
