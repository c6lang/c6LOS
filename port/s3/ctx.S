/* ESP32-S3 call0 上下文：统一用 80 字节陷阱帧保存 a0/a2..a15 + EPC + PS。
 * S3Ctx: r[0..15]@0..60（未用）、sp@64（指向帧）、stack@68、size@72。 */

    .section .text.ctx, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lg_sched, g_sched
    .literal .Lg_cur,   g_s3_cur
    .literal .Lprid_base, g_prid_base
    .literal .Lrtc_dbg, 0x50000A00
    .literal .Lrtc_sw,  0x50001000   /* switch 计数 */
    .literal .Lrtc_yl,  0x50001008   /* yield 计数 */
    .literal .Lsw_cnt,  g_sw_cnt
    .literal .Lts_cnt,  g_ts_cnt
    .literal .Ldbg2,    0x50001800
    .literal .Lwscratch, g_win_scratch
    .literal .Lyl_cnt,  g_yl_cnt

/* 把当前帧（a1 指向）恢复：EPC/PS + a0/a2..a15，然后 rfe。 */
    .macro  RESTORE_FRAME
    /* WINDOWSTART 恢复策略（混合版，§246）：
     *
     *   - **刚建的任务**（帧偏移 68 = 0，`port_task_ctx_create` 只清 17 个字）：
     *     归一化成 `1<<WINDOWBASE`，即"只有当前窗口 live"，交给它从零开始轮转；
     *   - **被切走的任务**：写回它自己保存的 WINDOWSTART 原值。
     *
     * 为什么必须按任务保存：WINDOWBASE 是**全局**寄存器，而别的任务（闭源库的
     * windowed 代码）会做 call8/call12 把 WINDOWBASE 搬走。如果恢复时"归一化成
     * 1<<当前base"，刚恢复的 a0..a15 就落到了错的窗口，`retw` 的 CALLINC 与
     * WINDOWBASE 不再配套 —— 真板症状就是 `retw.n` 直接 cause=0、
     * a0 变成栈地址（§242/§243：`osi_semphr_take` 结尾）。
     * 写回原值则会连 WINDOWBASE 一起复原（硬件由 WINDOWSTART 推导 WINDOWBASE）。
     *
     * 顺序：必须在恢复 a0..a15 **之前**做，因为这里要借用 a2/a3/a4/a5；
     * 它们稍后会被从帧里重新装载。PS.WOE=0 时不要碰窗口状态。 */
    l32i    a2, a1, 64
    extui   a3, a2, 18, 1
    beqz    a3, 8f
    /* 纯归一化（配合壳里 spill 的成对语义）*/
    rsr     a4, windowbase
    movi     a5, 1
    ssl     a4
    sll     a5, a5
    wsr     a5, windowstart
    isync
8:
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
    .endm

    .global port_task_switch
    .type   port_task_switch, @function
port_task_switch:
    /* 这里**不** spill：spill 放在 w2c 的壳里做。
     * 原因（本轮定死）：窗口溢出槽位落在"被 spill 的那个窗口的调用者的
     * 栈指针下方"，而 port_task_switch 运行在我们自己的 call0 内核栈上，
     * 那些字节正是内核活着的局部量 —— 在这里 spill 就是踩内核。壳里 sp 还是
     * 壳自己的帧基址、下面全是空闲栈，槽位天然落在自由区。
     * 另外 `call8 xthal_window_spill` 的契约是"溢出调用者的调用者且不返回"，
     * 用它再往下接自己的代码一定会读到另一个窗口的垃圾（§200 三轮失败的原因）。 */
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
    /* EPC 槽存的是恢复后的执行地址。a0 在 windowed 调用中是**带 CALLINC
     * 标记**的返回地址（高 2 位，如 0x82xxxxxx）；直接存进 EPC 再 rfe，
     * CPU 会去取指这个带标记的地址 -> InstrFetchProhibited（真板症状：
     * pc==vaddr==a0==0x820328e4，cause=0x14）。所以先清掉高 2 位。
     * 真实调用链的 CALLINC 由 PS 槽（偏移 64）保留，retw 仍能正确回退。 */
    slli    a15, a0, 2
    srli    a15, a15, 2          /* a15 = a0 & 0x3FFFFFFF（去掉 CALLINC） */
    movi    a14, 1
    slli    a14, a14, 30         /* 0x40000000：代码段基址高位 */
    or      a15, a15, a14
    s32i    a15, a1, 60
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64
    /* 保存 WINDOWSTART（帧偏移 68），恢复时按任务各自的窗口状态还原 */
    rsr     a5, windowstart
    s32i    a5, a1, 68
    /* 诊断：yield 计数（0x50001008=次数，0x5000100c=最近的目标 sp） */
    l32r    a14, .Lyl_cnt
    l32i    a15, a14, 0
    addi    a15, a15, 1
    s32i    a15, a14, 0
    l32r    a14, .Lrtc_yl
    s32i    a15, a14, 0
    /* 诊断：switch 计数（0x50001000=次数，0x50001004=最近的目标 sp） */
    l32r    a14, .Lsw_cnt
    l32i    a15, a14, 0
    addi    a15, a15, 1
    s32i    a15, a14, 0
    l32r    a14, .Lrtc_sw
    s32i    a15, a14, 0

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
    /* 诊断：switch 的外出 sp（主循环）与目标 ctx */
    l32r    a14, .Ldbg2
    s32i    a1, a14, 0
    l32i    a4, a1, 4               /* 目标任务 ctx（参数先前存在帧里） */
    s32i    a4, a14, 4
    l32r    a3, .Lg_cur
    slli    a5, a12, 2
    add     a3, a3, a5
    s32i    a4, a3, 0
    l32i    a1, a4, 64
    l32r    a14, .Ldbg2
    s32i    a1, a14, 8              /* 恢复出来的目标 sp */
    RESTORE_FRAME

    .global port_task_yield
    .type   port_task_yield, @function
port_task_yield:
    /* 诊断：入口计数（Ldbg2+28），区分"没被调用"与"调用后空指针早退" */
    l32r    a14, .Ldbg2
    l32i    a15, a14, 28
    addi    a15, a15, 1
    s32i    a15, a14, 28
    s32i    a0, a14, 32          /* 调用者的返回地址（call0：a0 就是返回地址） */
    /* 诊断：直接读 g_s3_cur 数组本身与 [0]，与 block_prepare 里 port_cur_ctx()
     * 读到的值对照，判断是不是同一个数组、以及是不是被清过。 */
    l32r    a15, .Lg_cur
    s32i    a15, a14, 36         /* 数组地址 */
    l32i    a15, a15, 0
    s32i    a15, a14, 40         /* g_s3_cur[0] */
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
    /* EPC 槽存的是恢复后的执行地址。a0 在 windowed 调用中是**带 CALLINC
     * 标记**的返回地址（高 2 位，如 0x82xxxxxx）；直接存进 EPC 再 rfe，
     * CPU 会去取指这个带标记的地址 -> InstrFetchProhibited（真板症状：
     * pc==vaddr==a0==0x820328e4，cause=0x14）。所以先清掉高 2 位。
     * 真实调用链的 CALLINC 由 PS 槽（偏移 64）保留，retw 仍能正确回退。 */
    slli    a15, a0, 2
    srli    a15, a15, 2          /* a15 = a0 & 0x3FFFFFFF（去掉 CALLINC） */
    movi    a14, 1
    slli    a14, a14, 30         /* 0x40000000：代码段基址高位 */
    or      a15, a15, a14
    s32i    a15, a1, 60
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64
    /* 保存 WINDOWSTART（帧偏移 68），恢复时按任务各自的窗口状态还原 */
    rsr     a5, windowstart
    s32i    a5, a1, 68
    s32i    a1, a4, 64
    /* 诊断：yield 的外出任务 ctx 与保存的 sp */
    l32r    a14, .Ldbg2
    s32i    a4, a14, 16
    s32i    a1, a14, 20

    l32r    a3, .Lg_sched
    movi    a6, 76
    mull    a5, a11, a6
    add     a3, a3, a5
    l32i    a1, a3, 64
    mov     a4, a3
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
    /* 诊断：记录任务启动次数 / 入口地址 / 该任务起始 sp（RTC 0x50000A00 起）。
     * **注意用 a8/a9，绝不能碰 a4** —— a4 里是入口的 ABI 标志（0=call0，
     * 1=windowed），原来这里用 a4 当计数器，等于把标志覆盖成了启动次数，
     * 于是所有任务都走 windowed 分支（call0 任务被 callx8 调，返回地址带
     * CALLINC 标记）——这正是 §237 里"新任务一跳就 TRAP 到地址 0"的元凶。 */
    l32r    a3, .Lts_cnt
    l32i    a8, a3, 0
    addi    a8, a8, 1
    s32i    a8, a3, 0
    l32r    a3, .Lrtc_dbg
    s32i    a8, a3, 0
    s32i    a12, a3, 4
    s32i    a1, a3, 8
    /* 入口地址也按序号存一份，便于确认是不是同一个入口 */
    extui   a9, a8, 0, 2
    slli    a9, a9, 2
    add     a9, a3, a9
    s32i    a12, a9, 16
    /* a4 是入口的 ABI 标志（由 port_task_ctx_create 从 g_next_task_windowed 写入）：
     *   0 -> 我们自己的 call0 任务，用 callx0
     *   1 -> 闭源库的 windowed 任务（如 ppTask），必须先置 PS.WOE 再 callx8，
     *        否则被调方的 entry 不会轮转窗口、retw 会用错窗口。 */
    beqz    a4, 2f
    rsr     a6, ps
    movi    a7, 1
    slli    a7, a7, 18
    or      a6, a6, a7
    wsr     a6, ps
    rsync
    callx8  a12
    call0   os_task_exit
    j       1f
2:
    callx0  a12
    call0   os_task_exit
1:  j       1b
