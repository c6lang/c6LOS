/* ESP32-S3 call0 上下文：统一用 80 字节陷阱帧保存 a0/a2..a15 + EPC + PS。
 * S3Ctx: r[0..15]@0..60（未用）、sp@64（指向帧）、stack@68、size@72。 */

    .section .text.ctx, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lg_sched, g_sched
    .literal .Lrtc_cs,  0x50001E80
    /* §464：窗口还原校验失败时的现场记录区（坏 WB / 坏 WS / 帧 / 次数）。
     * 0x50000600..0x5000064F 是 kernel/task.c6 §463 的阻塞诊断，
     * 0x50000740 起被 wifi_glue 占用，0x50000680 这一格是空的。 */
    .literal .Lwbdiag,  0x50000680
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
    .literal .Lwrap_diag, 0x50001C00   /* §884 */
    /* §450：错位闸门专用区。
     * **不能放在 0x50001800+32..+44** —— 那 4 个字被 `kernel/task.c6:430`
     * 的唤醒诊断占着，写进去会被它覆盖（真板实测读出来是随机值）。
     * 0x50001900 目前没人用（0x50001880/0x500018C0/0x500018E0 分别是
     * 坏帧记录 / sp 记录 / sp 闸门，0x50001D00 被 port_snapshot 占）。 */
    .literal .Lmis,     0x50001900
    /* §452：yield 轨迹环形缓冲，8 项 × 8 字节（每项 = ctx, sp），
     * 索引在 0x50001A40。用来回答"`ctx[0].sp` 是第几次 yield 离开自己
     * 栈区的" —— §450/§451 的两道闸门只能给出"某一次越界"，看不出
     * 它是从一开始就错，还是跑了一段才漂出去的。 */
    .literal .Ltrail,     0x50001A00
    .literal .Ltrail_idx, 0x50001A40
    /* §455：switch 轨迹（环形 8 项 × 8 字节，每项 = ctx, 恢复的 sp），
     * 与 §452 的 yield 轨迹交叉比对，用来判定"切到 ctx[N] 的那一刻，
     * 它的 sp 是不是已经错了"。索引在 0x50001B40。 */
    .literal .Lsrail,     0x50001B00
    .literal .Lsrail_idx, 0x50001B40
    .literal .Lwscratch, g_win_scratch
    .literal .Ltmpsp,   g_ctx_tmp_sp
    .literal .Lareg_magic, 0x41524547
    .literal .Lyl_cnt,  g_yl_cnt
    .literal .Lwt_diag, 0x50001E00


/* §934: 手写帧补 [sp-12]=上一帧 sp（_win_unf8 用），a9=当前 sp。 */
    .macro  WIN_CHAIN_FIX prevsp, scratch
    addi    \scratch, a1, -12
    s32i    \prevsp, \scratch, 0
    /* §995 曾在这里试过加一条 `memw`（怀疑帧链 store 没排空、被窗口溢出
     * 处理体读到旧的 0）。实测**无效**（仍 `bWu1b`），已删除。
     * §997 进一步查明：之前"加几条 store 就能过关"其实是**寄存器污染**
     * （那段代码冲掉了 a10，而 `_win_unf8` 要求 a8..a15 被保持），
     * 不是时序问题。 */
    mov     a9, a1
    .endm

/* §304：把当前所有祖先窗口整圈 spill 到各自栈帧（同 w2c.S 的 WIN_SPILL）。
 * 任务切换前必须先做：否则被切走任务的寄存器还压在物理窗口里，
 * 等它被调度回来时物理寄存器已经是别的任务的内容了。 */
    .macro  S3_SPILL_ALL
    rsr     a8, ps
    extui   a10, a8, 4, 1
    bnez    a10, 55f
    l32r    a4, .Lwscratch
    rsr     a10, epc1
    s32i    a10, a4, 16
    s32i    a8, a4, 20          /* §945: 保存原始 a8（库指针），宏末尾恢复 */
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
    /* normalize WS to single bit */
    rsr     a10, windowbase
    movi    a11, 1
    ssl     a10
    sll     a11, a11
    wsr     a11, windowstart
    isync
    l32i    a10, a4, 16
    wsr     a10, epc1
    l32i    a8, a4, 20          /* §945: 恢复 a8 —— 否则库指针被 PS 冲掉 */
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
    bnez    a3, 40f
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
    j       11f
40:
    /* §457 ★ WINDOWBASE / WINDOWSTART 必须逐任务复原。
     *
     * `port_task_switch` 和 `port_task_yield` 的保存段（`ctx.S:349-351`、
     * `:555-557`）一直**在存**这两个寄存器，注释还写着 §258 的理由：
     * "WINDOWBASE 也必须是每任务一份，别任务的 windowed 调用会把基址搬来
     * 搬去；如果切回来时不复原，被打断的 windowed 函数 retw 时的调用者窗口
     * 就落到了错误的物理寄存器上"。**但恢复这一侧从来没有实现** ——
     * 这里原来只有"68 == 0 就强制归零窗口"那一条，对已有任务(68 != 0)
     * 直接跳 `11:`，一个字节都不碰。
     *
     * §455/§456 实测到的正是这个后果：库任务把 WINDOWBASE 推到 10，
     * 切回 wifi 主任务时 WINDOWBASE 还是 10，主任务的 `retw` 于是落到
     * 库任务用过的物理窗口上，把库任务的 `a1`（`0x3fcc0300` 这种）当成
     * 自己的 sp "恢复"回来 —— 表现就是 switch 明明恢复对了 sp
     * （`0x3fcbe8a0`），下一次 yield 却已经在库任务的栈里。
     *
     * `wsr windowbase` 会重映射 aN→物理寄存器的对应关系，连 `a1` 自己
     * 都会变，所以帧指针必须先存进 `g_ctx_tmp_sp`（和上面那段同样的手法）。
     * 保存下来的 (windowstart, windowbase) 是当初真的在同一颗核上生效过的
     * 组合，天然满足 `(WS >> WB) & 1 == 1` 和 WB%4==0 两条硬不变式。 */
    l32i    a5, a1, 72              /* 本任务保存的 WINDOWBASE */
    l32r    a4, .Ltmpsp
    s32i    a1, a4, 0               /* 藏帧指针（wsr windowbase 后 a1 会变）*/
    /* ★ 顺序不能反（第一版就写反了，真板症状是 `pc` 落在
     * `_vec_default+0x2c` 的自循环里、`exccause` 走未预期异常）。
     *
     * `wsr windowbase` 改变的是 aN→物理寄存器的映射，写完 `a3`/`a5` 里装的
     * **就已经不是原来那个值**了。所以必须先 `wsr windowstart, a3`
     * （windowstart 不改变映射，此时 `a5` 仍是 WINDOWBASE，仍然有效），
     * 再 `wsr windowbase, a5`。
     *
     * 中间那一瞬 (新 WS, 旧 WB) 可能违反 `(WS>>WB)&1==1`，但硬件只在
     * `retw`/`call` 时才检查，而这一段整个在 `rsil 15` 关中断内、
     * 两条 `wsr` 之间没有任何指令，所以没有窗口可被观察。 */
    /* §464 ★ 写前校验：帧里的 WINDOWBASE / WINDOWSTART 可能是垃圾。
     *
     * §464 真板实测的崩溃现场（`_vec_default` 记在 a2 指向处，这次正好是
     * 0x50000100）：
     *     magic=0xbad0  exccause=29(0x1d = LoadStorePIFAddrErrorCause)
     *     excvaddr=0xfffffff1  epc1=0x40379746  a1=0x0000000a
     *     wb=0xa  ws=0x400  ps=0x0006091f
     * 而 0x40379746 反汇编出来正是这一行的 `wsr a5, windowbase`：
     *     40379740: wsr.windowstart a3
     *     40379746: wsr.windowbase  a5     ← 崩在这里
     * 也就是说 `l32i a5, a1, 72` 从"帧"里取出来的 WINDOWBASE 是越界值，
     * `wsr` 直接进未预期异常，CPU 停在 `_vec_default+0x2c` 的 `j .` 上。
     *
     * 这是 §457 那次修复引入的新失败模式：窗口还原本身是对的（§457 之后
     * sp 错位确实消失了），但**没有校验**，一旦帧被踩坏就整机卡死，
     * 而且现场全丢（只能靠 JTAG 读 ps/exccause 猜）。
     *
     * NUM_AREGS=64 ⇒ WINDOWBASE ∈ [0,16)；WINDOWSTART 只有低 16 位有意义。
     * 两者任一越界就走 `43:` 的兜底：记录坏值到 0x50000680，然后把窗口
     * 归一化（windowstart=1 / windowbase=0，与上面 `68 == 0` 分支一致），
     * 保证不崩。归一会让这个任务的 retw 落错窗口，但那是可恢复的逻辑错误，
     * 比整机卡死好得多，而且坏值被记下来可以直接看出是哪个帧坏了。 */
    movi    a6, 16
    bgeu    a5, a6, 43f             /* WINDOWBASE >= 16 ⇒ 非法 */
    /* §470 ★★ 还必须校验"WINDOWBASE 是 4 的倍数"。
     * Xtensa 硬不变式：windowbase 必须 4 对齐 —— 它同时也是 WINDOWSTART
     * 的右移量，要求 `(WINDOWSTART >> WINDOWBASE) & 1 == 1`。
     * 写一个非 4 倍数的值，`wsr windowbase` 自己就会进异常。
     *
     * §468 把库任务栈抬到 8192 之后，`_vec_default` 记下的是：
     *   exccause = 0x1d (LoadStorePIFAddrErrorCause)
     *   epc1     = 0x40379754   ← 就是下面这条 `wsr windowbase`
     *   windowbase = 0xa (=10)  ← 非 4 倍数
     * §464 只查了 `< 16`，10 能通过，所以照样崩。
     * （上面注释里"库任务把 WINDOWBASE 推到 10"这句，恰恰说明 10 是垃圾值，
     *  不是合法的库任务窗口 —— 合法值只能是 0/4/8/12。） */
    /* ★必须是 srli（取高 16 位）而不是 slli。写成 slli a6, a3, 16 时，
     * 只要 low16 != 0 结果就 != 0，于是**每一次**恢复都被误判成非法
     * （§464 实测：诊断区记到 坏WB=0x0 坏WS=0x00000001、次数=8，
     *  两个值其实都完全合法），窗口全被归一化掉。 */
    srli    a6, a3, 16
    bnez    a6, 43f                 /* WINDOWSTART 高位非 0 ⇒ 非法 */
    /* §910 ★★ 必须写回帧里保存的**完整** WS（真板 0xaa00 = bit 9/11/13/15），
     * 不能像 §907 那样写成 1<<WB 单比特。真板证据（§908 那次）：
     *   epc1 = 0x403798f2 = port_task_yield 壳里的 `retw.n`
     *   WB=10(异常入口 rotw-1 前是 11)，WS=0x1000(只有 bit12)。
     * `retw` 会把 WB 从 12 旋到 11（CALLINC=1），落进**调用者**的窗口；
     * 单比特 WS 的 bit11=0 ⇒ 该窗口非法 ⇒ WindowUnderflow ⇒
     * _win_unf8 的 `l32e a7,a1,-12` 拿 a1=0 当 sp ⇒ 双重异常 excvaddr=-12。
     * 多位 WS 正是嵌套 windowed 调用链所需要的：每一层活窗口都得置位。
     * §907 引入单比特是为了躲开"写 WS 时 WB 还是上一个任务的" —— 那个
     * 问题已由 §908（先清 PS.WOE、并且真的写 wsr windowbase）解决，
     * 所以这里恢复写回完整 a3（帧[68]）。
     * a3 在上面 `srli a6,a3,16` 之后没被改过，仍是保存的 WINDOWSTART。
     * 顺序仍是先 WS 后 WB：wsr windowbase 会重映射 aN，a3 必须先消费掉。 */
    /* §908 ★★ WSR.WINDOWSTART/WINDOWBASE 必须在 PS.WOE=0 时写 —— IDF
     * SET_STACK 宏就是先 `ps &= ~(WOE|OWB|CALLINC)` 再写窗口寄存器的。
     * 真板证据：epc1 正好停在 `wsr.windowbase a5` 那条指令，exccause=0x1c、
     * 双重异常落在 _win_ovf12+3（`l32e a0,a1,-12`，a1=0xf ⇒ excvaddr=3）。
     * WOE=1 时改窗口基址会让硬件按新映射去解释旧窗口，立即触发窗口异常。
     * 保留 bit0..7（INTLEVEL/EXCM/UM）和 bit20..31，清掉 bit8..19
     * （OWB + CALLINC + WOE）；末尾 §905 会用帧里的 PS 整体写回。 */
    rsr     a7, ps
    extui   a6, a7, 0, 8
    srli    a7, a7, 20
    slli    a7, a7, 20
    or      a7, a7, a6
    wsr     a7, ps
    rsync
#ifdef S3_SPILL_SHELL
    movi    a6, 1
    ssl     a5
    sll     a6, a6
    wsr     a6, windowstart
#else
    /* ★★★★ §1050（**根因修法**）：写回帧里的 WINDOWSTART 之前，
     * **先和硬件当前的 WINDOWSTART 取并集**。
     *
     * §1049 实测：到 ppTask 故障那一刻，`RESTORE_FRAME` 已经跑了 **258 次**
     * （`0x50000400` 的计数 = 0x102）。而每次恢复都用**帧里保存的 WS 整值**
     * 去覆盖硬件 —— 那份快照只反映"任务被切走那一刻"的窗口状态。
     *
     * 但 windowed 调用链的**祖先窗口在切走、切回之间一直活着**，
     * 它们的位必须保持。直接覆盖就会把"当前仍 live 的窗口位"抹掉，
     * 于是下一次 `retw` 误判为欠载、从没写过的溢出记录里恢复 ⇒ `a1 = 3`。
     *
     * 取并集既保留了帧里的意图，又不会丢掉此刻仍活着的窗口位。 */
    /* ★★★★ §1083：**不再取并集，直接写帧里的 WS**。
     *
     * §1050 当初取并集的理由是"祖先窗口在切走/切回之间一直活着"。但 §1056
     * 之后，所有会阻塞的 OSI 函数（sem/mutex/event/delay）都加了
     * `W2C_SPILL` 薄壳：**阻塞前就已经把祖先窗口 spill 到自己栈上**，
     * 它们的位理应已经清掉。
     *
     * 于是"并集"变成了**只增不减的泄漏**：每次任务恢复都把上一批任务残留的
     * 窗口位又并回来，WS 位型一路涨到饱和。§1082 真板实测崩溃那一瞬
     * `WS = 0xbf54`（16 个窗口里约 10 个被标活）—— 环上根本没有那么多
     * 真活窗口，于是 `retw` 的欠载判断全乱，`a1` 被恢复成 0。
     *
     * 改回直接赋值，让 WS 精确等于"该任务被切走那一刻"的集合。 */
    wsr     a3, windowstart
#endif
    isync
    wsr     a5, windowbase
    isync
    l32r    a4, .Ltmpsp
    l32i    a1, a4, 0               /* 取回帧指针（此时窗口已是本任务的）*/
    j       44f
43:
    /* 兜底：先记现场（坏 WB / 坏 WS / 帧 / 累计次数），再归一化窗口。 */
    l32r    a4, .Lwbdiag
    s32i    a5, a4, 0
    s32i    a3, a4, 4
    s32i    a1, a4, 8
    l32i    a7, a4, 12
    addi    a7, a7, 1
    s32i    a7, a4, 12
    l32r    a4, .Ltmpsp
    s32i    a1, a4, 0               /* 藏帧指针：wsr windowbase 会重映射 a1 */
    movi    a6, 1
    wsr     a6, windowstart
    isync
    movi    a6, 0
    wsr     a6, windowbase
    isync
    l32r    a4, .Ltmpsp
    l32i    a1, a4, 0
44:
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
    /* §905 ★★ 必须把 PS.OWB 改成本任务保存的 WINDOWBASE。
     * `call8`/`retw` 只改 WINDOWBASE 硬件寄存器，**不会**同步 PS.OWB；
     * PS.OWB 只在进异常/中断时由硬件写入。我们的 task yield 是普通 call8
     * 进来的（不是 IDF 那种软件中断），所以帧里存的 PS.OWB 是**旧的**：
     * 真板实测 帧[68]=WS=0xaa00(bit 9/11/13/15)、帧[72]=WB=15，
     * 但 帧[64]=PS=0x00060a00 ⇒ OWB=10。rfe 按 OWB 把 WB 设回 10，
     * 而 WS bit10=0 ⇒ (WS>>WB)&1==0 非法组合，任务一 retw 就在
     * `_win_ovf8` 里拿 a1=0xf 当 sp 双重异常打死在 `_vec_default`。
     *
     * 修法：清掉 PS 的 bit8..11，再把帧[72] 的 WB 移进 OWB，
     * 这样 rfe 装回的 WB 和上面的 wsr windowstart 才是同一套状态。 */
    l32i    a6, a1, 72
    srli    a7, a2, 12
    slli    a7, a7, 12          /* 保留 bit12..31（含 CALLINC/WOE），下面放回 */
    extui   a2, a2, 0, 8        /* 保留 INTLEVEL/EXCM/UM 等低 8 位 */
    slli    a6, a6, 8           /* WINDOWBASE << 8 → PS.OWB（bit8..11） */
    or      a2, a2, a6
    or      a2, a2, a7
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
#ifdef S3_SPILL_SHELL
    l32r    a8, .Lareg_magic
    l32i    a9, a1, 108
    bne     a8, a9, 94f
    l32i    a0, a1, 80
    l32i    a2, a1, 84
    l32i    a3, a1, 88
    l32i    a4, a1, 92
    l32i    a5, a1, 96
    l32i    a6, a1, 100
    l32i    a7, a1, 104
94:
    /* §942: 标记检查用 a8/a9 当 scratch，必须无条件恢复 —— a9 是
     * windowed 调用的 spill 指针，留成 magic 会让下一次 call8 写飞。
     * 放在 94: 之后，ISR 帧（无标记）也能恢复。 */
    l32i    a8, a1, 28
    l32i    a9, a1, 32
#endif
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
    /* ★ §967（真 bug，真板取证见 docs/wifi-plan.md）：
     * **恢复完 a1 之后必须把 a9 也设成这个 a1。**
     *
     * windowed ABI 里 sp 的"影子"在 a9：`call8/callx8` 的被调者拿到的 a1
     * 来自**调用者的 a9**（§890）。`entry a1,N` 会自己把 a9 设好，所以
     * 函数内部的第二层调用没问题；但**任务恢复的这一刻没有 `entry`** ——
     * `rfe` 直接把窗口装回来，a9 取的是帧里的偏移 32 槽。
     * `port_task_ctx_create` 把整帧 20 个字清零（§339），所以新任务的
     * a9 就是 **0**；被切走的任务那一格也可能停在一个早先的暂存值上。
     *
     * 真板现场（§967，记录在 RTC `0x500018c0`）：
     *   ppTask 在 `403822c7: or a11,a1,a1` 时 a1 还是对的 `0x3fcc4790`，
     *   紧接着 `403822ca: callx8 a5` 把 a9 当成了被调者的 sp；返回后
     *   a1 被 `_win_unf8` 还原成 0，下一句 `403822d3: l32i.n a5,a1,0`
     *   读地址 0 → `exccause=0x1c`(LoadStoreAlignment)、`excvaddr=0`，
     *   落进 `_vec_level1` 的坏 sp 闸门原地死转（就是 `bWu1b` 之后卡住）。
     *
     * IDF 的 SET_STACK 也是这么约定的：`a9 = new_sp`。这里补上，
     * 让"任何被恢复的上下文"都满足 a9 == a1 这个不变式。
     * 放在范围检查之后：a3 已经被当 scratch 用掉了，这里只用 a1。 */
    mov     a9, a1
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

#ifdef __XTENSA_CALL0_ABI__
    .global port_task_switch
    .type   port_task_switch, @function
port_task_switch:
#else
    /* §884: 全 windowed 构建里本函数被 call8 调用；原 call0 体改名为 _c0，
     * 由下面的 windowed 壳 entry→call0→retw 进入，保证 a1 是正确的 sp。 */
    .global port_task_switch_c0
    .type   port_task_switch_c0, @function
port_task_switch_c0:
#endif
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
    /* ★ §973（真 bug）：`mov a9, a1` **必须在 `s32i a9, a1, 32` 之后**。
     *
     * 原来这两句的顺序是
     *     addi a1, a1, -128
     *     mov  a9, a1          ← 先把 a9 改成"新帧的 sp"
     *     ...
     *     s32i a9, a1, 32      ← 于是存进帧里的是 a1-128，不是被打断上下文的 a9
     * 帧偏移 32 是"恢复时要写回 a9 的那一格"。存错的后果不是立刻崩：
     * 任务被切回来时 a9 = sp-128，而 windowed ABI 要求 a9 == sp
     * （`call8/callx8` 的被调者 a1 取调用者的 a9）。于是任务下一次
     * `callx8` 的被调者拿着 `sp-128` 当栈跑，返回时 `_win_unf8`
     * 把 a1 还原成别处的值。
     *
     * 真板现场（docs/wifi-plan.md §967/§968/§971，RTC `0x500018c0`）：
     *   `a9 = 0x3fcc4710`、而任务真实 sp = `0x3fcc4790` ——
     *   **差值正好 0x80 = 128 = 这一帧的大小**，一字不差地指向这里。
     *   后续 `403822d3: l32i.n a5,a1,0` 读地址 0 → `exccause=0x1c`、
     *   `excvaddr=0`，掉进 `_vec_level1` 的坏 sp 闸门死转（`bWu1b` 之后卡住）。
     *
     * `mov a9, a1` 本身不能删：它是在给**后面那条 call8** 准备
     * "callee 的 a1 = caller 的 a9"（§896）。所以只把它挪到存完之后。 */
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
    mov     a9, a1
#ifdef S3_SPILL_SHELL
    /* §939: 显式存当前窗口 a0..a7 到帧尾 + 打标记（ISR 帧没这些槽）。 */
    s32i    a0, a1, 80
    s32i    a2, a1, 84
    s32i    a3, a1, 88
    s32i    a4, a1, 92
    s32i    a5, a1, 96
    s32i    a6, a1, 100
    s32i    a7, a1, 104
    l32r    a8, .Lareg_magic
    s32i    a8, a1, 108
    /* §940: [sp-12] 链必须在所有寄存器存完后再写 —— WIN_CHAIN_FIX 用
     * a8/a10 做 scratch，放前面会毁掉刚存进 frame[28]/[36] 的 a8/a10。 */
    addi    a10, a1, 128
    WIN_CHAIN_FIX a10, a8
#endif
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
    /* §473 ★★★ 必须保存 PS 到帧偏移 64 —— `port_task_switch` 原来**漏了**这四行。
     *
     * `RESTORE_FRAME` 的恢复段：
     *     l32i a2, a1, 64      ← 取 PS
     *     movi a3, 16
     *     or   a2, a2, a3      ← 置 EXCM（给 rfe 用）
     *     wsr  a2, ps
     *     ... rfe
     * `port_task_yield` 的保存段一直是**有**这四行的
     * （`rsr a5,ps / movi a6,-32 / and / s32i a5,a1,64`，见下面 yield 那段），
     * 唯独 `port_task_switch` 没有 ⇒ 偏移 64 是**栈上的旧内存**。
     * 一旦那格恰好带着 EXCM=1（或 INTLEVEL≠0），`rfe` 返回后 EXCM 仍是 1，
     * 被恢复的任务执行的下一条 `retw` 就是**非法指令** —— 正是 §468 抓到的
     * `exccause=0`、`epc1=0x40048c6e(retw.n)`、`ps=0x00060010`(EXCM=1,
     * INTLEVEL=0)、`a0=0`。
     *
     * 和 yield 一样清掉 INTLEVEL(bit0-3) 与 EXCM(bit4)：恢复侧会自己
     * `or 16` 重新置 EXCM 给 `rfe`，而任务自身的 INTLEVEL 必须是 0。 */
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64
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
    /* §454：写 `g_s3_cur` 之前校验 `a4` 是个像样的 ctx 指针。
     *
     * §453 证明 `port_task_yield` 里的 `a4` 只可能来自 `g_s3_cur[core]`
     * （`ctx.S:445-448` 是唯一来源，没有第二个入口），而真板轨迹里出现过
     * `0x42018088`（代码地址）、`0x00000006`、`0x4037992e` 这些绝不可能是
     * ctx 的值 ⇒ `g_s3_cur[0]` 确实曾被写成垃圾。全系统里写它的地方只有
     * 下面这条 `s32i a4, a3, 0`，所以就在这里拦。
     *
     * 判据刻意做得很宽（只查两条，避免误报）：
     *   1. 16 字节对齐 —— `S3Ctx` 是 76 字节但按 4 对齐，实际观测都是 16 对齐；
     *   2. 高 8 位 == 0x3f —— DRAM 段。这样 `0x42..`（代码）、`0x00..`（小整数）
     *      都会被抓住，而 `0x3fcc6f1c`（ctx[1]，合法）会放行。
     * 用 `extui` 取高 8 位而不是 `movi 0x3fca0000`，是因为 `movi` 只装得下
     * 12 位有符号立即数。
     * 仍然只记录不修正，只记首次（标志 `0x450C`）。 */
    extui   a6, a4, 0, 4
    bnez    a6, 6f
    extui   a6, a4, 24, 8
    movi    a7, 0x3f
    bne     a6, a7, 6f
    j       7f
6:
    l32r    a8, .Lmis
    l32i    a9, a8, 44
    movi    a6, 0x450C
    beq     a9, a6, 7f
    movi    a6, -17705              /* 0xBAD7 */
    s32i    a6, a8, 32
    s32i    a4, a8, 36              /* 那个垃圾 ctx */
    s32i    a1, a8, 40              /* 当时的帧基址 */
    movi    a6, 0x450C
    s32i    a6, a8, 44              /* 标志 */
7:
    l32r    a3, .Lg_cur
    slli    a5, a12, 2
    add     a3, a3, a5
    s32i    a4, a3, 0
    l32i    a1, a4, 64
    l32r    a14, .Ldbg2
    s32i    a1, a14, 8              /* 恢复出来的目标 sp */
    /* §455：switch 轨迹（环形 8 项）。寄存器纪律同 §452：只用 a8..a10
     * 和 a15（a15 已存进帧的 56 槽，RESTORE_FRAME 会覆盖它）。 */
    l32r    a8, .Lsrail
    l32r    a9, .Lsrail_idx
    l32i    a10, a9, 0
    extui   a15, a10, 0, 3
    slli    a15, a15, 3
    add     a15, a8, a15
    s32i    a4, a15, 0              /* 本次切到的 ctx */
    s32i    a1, a15, 4              /* 从它恢复出来的 sp */
    addi    a10, a10, 1
    s32i    a10, a9, 0
    /* §451 恢复前校验：比 yield 那道闸门更靠前，能抓到**第一次越界恢复**。
     *
     * §450 的 yield 闸门第一次命中时 sp 已经是 `0x3fcc2300`，说明在这次 yield
     * **之前**任务就已经在越界区里跑了 —— 也就是 `port_task_switch` 把它恢复
     * 到了别人的栈上。真正的源头在一次"越界恢复"，而那次恢复本身不经过
     * `s32i a1, a4, 64`，所以 yield 闸门看不到它。
     *
     * 这里拿 `a4`（= 本次要恢复的目标 ctx）自己的 `stack`/`size` 核对
     * `a1`（= `c->sp`，帧基址）。帧基址必须落在 `[stack, stack+size)` 内。
     * 同样**只记录不修正**，只记首次（标志用 `0x450B`，与 yield 闸门的
     * `0x450A` 分开，两者互不干扰）。 */
    l32i    a6, a4, 68              /* c->stack */
    l32i    a7, a4, 72              /* c->size  */
    beqz    a6, 8f
    beqz    a7, 8f
    add     a7, a6, a7
    bltu    a1, a6, 7f
    bgeu    a1, a7, 7f
    j       8f
7:
    l32r    a8, .Lmis
    l32i    a9, a8, 28
    movi    a6, 0x450B
    beq     a9, a6, 8f
    movi    a6, -17706              /* 0xBAD6 */
    s32i    a6, a8, 16
    s32i    a4, a8, 20
    s32i    a1, a8, 24
    movi    a6, 0x450B
    s32i    a6, a8, 28
8:
    RESTORE_FRAME

#ifndef __XTENSA_CALL0_ABI__
    /* §885: windowed 壳 —— **尾跳**而非 entry+call0。
     *
     * 被 windowed C 用 callx8 调用时，进入本壳的寄存器：
     *   a0 = 带 CALLINC=2 标记的返回地址（0x82xxxxxx）；
     *   a1 = 合法 sp（call8 的 callee a1 = 调用者 a9，编译器已备好）；
     *   a2 = ctx 参数（call8 的 callee a2 = 调用者 a10）。
     * 上一版用 `entry a1,64` 会执行 `rotw -CALLINC`，把逻辑寄存器重新映射，
     * a2 立刻变成调用者的旧 a2（实测 0x60a00，一个 PS 值）—— ctx 就此丢失。
     *
     * c0 体**从不正常返回**，结尾总是 RESTORE_FRAME → rfe 切到目标任务；
     * 而它算 EPC 用 `(a0 & 0x3fffffff)|0x40000000`，恰好把 CALLINC 标记剥掉
     * 换回 flash 高位（0x82018f5b → 0x42018f5b，正是 callx8 的下一条）。
     * 所以直接尾跳：a0、a2 原样带进 c0，保存的帧完全正确；将来该任务被
     * 切回时 rfe 恢复带标记的 a0，调用者的 retw 自会转回窗口。
     * 参照 win_bridge.S：call8 进来的函数用 addi 而非 entry 即可。 */
    .align  4
    .global port_task_switch
    .type   port_task_switch, @function
port_task_switch:
    /* §891: windowed C 用 `callx8` 调进来。真板帧实测：callx8 **不轮转窗口**，
     * 它把带 CALLINC 的返回地址写进 a8，而 a0 还是调用者的旧值
     * （a0=0x82018c00、a8=0x82018b00、a2=任务结构 t、a10=ctx）。
     * c0 从 a0 算 EPC、从 a2 取 ctx，所以两者都要先纠正：
     *   a0 <- a8  ⇒ EPC = 0x42018b00（callx8 的下一条）
     *   a2 <- a10 ⇒ ctx 而不是 t
     * 只修 a2 时 EPC 仍指向旧 a0 的 retw.n，切回来立刻崩 —— 这就是上一版
     * 只加 mov a2,a10 反而更糟的原因。
     * §895: IDF 正统 SPILL_ALL_WINDOWS 试过（entry+5×rotw）：第一次切换就在
     * 第 3 个 `and a12,a12,a12` 上 exccause=0x1d（脏 a9 写飞），因为我们的
     * 中断/切换帧是 call0 风格、没有合法的 a9 链。故仍走本补丁。 */
    mov     a0, a8
    mov     a2, a10
    j       port_task_switch_c0
#endif

#ifdef __XTENSA_CALL0_ABI__
    .global port_task_yield
    .type   port_task_yield, @function
port_task_yield:
#else
    .global port_task_yield_c0
    .type   port_task_yield_c0, @function
port_task_yield_c0:
#endif
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
    /* ★ §973（真 bug）：`mov a9, a1` **必须在 `s32i a9, a1, 32` 之后**。
     *
     * 原来这两句的顺序是
     *     addi a1, a1, -128
     *     mov  a9, a1          ← 先把 a9 改成"新帧的 sp"
     *     ...
     *     s32i a9, a1, 32      ← 于是存进帧里的是 a1-128，不是被打断上下文的 a9
     * 帧偏移 32 是"恢复时要写回 a9 的那一格"。存错的后果不是立刻崩：
     * 任务被切回来时 a9 = sp-128，而 windowed ABI 要求 a9 == sp
     * （`call8/callx8` 的被调者 a1 取调用者的 a9）。于是任务下一次
     * `callx8` 的被调者拿着 `sp-128` 当栈跑，返回时 `_win_unf8`
     * 把 a1 还原成别处的值。
     *
     * 真板现场（docs/wifi-plan.md §967/§968/§971，RTC `0x500018c0`）：
     *   `a9 = 0x3fcc4710`、而任务真实 sp = `0x3fcc4790` ——
     *   **差值正好 0x80 = 128 = 这一帧的大小**，一字不差地指向这里。
     *   后续 `403822d3: l32i.n a5,a1,0` 读地址 0 → `exccause=0x1c`、
     *   `excvaddr=0`，掉进 `_vec_level1` 的坏 sp 闸门死转（`bWu1b` 之后卡住）。
     *
     * `mov a9, a1` 本身不能删：它是在给**后面那条 call8** 准备
     * "callee 的 a1 = caller 的 a9"（§896）。所以只把它挪到存完之后。 */
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
    mov     a9, a1
#ifdef S3_SPILL_SHELL
    /* §939: 显式存当前窗口 a0..a7 到帧尾 + 打标记（ISR 帧没这些槽）。 */
    s32i    a0, a1, 80
    s32i    a2, a1, 84
    s32i    a3, a1, 88
    s32i    a4, a1, 92
    s32i    a5, a1, 96
    s32i    a6, a1, 100
    s32i    a7, a1, 104
    l32r    a8, .Lareg_magic
    s32i    a8, a1, 108
    /* §940: [sp-12] 链必须在所有寄存器存完后再写 —— WIN_CHAIN_FIX 用
     * a8/a10 做 scratch，放前面会毁掉刚存进 frame[28]/[36] 的 a8/a10。 */
    addi    a10, a1, 128
    WIN_CHAIN_FIX a10, a8
#endif
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
    /* §452：yield 轨迹（环形 8 项）。寄存器纪律：
     *   - 不动 a11/a12（core 号）、a4/a1（闸门和保存都要用）；
     *   - 只用 a8..a10 和 **a15**（a15 已在帧的 56 槽存过，之后
     *     RESTORE_FRAME 会从帧里恢复，破坏它无害）。 */
    l32r    a8, .Ltrail
    l32r    a9, .Ltrail_idx
    l32i    a10, a9, 0
    extui   a15, a10, 0, 3
    slli    a15, a15, 3
    add     a15, a8, a15
    s32i    a4, a15, 0              /* 本项：ctx   */
    s32i    a1, a15, 4              /* 本项：sp    */
    addi    a10, a10, 1
    s32i    a10, a9, 0              /* 计数自增（不回绕，索引取低 3 位）*/
    /* §450 错位闸门：刚写进 `a4` 的那个 sp，必须落在 `a4` 自己记录的栈区间里。
     *
     * §449 的真板实测：`ctx[0]`（wifi 主任务，栈 size=16384）的 `sp` 被写成了
     * `0x3fcc2300` —— 那是 `ctx[1]`（库任务）的栈内地址，越界量恒为 5168 字节，
     * **把栈从 8192 加大到 16384 后越界量一点没变**。所以这不是栈真的长出去了，
     * 而是 `g_s3_cur[core]` 与实际在 CPU 上跑的任务错位：库任务的 yield 把
     * 库任务的 sp 写进了 wifi 主任务的 ctx。之后两个任务恢复同一份帧，
     * 变成同一个执行流，双双阻塞在 `osi_queue_recv` 上，没人能推进。
     *
     * 这里**只记录不修正**：命中就写 `0xBAD5` 到 `0x50001800+44`、把错位的 ctx
     * 指针写到 `+48`，方便离线定位第一次错位发生在哪个切换上。
     * 用 a6/a7（它们已经存进栈帧了，后面不会再读），不碰 a11（那是 core 号）。 */
    l32i    a6, a4, 68              /* c->stack */
    l32i    a7, a4, 72              /* c->size  */
    beqz    a6, 7f
    beqz    a7, 7f
    add     a7, a6, a7              /* 栈顶 */
    bltu    a1, a6, 6f              /* sp < stack      -> 错位 */
    bgeu    a1, a7, 6f              /* sp >= stack+size -> 错位 */
    j       7f
6:
    /* §450：**只记首次**。覆盖式记录只能看到最后一次命中，定位不到根因 ——
     * 第一次错位才是把 `ctx[0].sp` 污染的源头。
     *
     * 判断"是否已记录"必须用**魔数**，不能用 `== 0`：RTC 这片复位后不清零，
     * 上电初值是随机数，上一版用 `bnez` 判空就被随机值骗过去、把首次记录
     * 静默跳过了（真板读 `0x5000182c` 得到 `0x1f817f87`）。
     * 这里用 `0x450A`（16 位立即数，一条 `movi` 就能装）作标志。 */
    l32r    a8, .Lmis
    l32i    a9, a8, 12              /* +12 已记录标志 */
    movi    a6, 0x450A
    beq     a9, a6, 7f
    movi    a6, -17707              /* 0xBAD5 */
    s32i    a6, a8, 0               /* +0  闸门 */
    s32i    a4, a8, 4               /* +4  错位的 ctx */
    s32i    a1, a8, 8               /* +8  首次错位时的 sp */
    movi    a6, 0x450A
    s32i    a6, a8, 12              /* +12 置标志 */
7:

    /* 恢复到 `g_sched[core].sp` —— 那是**调度器主循环**的上下文
     * （`port_task_switch` 从调度器调用时存进去的）。
     *
     * §435：这里一度被我改成"恢复自己"，是**错的**。
     * `kernel/task.c6` 的 `os_task_block_wait()` 在 `state = BLOCKED` 之后
     * 只调 `port_task_yield()`，然后 `return g_cur->wait_result;` ——
     * 它**依赖 yield 切回调度器**。yield 若原地恢复自己，阻塞就退化成忙等：
     * 真板现象是串口刷出上百个 'b'（`task.c6:343` 每阻塞一次打一个）
     * 然后同一个任务重新进 `s3_win_wifi_main`。 */
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

#ifndef __XTENSA_CALL0_ABI__
    /* §884: windowed 壳，同 port_task_switch。 */
    .align  4
    .global port_task_yield
    .type   port_task_yield, @function
port_task_yield:
    entry   a1, 64
#ifdef S3_SPILL_SHELL
    /* §937: 壳帧自己的 [sp-12] 也必须初始化 —— SPILL_ALL 溢出的第一个窗口
     * 就是当前壳，_win_ovf8 的 `l32e a0,a1,-12` 从这里取"上一帧 sp"。
     * entry a1,64 只改 a1，不写这一格；不补就是垃圾 a9/a0。 */
    addi    a10, a1, 64
    addi    a8, a1, -12
    s32i    a10, a8, 0
    mov     a9, a1
    S3_SPILL_ALL
#endif
    s32i    a0, a1, 0
    call0   port_task_yield_c0
    l32i    a0, a1, 0
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
    /* §487：帧从 16 抬到 48。CALLINC=2 时 `entry` 的溢出区是
     * [a1+N-32, a1+N-4]；N=16 会把 a4..a7 的槽放到**帧外**，
     * 溢出/欠载处理体按 [sp-32,sp) 找槽时就会落到别人的帧里。 */
    entry   a1, 48
/* ★ §977（**根因**）：`WIN_CHAIN_FIX` 必须**无条件**执行，
     * 不能藏在 `#ifdef S3_SPILL_SHELL` 里。
     *
     * `S3_SPILL_SHELL` 在最终构建里**从来没定义过**（Makefile 里搜不到），
     * 所以这两件事在这条跳板上从来没发生过：
     *   1. `[a1-12] = 上一帧 sp`（帧链）
     *   2. `a9 = a1`
     *
     * 真板硬件断点实测（docs/wifi-plan.md §976/§977，gdb 硬件断点，
     * pc = ppTask+3，即 `entry a1,48` 刚执行完）：
     *     a1 = 0x3fcc4790   ✓ 正确
     *     a9 = 0x3fcc0660   ✗ 完全是别处的垃圾
     *     [a1-12] = 0x00000000   ✗ 帧链根本没写
     * 而 `_win_ovf8` 第一步就是 `l32e a0, a1, -12` 去取帧链，
     * 拿到 0 之后 `s32e a4, a0, -32` 就往 0xffffffe0 写；
     * 下溢那侧则从**从没被写过的清零栈**里还原 a0..a3，
     * 于是 a0 = a1 = 0 —— 正好等于闸门记录里存下的那两个 0
     * （RTC `0x500018c0`），随后 `403822d3: l32i.n a5,a1,0` 读地址 0，
     * `exccause=0x1c`(LoadStoreAlignment) → 掉进 `_vec_level1` 死转，
     * 串口就停在 `bWu1b`。
     *
     * IDF 的 `SET_STACK`（xt_instr_macros.h:39）就是
     * `*(sp-12) = sp + SAVE_AREA_OFFSET(48)`，和这里的
     * `prevsp = a1 + 48` 完全一致，所以这本来就是"windowed 根帧"的
     * 标准动作，不该受任何调试开关控制。 */
    addi    a10, a1, 48
    WIN_CHAIN_FIX a10, a8
    s32i    a0, a1, 0
    /* §906：进 callx8 之前的窗口现场，定位 os_sched_tick entry 的 cause=0。
     * 0x50001E00..1E10 在清零区(0x50000200..0x50001F00)内且 1E80 才被占用。 */
    l32r    a6, .Lwt_diag
    s32i    a1, a6, 0
    s32i    a4, a6, 4
    rsr     a7, ps
    s32i    a7, a6, 8
    rsr     a7, windowstart
    s32i    a7, a6, 12
    rsr     a7, windowbase
    s32i    a7, a6, 16
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
    /* §890 全 windowed ABI：新任务的帧里 a9 被清零，而 call8 的被调者
     * a1 = 调用者 a9。任务体是 windowed C，它再 callx8 到 os_sched_tick
     * 时对方 entry a1,144 会拿到 a1=0（真板 epc1=os_sched_tick、excvaddr=0）。
     * 进入任务体前必须把 a9 设成合法 sp。 */
    mov     a9, a1
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
    /* ★★★ §1024（**结构修法**）：**直接 `callx8 a12`，去掉 s3_win_tramp 这一级窗口**。
     *
     * tr45 实测：把跳板调用从 `call8` 换成 `callx8`，ppTask 仍是
     * `wb=4 / ws=0x15` —— 说明 §412 那句"`callx8` 不推进 WINDOWSTART"
     * **是错的**（两者都会轮转窗口）。
     * 而跳板存在的唯一理由是 `call8` 的 ±128KB 相对位移够不到 flash 入口；
     * `callx8 寄存器` 没有位移限制，**根本不需要跳板**。
     *
     * 收益：任务入口少占一级窗口 ⇒ ppTask 从 **WB=2** 起（而不是 4），
     * 窗口环上多一个槽。§1023 的机制是"库的调用链绕环一圈，把跳板的窗口
     * 和 ppTask 自己的窗口当空闲回收"（tr44：第一次调用前 ws=0x15，
     * 返回后变 ws=0x10），多一个槽能显著推迟这次回卷。
     *
     * 同时补上根帧的帧链 `[sp-12]`：`s3_task_tramp` 没有 `entry`，
     * a1 就是恢复出来的任务 sp，链值写 `a1+48`（对齐 IDF SET_STACK 的
     * `sp + SAVE_AREA_OFFSET`），溢出处理体才不会拿脏链算落点。 */
    movi    a10, 48
    add     a10, a10, a1
    addi    a8, a1, -12
    s32i    a10, a8, 0
    mov     a9, a1
    /* ★★★★ §1088（**关键修法**）：`callx8` 会轮转窗口，被调者看到的
     * `a2` 其实是 **tramp 自己的 `a10`**（4 寄存器窗口、call8 轮转 8 个
     * 寄存器 = 2 个窗口；a10 正好落在被调者的 a2 位置）。
     *
     * 而本函数开头是 `mov a12, a2` / `mov a2, a3`：入口放 a12、参数放 a2。
     * 于是库任务（ppTask）启动时，它拿到的第一个参数 = tramp 的 a10
     * —— 可 a10 在上面刚被写成**帧链值 sp+48**！
     *
     * 真板后果（§1085~§1087）：ppTask 一路拿着垃圾参数跑，
     * `_queue_recv` 收到的队列句柄是 0x05 这种小值（EXCVADDR=0x2d）。
     * 补这一句，把真正的参数搬进 a10，被调者的 a2 才拿到它。 */
    mov     a10, a2
    /* §1036 曾在这里试过 `callx4`（只轮转 1 个窗口）。它**确实**让 ppTask 落在
     * WB=1、并且两次调用返回后 a1 不再被破坏（坏 sp 崩溃消失），
     * 但 CALLINC=1 会破坏编译器"调用一律 call8（CALLINC=2）"的假设，
     * 结果在别处出现非法的 `retw.n`（epc1=0x4037d7d6, exccause=0）。
     * **方向已验证（窗口层级是关键），但 callx4 不是合法修法，已撤回。*/
    callx8  a12
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
    .literal .Lwtramp, s3_win_tramp

    .section .text, "ax", @progbits
    .align  4
    .literal_position
    .literal .Lstklo2, 0x3FCA0000
    .literal .Lstkhi2, 0x3FCEF000
    .literal .Lbadsp,  0x500018E0
