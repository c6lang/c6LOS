/* ESP32-S3 call0 上下文：统一用 80 字节陷阱帧保存 a0/a2..a15 + EPC + PS。
 * S3Ctx: r[0..15]@0..60（未用）、sp@64（指向帧）、stack@68、size@72。 */

    .section .text.ctx, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lg_sched, g_sched
    .literal .Lrtc_cs,  0x50001E80
    .literal .Lg_cur,   g_s3_cur
    .literal .Lprid_base, g_prid_base
    .literal .Lrtc_dbg, 0x50000A00
    .literal .Lrec_rs,  0x50000400   /* §317：恢复点记录 */
    .literal .Lrec_sw,  0x50000500   /* §346：切换点记录 a0/EPC/PS/WS/WB。原地址 0x50000300 的 +0x10/+0x14 被 wifi_glue 诊断（0x310 存队列句柄）覆盖，读出来的 WS/WB 是假的 */
    .literal .Lrtc_sw,  0x50001000   /* switch 计数 */
    .literal .Lrtc_yl,  0x50001008   /* yield 计数 */
    .literal .Lsw_cnt,  g_sw_cnt
    .literal .Lts_cnt,  g_ts_cnt
    .literal .Ldbg2,    0x50001800
    .literal .Lwscratch, g_win_scratch
    .literal .Ltmpsp,   g_ctx_tmp_sp
    .literal .Lyl_cnt,  g_yl_cnt


/* §304：把当前所有祖先窗口整圈 spill 到各自栈帧（同 w2c.S 的 WIN_SPILL）。
 * 任务切换前必须先做：否则被切走任务的寄存器还压在物理窗口里，
 * 等它被调度回来时物理寄存器已经是别的任务的内容了。 */
    .macro  S3_SPILL_ALL
    rsr     a8, ps
    extui   a9, a8, 4, 1
    bnez    a9, 55f
    rsr     a9, epc1
    movi    a10, -32
    and     a10, a10, a8
    movi    a11, 1
    slli    a11, a11, 18
    addi    a11, a11, 7
    or      a10, a10, a11
    wsr     a10, ps
    rsync
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    4
    wsr     a8, ps
    rsync
    wsr     a9, epc1
55:
    .endm

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
    /* §258：**先把 WINDOWBASE 复原**。它是全局寄存器——别的任务的 windowed 调用
     * 会把基址搬走；不复原的话，被打断的 windowed 函数 retw 时"调用者的窗口"就
     * 落到了错误的物理寄存器（真板：cause=0 落在 retw.n、a0 变成栈地址）。
     *
     * 麻烦点在于 `wsr windowbase` 会把**所有虚拟寄存器**重新映射，包括 a1（帧指针）
     * 自己。所以先用当前映射把帧指针存进全局，换完基址再用 `l32r`（只需要一个字面量
     * 地址，不依赖任何寄存器）把它取回来。 */
    /* 整段恢复期间必须**关中断**：下面借全局 g_ctx_tmp_sp 传帧指针，
     * 中途被定时器抢占（甚至切任务）会把窗口状态彻底搅乱（§260：真板上表现
     * 为偶发 WS=0、a0=0、PC 跳进栈地址）。最后那句 `wsr ps` 会把 INTLEVEL
     * 恢复成帧里保存的值，所以这里直接拉到 15 就行。 */
    rsil    a5, 15
    /* §317：把**这次真正用来恢复的帧**记下来（4 项 x 8 字环）：
     * 序号、帧地址、EPC、PS、WS、WB。切回来时崩在 ROM 的 retw.n，
     * 先要弄清 rfe 到底跳去了哪里。 */
    l32r    a4, .Lrec_rs
    l32i    a6, a4, 0
    extui   a6, a6, 0, 2
    slli    a6, a6, 5
    add     a6, a6, a4
    s32i    a1, a6, 4
    l32i    a7, a1, 60
    s32i    a7, a6, 8
    l32i    a7, a1, 64
    s32i    a7, a6, 12
    l32i    a7, a1, 68
    s32i    a7, a6, 16
    l32i    a7, a1, 72
    s32i    a7, a6, 20
    l32i    a7, a4, 0
    addi    a7, a7, 1
    s32i    a7, a4, 0
    /* §404（照抄 IDF）：**不要主动写 WINDOWSTART / WINDOWBASE**。
     *
     * 这一块原来是 §296/§311 引入的：把帧里存的 WINDOWSTART 写回硬件，
     * 新建任务则归一化成 WB=0/WS=1。本轮把 IDF 的 `_xt_context_restore`
     * 和 `_xt_user_exit`（xtensa_context.S / xtensa_vectors.S）从头读了一遍
     * —— **它们一行都不写窗口状态寄存器**：
     *
     *     _xt_user_exit:
     *         l32i a0, sp, XT_STK_PS   ; wsr a0, PS
     *         l32i a0, sp, XT_STK_PC   ; wsr a0, EPC_1
     *         l32i a0, sp, XT_STK_A0
     *         l32i sp, sp, XT_STK_A1   ; rsync ; rfe
     *
     * 窗口状态的恢复**完全由 `rfe` 按 PS.OWB 完成**。我们额外做的那些
     * `wsr windowstart` 反而制造了 §402 实测到的非法组合
     *（`windowbase=9` 而 `windowstart=0x200`，bit 9 未置位）。
     *
     * 同时删除的还有"新建任务归一化"那段：新任务的初始 PS 里 OWB=0
     *（ctx.c 已清），`rfe` 会自己把 WB 设成 0，不需要我们插手。
     * 这也是本轮 §387/§388/§389/§391/§400/§403 六次"自己写 WB"全部失败的
     * 根本原因 —— 这条路本来就不该走。
     */
    /* §409：**新建任务的帧（WS==0）必须显式把窗口状态归一化**。
     *
     * §404 照抄 IDF 删掉了"新建任务归一化"的 `wsr windowbase`，因为
     * 已有任务的帧里 WS 是有效的、`rfe` 会按 PS.OWB 自己恢复。
     * 但**新建任务**的帧 WS==0（ctx.c 只清了 PS.OWB，没有 WS），
     * 此时 `rfe` 用 OWB=0 把 WB 设成 0，而硬件当前 WB 可能是 15
     *（真板实测恢复前 wb=15/ws=0x8000）—— 虽然 WB 归 0 是对的，
     * 但 WS 里残留的 `0x8000` 没被清掉，两者不配套，恢复出来的
     * a0..a15 仍落在错的窗口（真板：a1 变成 0xffffffe0）。
     *
     * 所以只在这一种情况（WS==0）下把它设成干净起点 `WS=1, WB=0`，
     * 与 PS.OWB=0 三者一致。已有任务的帧**完全不碰**（§404 的结论）。
     * 用 g_ctx_tmp_sp 藏帧指针（`wsr windowbase` 会重映射 a1），
     * 这一段在 `rsil 15` 关中断内、只存取一次，不会和别的路径打架。 */
    l32i    a3, a1, 68
    bnez    a3, 11f
    l32r    a4, .Ltmpsp
    s32i    a1, a4, 0
    movi    a3, 0
    wsr     a3, windowbase
    isync
    movi    a3, 1
    wsr     a3, windowstart
    isync
    l32r    a4, .Ltmpsp
    l32i    a1, a4, 0
11:
    /* §363 健全性闸门：EPC 必须落在合法代码区（ROM/IRAM 0x40000000-0x43FFFFFF）。
     * 不合法就说明"这个帧"是垃圾（错误 ctx / 已释放栈 / sp 差了几十字节），
     * 直接把帧和 EPC/PS/WS/WB/a0 记到 0x50001880 并原地停住，等 JTAG 看现场。
     * 以前是直接 `rfe` 跳进垃圾，现场全丢。 */
    l32i    a2, a1, 60
    l32r    a6, .Lepclo
    bltu    a2, a6, 8f
    l32r    a6, .Lepchi
    bgeu    a2, a6, 8f
    j       9f
8:
    l32r    a4, .Lbadrec
    s32i    a1, a4, 0
    s32i    a2, a4, 4
    l32i    a7, a1, 64
    s32i    a7, a4, 8
    l32i    a7, a1, 68
    s32i    a7, a4, 12
    l32i    a7, a1, 72
    s32i    a7, a4, 16
    l32i    a7, a1, 0
    s32i    a7, a4, 20
    l32i    a7, a1, 4
    s32i    a7, a4, 24
    movi    a7, 0xBAD1
    s32i    a7, a4, 28
9:
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
    /* §372：sp 由帧自己给出（偏移 76）。**必须用 `l32i a1, a1, 76` 这种
     * 自加载写法** —— 此时 a0..a15 都已经从帧里恢复好了，任何一个临时寄存器
     * 都会毁掉刚恢复的值。基址用的是旧 a1，所以一条指令就够。
     * 所有会产出帧的地方（port_task_switch / port_task_yield / _vec_level1 /
     * port_task_ctx_create）都写这个槽。 */
    /* §380：sp 闸门 —— 偏移 76 读出来的必须是 DRAM 栈区里的地址。
     * 不合理就记录 (帧, 该值, EPC, a0) 到 0x500018E0 并停住，
     * 而不是带着垃圾 sp 跑下去（真板症状：a1=0x0008437f 恰好是 flash text 大小）。 */
    l32i    a1, a1, 76
    /* §392：偏移 76 为 0 ⇒ 这是个**没写过自描述槽的旧帧**（例如
     * `s3_task_tramp` 之前某条老路径留下的）。直接记现场并停住，
     * 比拿 a1=0 继续跑好定位得多。 */
    beqz    a1, 83f
    l32r    a3, .Lstklo2
    bltu    a1, a3, 81f
    l32r    a3, .Lstkhi2
    bgeu    a1, a3, 81f
    rfe
81:
    l32r    a3, .Lbadsp
    s32i    a1, a3, 0
    s32i    a0, a3, 4
    s32i    a2, a3, 8
    rsr     a4, epc1
    s32i    a4, a3, 12
    movi    a4, 0xBAD3
    s32i    a4, a3, 16
82: j       82b
83:
    l32r    a3, .Lbadsp
    movi    a4, 0xBAD4
    s32i    a4, a3, 16
    s32i    a0, a3, 4
    s32i    a2, a3, 8
84: j       84b
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
    /* §379（真 bug）：**帧必须整体低于 sp-48**。
     *
     * windowed ABI 把 `[sp-48 .. sp-4]` 当作"被调用者把 caller 的
     * a0..a11 溢出到栈上"的槽位区（call4 用 -16..-4，call8 用 -32..-4，
     * call12 用 -48..-4）。原来这里 `addi a1, a1, -80`，帧占
     * `[sp-80, sp)`，**正好盖住整个槽位区** —— 我们保存的 a0..a15 与
     * 硬件溢出槽互相覆盖，`retw`/下溢时读到的就是别人的帧数据。
     * 改成 -128，帧占 `[sp-128, sp-48)`，与槽位区完全不相交。
     * （`_vec_level1` 早就用 -128 了，见 §361；这里漏改了。） */
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
    /* §372：**帧自描述** —— 偏移 76 存"恢复这个帧之后 sp 应该是多少"。
     *
     * 原来 RESTORE_FRAME 结尾写死 `addi a1, a1, 80`，而 §361 把中断帧改成了
     * 128 字节并且 `port_irq_fill` 恰好把 c->sp 记成那个中断帧 —— 于是任何
     * 被中断过的任务恢复时 sp 都会**低 48 字节**（踩在别人栈上，症状是垃圾
     * a1 与随机 TRAP）。帧大小写死在两处迟早还会错，所以改成每一帧自带这个
     * 数字，恢复方只负责读。 */
    addi    a14, a1, 128
    s32i    a14, a1, 76
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
    /* §316：把每次切换点的 a0 / 算出来的 EPC / PS / WS / WB 记进 RTC 环形区
     * （0x50000300=序号，0x50000304 起每项 16 字节），用来查"切回来时
     * EPC 为什么会是 ROM 里的 retw.n"。a11..a14 此时已经压进帧，可以随便用。 */
    l32r    a14, .Lrec_sw
    l32i    a13, a14, 0
    extui   a13, a13, 0, 3
    slli    a12, a13, 5
    add     a12, a12, a14
    s32i    a0,  a12, 4
    s32i    a15, a12, 8
    rsr     a11, ps
    s32i    a11, a12, 12
    rsr     a11, windowstart
    s32i    a11, a12, 16
    rsr     a11, windowbase
    s32i    a11, a12, 20
    l32i    a13, a14, 0
    addi    a13, a13, 1
    s32i    a13, a14, 0
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64
    /* 保存 WINDOWSTART（帧偏移 68）和 WINDOWBASE（帧偏移 72）。
     * **WINDOWBASE 也必须是每任务一份**（§258）：它是全局寄存器，别任务的
     * windowed 调用（call8/call12）会把基址搬来搬去；如果切回来时不复原，
     * 被打断的 windowed 函数 retw 时的"调用者窗口"就落到了错误的物理寄存器上
     * ——真板症状就是 cause=0 落在 retw.n、且 WINDOWSTART 与 CALLINC 对不上。 */
    rsr     a5, windowstart
    s32i    a5, a1, 68
    rsr     a5, windowbase
    s32i    a5, a1, 72
    /* §312：S3_SPILL_ALL 整圈 rotw 会触发窗口溢出 → 溢出处理体里
     * `s32e a0,a9,-16` 用到脏 a9，双重异常打死在 _vec_default。停用它。 */
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
    /* §429 ★★（真 bug）：**保存 sp 必须写进"当前任务"的 ctx**。
     *
     * 原来是存进 `g_sched[core]` —— 那是**一个** per-core 的固定槽，
     * 而任务实际用的是 `ctx.c` 里 `static S3Ctx g_ctx[]`（由
     * `port_task_ctx_create` 分配、`g_s3_cur[core]` 指向）。
     * `g_sched` 从来没人当任务的 ctx 用 ⇒ **被切走的任务，它的 `sp`
     * 一次都没有被保存过** ⇒ 恢复时永远读 `port_task_ctx_create` 造的那个
     * "新任务帧"（EPC = `s3_task_tramp`、`a1sl == top`）
     * ⇒ **任务每次被调度都从函数开头重新执行**。
     *
     * 真板症状（全都能对上）：
     *   - `[wifimain] id=1 tick=0` … 然后 `[wifimain] id=1 tick=16/30` 又打一次
     *     （同一个任务重新跑函数开头）
     *   - JTAG 读 `g_ctx[0]`：EPC = `s3_task_tramp`、`a1sl(+76) == sp == top`
     *     —— 明明在跑，帧却还是"从未启动"的样子
     *   - `TRAP pc=0x3fcef000`（返回地址被写坏）
     *
     * 现在改成先取 `g_s3_cur[core]` 指向的那个 ctx，再写它的 `sp`(偏移 64)。
     * 取不到（0）就跳过，别把 0 当指针写。 */
    l32r    a3, .Lg_cur
    slli    a5, a12, 2
    add     a3, a3, a5
    l32i    a3, a3, 0
    beqz    a3, 81f
    s32i    a1, a3, 64
    /* §433 诊断：记录每次 switch 保存的 (ctx, 帧sp) 与次数，写到 RTC
     * 0x50001D00（[0]=ctx [4]=sp [8]=次数）。用来分清"保存没发生"与
     * "保存了但恢复用错了 sp"。 */
    l32r    a6, .Lrtc_cs
    s32i    a3, a6, 0
    s32i    a1, a6, 4
    l32i    a5, a6, 8
    addi    a5, a5, 1
    s32i    a5, a6, 8
81:
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
    /* §379（真 bug）：**帧必须整体低于 sp-48**。
     *
     * windowed ABI 把 `[sp-48 .. sp-4]` 当作"被调用者把 caller 的
     * a0..a11 溢出到栈上"的槽位区（call4 用 -16..-4，call8 用 -32..-4，
     * call12 用 -48..-4）。原来这里 `addi a1, a1, -80`，帧占
     * `[sp-80, sp)`，**正好盖住整个槽位区** —— 我们保存的 a0..a15 与
     * 硬件溢出槽互相覆盖，`retw`/下溢时读到的就是别人的帧数据。
     * 改成 -128，帧占 `[sp-128, sp-48)`，与槽位区完全不相交。
     * （`_vec_level1` 早就用 -128 了，见 §361；这里漏改了。） */
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
    /* §372：**帧自描述** —— 偏移 76 存"恢复这个帧之后 sp 应该是多少"。
     *
     * 原来 RESTORE_FRAME 结尾写死 `addi a1, a1, 80`，而 §361 把中断帧改成了
     * 128 字节并且 `port_irq_fill` 恰好把 c->sp 记成那个中断帧 —— 于是任何
     * 被中断过的任务恢复时 sp 都会**低 48 字节**（踩在别人栈上，症状是垃圾
     * a1 与随机 TRAP）。帧大小写死在两处迟早还会错，所以改成每一帧自带这个
     * 数字，恢复方只负责读。 */
    addi    a14, a1, 128
    s32i    a14, a1, 76
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
    /* 保存 WINDOWSTART（帧偏移 68）和 WINDOWBASE（帧偏移 72）。
     * **WINDOWBASE 也必须是每任务一份**（§258）：它是全局寄存器，别任务的
     * windowed 调用（call8/call12）会把基址搬来搬去；如果切回来时不复原，
     * 被打断的 windowed 函数 retw 时的"调用者窗口"就落到了错误的物理寄存器上
     * ——真板症状就是 cause=0 落在 retw.n、且 WINDOWSTART 与 CALLINC 对不上。 */
    rsr     a5, windowstart
    s32i    a5, a1, 68
    rsr     a5, windowbase
    s32i    a5, a1, 72
    s32i    a1, a4, 64
    /* 诊断：yield 的外出任务 ctx 与保存的 sp */
    l32r    a14, .Ldbg2
    s32i    a4, a14, 16
    s32i    a1, a14, 20

    /* §430：**yield 恢复的是自己**，所以要读自己那个 ctx 的 sp。
     *
     * 原来从 `g_sched[core].sp` 读 —— 那是"最近一次 `port_task_switch`
     * 外出的 sp"，属于**别的**任务。§429 把 switch 的写入挪走后这里更明显：
     * 会恢复到一个空槽/垃圾 sp。`a4` 在保存之前就已经是
     * `g_s3_cur[core]`（当前任务的 ctx），用它。 */
    l32i    a1, a4, 64
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
/* §412：windowed 跳板。被 tramp 用 `call8` 调（硬件完成窗口轮转），
 * 再用 `callx8` 去调远端 windowed 入口。a12 里是目标地址、a2 是参数，
 * 两者在 `call8` 的轮转里都不受影响（a12 → 被调者的 a4）。
 * 返回时 `retw` 会把窗口退回去。 */
    .align  4
    .global s3_win_tramp
    .type   s3_win_tramp, @function
s3_win_tramp:
    /* call8 轮转规则（§385 已实测）：
     *     壳的 a0..a7  = 调用者的 a8..a15
     *     壳的 a8..a15 = 调用者的 a0..a7
     * tramp 把目标地址放在 **a12**，而 a12 是 tramp 的 a8..a15 之一
     * ⇒ 在壳里它变成 **a4**。参数在 tramp 的 a2 ⇒ 壳里是 **a10**。 */
    /* §413：跳板必须有**标准的 windowed 函数开场 `entry`**。
     *
     * 我第一版写的是 `mov a8,a4 / callx8 a8 / retw`，结果
     * `TRAP pc=00000001 a0=00000000`。原因：`retw` 是靠 PS.CALLINC
     * 决定"往回轮转几个窗口"的，而被调者自己的 `retw` **已经把 CALLINC
     * 归零**；跳板再 `retw` 就rotate 0 个窗口，用当前窗口里那个 0 当返回
     * 地址跳走。`entry` 才是建立"被调者帧 + CALLINC 记账"的那条指令。 */
    entry   a1, 16
    s32i    a0, a1, 0
    mov     a8, a4               /* 壳的 a4 = tramp 的 a12 = 目标地址 */
    callx8  a8
    l32i    a0, a1, 0
    retw

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
    /* §259：这里原来还往 DRAM 全局 g_tramp_dbg 里多写 4 个字做诊断，实测
     * **只加这 4 条 store 就足以让 WiFi 初始化阶段的时序翻车**（bringup 从
     * 3/3 变成 0/3），说明整条链路还有很紧的时序窗口。诊断信息保留在 RTC
     * 那 3 个字里就够了，别再往这条路径上加东西。 */
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
    /* §412：**用 `call8` 而不是 `callx8` 进 windowed 入口**。
     *
     * `call8` 与 `callx8` 的差别不在"编码 CALLINC"，而在**硬件会把
     * WINDOWSTART 推进一格**（真正把 8 个寄存器让给被调用者）；`callx8`
     * 只做前者。被调用者因此拿到的 a1 还是 tramp 的，第一条
     * `s32i a0, a1, 0` 就写到 `0xffffffe0`。
     *
     * 但入口地址在 flash（0x42xxxxxx），`call8` 的 128KB 相对位移够不着，
     * 所以先 `call8` 到**近处的 windowed 跳板** `s3_win_tramp`（本文件内、
     * 同一个段），由它去 `callx8` 远端入口 —— 此时窗口已经轮转过，
     * `callx8` 只是在已经正确的窗口里做一次远调用。 */
    call8   s3_win_tramp
    call0   os_task_exit
    j       1f
2:
    callx0  a12
    call0   os_task_exit
1:  j       1b

    .section .text, "ax", @progbits
    .align  4
    .literal_position
    .literal .Lepclo,  0x40000000
    .literal .Lepchi,  0x44000000
    .literal .Lbadrec, 0x50001880

    .section .text, "ax", @progbits
    .align  4
    .literal_position
    .literal .Lstklo2, 0x3FCA0000
    .literal .Lstkhi2, 0x3FCEF000
    .literal .Lbadsp,  0x500018E0
