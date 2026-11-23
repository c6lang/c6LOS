/* call0 <-> windowed 的桥。只有这里允许跨越 ABI，规则：
 *
 *   call0 -> windowed：
 *     参数搬进**调用者的 a10..a15**（windowed 的被调用者看到的是自己的 a2..a7），
 *     `call8` 进，返回后返回值在调用者的 a10（= callee 的 a2）。
 *     注意 call0 里 a12..a15 是 callee-saved，用之前必须自己保存。
 *     call8 需要 PS.WOE=1，这里临时打开、返回后恢复。
 *
 *   windowed -> call0：
 *     windowed 函数进来时参数已在它自己的 a2..a7（就是调用者的 a10..a15），
 *     直接 `call0` 目标即可（call0 被调用者也是从 a2..a7 读参数），
 *     但要把 a0（返回地址）先存起来，调完再放回 a0，最后 `retw` 转回窗口。
 */

    .section .text, "ax", @progbits

/* §879: 全 windowed 构建下，这些 call0→windowed 桥被 windowed 代码用 call8 调用，
 * a0 带 CALLINC 标记；必须用 retw 回退窗口，否则 ret 跳到带标记地址 → cause=0。 */
#ifdef S3_ONE_ABI
#define BRIDGE_RET retw
#else
#define BRIDGE_RET ret
#endif

/* u32 s3_win_call_add(u32 a, u32 b) —— 从 call0 调 windowed 的 s3_win_add() */
    .align  4
    .global s3_win_call_add
    .type   s3_win_call_add, @function
s3_win_call_add:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18            /* PS.WOE */
    or      a12, a12, a13
    wsr     a12, ps
    rsync

    mov     a10, a2                 /* windowed 的参数 0 */
    mov     a11, a3                 /* windowed 的参数 1 */
    call8   s3_win_add
    mov     a2, a10                 /* 返回值搬回 call0 的 a2 */

    l32i    a12, a1, 20             /* 恢复 PS（含 WOE 原值） */
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_phy_cal(void) —— 从 call0 调 windowed 孤岛的 s3_win_phy_cal() */
    .align  4
    .global s3_win_call_phy_cal
    .type   s3_win_call_phy_cal, @function
s3_win_call_phy_cal:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_phy_cal
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_deep(u32 n) —— 从 call0 调 windowed 的深递归函数 */
    .align  4
    .global s3_win_call_deep
    .type   s3_win_call_deep, @function
s3_win_call_deep:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    call8   s3_win_deep
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_flash_add(u32 a, u32 b) —— 调放在 flash 段里的 windowed 函数 */
    .align  4
    .global s3_win_call_flash_add
    .type   s3_win_call_flash_add, @function
s3_win_call_flash_add:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    mov     a11, a3
    /* 注意：IRAM→IROM 的 call8 超出 ±128KB 范围，所以这里只调 IRAM 里的
     * 包装函数，跨到 flash 的那一步交给编译器（它会用 l32r+callx8）。 */
    call8   s3_win_flash_chain
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_flash_pat(void) —— 取 flash 段里图案数组的地址 */
    .align  4
    .global s3_win_call_flash_pat
    .type   s3_win_call_flash_pat, @function
s3_win_call_flash_pat:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    rsr     a13, ps
    s32i    a13, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_flash_pat
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_flash_addr(void) —— 取 flash 里那个函数的地址（验证 call 目标） */
    .align  4
    .global s3_win_call_flash_addr
    .type   s3_win_call_flash_addr, @function
s3_win_call_flash_addr:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_flash_addr
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_flash_leaf(u32 a) —— 调 flash 里的叶子函数 */
    .align  4
    .global s3_win_call_flash_leaf
    .type   s3_win_call_flash_leaf, @function
s3_win_call_flash_leaf:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    call8   s3_win_flash_leaf_chain
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* ---- 放在 flash 段里的手写 windowed 叶子函数 ----
 * 必须成对写 `entry` / 宽 `retw`（§202 真板实测）：
 *   - 只有 retw 没有 entry：`call8` 把 CALLINC=2 写进 a0，retw 会按 CALLINC
 *     把窗口"退"回去，但根本没进过窗口 → 触发窗口欠载/非法指令
 *     （签名：cause=28、pc=retw.n、a0 带 0x80xxxxxx 标签）。
 *   - 窄 retw.n 在需要欠载填充时是非法指令（§196 结论），所以这个文件用
 *     -Wa,--no-transform 编译，保证 retw 不被收窄。 */
    .section .flash.text, "ax", @progbits
    .align  4
    .global s3_asm_flash_42
    .type   s3_asm_flash_42, @function
s3_asm_flash_42:
    entry   a1, 16
    movi    a2, 42
    retw

/* int s3_wpa_cb_stub(void) —— 窗口化的"空回调"桩：返回 0。
 * 闭源 WiFi 库的 WPA 回调表（g_cnxMgr+0x1b4）里全是函数指针，我们注册的
 * 全零表会让它 callx8 到 NULL（§279）。把每个槽都填成这个桩，唯独把
 * `+0x48` 那一格留 0（`sta_is_wpa3_enabled()` 是把它当布尔量读的，非 0 就
 * 会以为要上 WPA3/SAE）。 */
    .align  4
    .global s3_wpa_cb_stub
    .type   s3_wpa_cb_stub, @function
s3_wpa_cb_stub:
    entry   a1, 16
    movi    a2, 0
    retw

    .section .text, "ax", @progbits
/* u32 s3_win_call_flash42(void) —— 从 call0 调上面那个 flash 里的手写函数 */
    .align  4
    .global s3_win_call_flash42
    .type   s3_win_call_flash42, @function
s3_win_call_flash42:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_flash42_chain
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_phy_enable(void) —— 从 call0 调 windowed 的 PHY 初始化 */
    .align  4
    .global s3_win_call_phy_enable
    .type   s3_win_call_phy_enable, @function
s3_win_call_phy_enable:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_phy_enable
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_wifi_main(void) —— call0 任务里跑整条 WiFi 主流程（§241）
 *
 * 动机：让**只有闭源库自己那个 ppTask** 是 windowed 任务。我们自己的任务保持
 * call0（call0 代码不轮转窗口），这样"两个 windowed 任务互相切换"这个还没解决的
 * 难题就被绕开了 —— 窗口文件只被这一条 windowed 调用链和库自己的任务使用。 */
    .align  4
    .literal_position
    .literal .Lwmain, s3_win_wifi_main
    .literal .Lbridge_sp, g_win_bridge_sp
    .global s3_win_call_wifi_main
    .type   s3_win_call_wifi_main, @function
s3_win_call_wifi_main:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    /* §416（真 bug）：**本函数是 call0 的（用 `ret` 返回），绝不能 `callx8`**。
     *
     * `callx8` 会把寄存器窗口轮转 8 个：返回之后 `a1` 已经是**被调者窗口**
     * 里的值，后面的 `l32i a12, a1, 20` / `addi a1, a1, 32` / `ret` 全在错的
     * 窗口里执行。真板症状：`s3_win_call_wifi_main` 返回后
     * `wifi_call0_task` 的 `a1` 变成 0（`epc1` 正是 `call0` 的下一条指令）。
     *
     * `call8`/`callx8` 必须配 `retw`（windowed 函数）；`ret` 是 call0 的。
     * 目标在 flash（0x42xxxxxx），`call8` 的 128KB 相对位移够不着，
     * 所以先 `call8` 到**本文件内的 windowed 跳板**（`entry` 开场、`retw` 收尾，
     * 见 §412/§413），由它去 `callx8` 远端 —— 那时窗口已经轮转好了。 */
    /* §417（真 bug，第 2 次修）：**`callx8`/`call8` 之后 `a1` 已经轮转**。
     *
     * 本函数是 call0 的（`ret` 返回、`addi a1,a1,-32` 开帧），却要调
     * windowed 的 `s3_win_wifi_main`。`callx8`/`call8` 会把寄存器窗口轮转
     * ⇒ 返回之后**当前窗口里的 a1 不再是我们那个帧基址**
     *（轮转规则：壳的 a8..a15 = 调用者的 a0..a7 ⇒ 壳的 a9 = 调用者的 a1）。
     * 真板症状：返回后 `wifi_call0_task` 的 a1 变成 0。
     *
     * 修法：**把帧基址放在 a9**（= 壳里的 a1），调用回来后从 a9 里取回来。
     * `mov a1, a9` 在返回后的旧窗口里执行，而旧窗口的 a9 是我们自己写的值
     *（调用者 a1 在壳里是 a1，调用者 a9 不受 call8 的转义影响）。 */
    /* §418：**a 寄存器全都不可靠** —— `call8` 轮转 8 个，而
     * `callx8` 的 CALLINC 只让硬件把被调者的 a0..a3 存在溢出槽里，
     * 我们窗口里的 a4..a15 会被被调者当成自己的 a12..a7 用掉。
     * 试过 a9（壳里它是 a1）也失败。
     * ⇒ 帧基址只能放**内存**。借 `g_win_bridge_sp` 这个专用全局
     * （本函数只在 wifi 主任务里被调一次，不会重入）。 */
    l32r    a9, .Lbridge_sp
    s32i    a1, a9, 0
    /* §488 ★★ 进岛前把 WINDOWSTART **归一化到只剩当前窗口**。
     *
     * 真板实测：wifi 任务的保存帧里 `WS=0x5001`、`WB=0` —— 即除了当前窗口 0
     * 之外，12、14 两个"幽灵活窗口"还挂着。这些脏 bit 是以前 windowed 往返
     * 留下的（`retw` 只在正常返回时清位；我们有一次返回路径不正常）。
     *
     * 后果是致命的：硬件认为那几个窗口还活着，于是稍微深一点的 `call8`
     * 就真的撞上**窗口溢出向量**，而 `_win_ovf8/12` 要靠 `[sp-12]` 这条
     * **帧链**去找溢出槽 —— 我们的 call0 帧没有这条链（那不是 `entry` 开的帧），
     * 于是溢出写到野地址、欠载从野地址填回来，`a0` 立刻变垃圾。
     * 症状：`esp_wifi_init_internal` 里"整个调用链莫名其妙退回到岛外面"
     *（`WSTAGE` 停在 0x1005 = 还在 bringup 里，但 wifi 任务已经跑到
     * `s3_task_tramp` 收尾、被 `os_task_exit` 收成 FREE），以及
     * §485 的 `EPC=0x3fcef000`。
     *
     * 归一化的写法：只置 `1 << WINDOWBASE` 这一位（不写 WINDOWBASE 本身，
     * 避免重映射把 aN 换掉）。本函数是 call0 代码，祖先窗口里没有 windowed
     * 帧，所以清掉其余位是无损的。 */
    rsr     a9, windowbase
    ssl     a9
    movi    a10, 1
    sll     a10, a10
    wsr     a10, windowstart
    isync
    l32r    a10, .Lwmain
    /* §487 ★ 一次 `callx8` 直接进岛，**不要** `call8 跳板 + callx8 岛` 两跳。
     *
     * 寄存器文件只有 64 个寄存器 = **8 个窗口**，每个 `call8`/`callx8`
     * 占掉 2 个。走两跳等于把 4 个窗口花在"进门"上，闭源库自己的调用链
     * 只剩 4 个窗口 —— 真板实测中断现场 `WINDOWSTART=0x1555`
     * （0/2/4/6/8/10/12 七个窗口全活着）就已经贴上上限了，再深一点就撞
     * 窗口溢出；溢出/欠载处理体要用 `[sp-12]` 这条**帧链**，而我们的
     * call0 帧没有这条链 ⇒ 填回来的 a0 是垃圾。
     * 症状就是 §480（任务"自己返回了" → 被 `os_task_exit` 收成 FREE）和
     * §485（`EPC=0x3fcef000` 这种"跳到 DRAM 顶"的取指错误）。
     *
     * `callx8` 的目标在寄存器里，位移不受 ±128KB 限制，直接够得到 flash
     * 里的 `s3_win_wifi_main`，跳板本来就没必要。参数/返回值仍走 a10
     *（call8 之后它就是被调者的 a2）。 */
    callx8  a10
    mov     a2, a10
    l32r    a9, .Lbridge_sp
    l32i    a1, a9, 0            /* 从内存取回帧基址 */
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* §416：windowed 跳板。被 `call8` 调（硬件完成窗口轮转），再 `callx8`
 * 远端 windowed 入口。`entry` 建立 CALLINC 记账，`retw` 才能正确退回去。
 * 参数与返回值都经 a10 透传（call8 后 a10 就是被调者的 a2，正好是第一个参数）。 */
    .align  4
    .type   .Lwmain_stub, @function
.Lwmain_stub:
    /* call8 轮转：壳的 a0..a7 = 调用者的 a8..a15，壳的 a8..a15 = 调用者的 a0..a7。
     * 调用者把目标地址放在 a10 ⇒ 壳里是 **a2**。
     * `callx8` 之后目标在**新窗口**的 a0..a7 = 壳的 a8..a15，
     * 返回值在目标的 a2 ⇒ 壳的 **a10**。
     * 所以要：从 a2 取地址 → callx8 → 从 a10 取返回值 → 放回 a2 交还调用者。 */
    entry   a1, 16
    s32i    a0, a1, 0
    /* §419（真 bug，第 3 次修）：**必须在 `callx8` 前保存 PS、之后恢复**。
     *
     * `retw` 靠 PS.CALLINC 决定往回轮转几个窗口。被调者自己的 `retw`
     * 会把 CALLINC **清零**，跳板再 `retw` 就轮转 0 个窗口 —— 于是返回到
     * **跳板的窗口**而不是调用者的窗口，`a1` 全套都是错的（真板症状：
     * `s3_win_call_wifi_main` 返回后 `wifi_call0_task` 的 `a1` = 0，
     * 中断进来后 sp 闸门报 `0xBAD4`）。
     *
     * 这一步和 §412/§413 在 ctx.S 里踩的是同一个坑；那边跳板之后不再依赖
     * 窗口状态所以侥幸没事，这里必须显式把 CALLINC 恢复成 2。 */
    mov     a8, a2
    /* §476/§478：`callx8` 是**正确的**！
     * `win_island.c` 用 `-mabi=windowed` 编译（见 Makefile:242），
     * 所以 `s3_win_wifi_main` 的调用约定是 windowed：它内部用 `call8`/`callx8`
     * 调别的函数、末尾用 `retw` 返回、栈参数在 `a1+16` 起。
     * 这里必须用 `callx8`，返回值在调用者的 `a10`。 */
    callx8  a8
    mov     a2, a10
    l32i    a0, a1, 0
    retw

/* u32 s3_win_call_phy_cal_word(u32 i) —— 读回校准数据里的一个 32bit 字 */
/* §479 void s3_win_puts(const char *s) —— windowed 调 call0 的 port_puts。
 * windowed 进来时参数已在 a2；要把 a0 先存起来，call0 返回后放回、再 retw。 */
    .align  4
    .literal_position
    .literal .Lputs, port_puts
    .global s3_win_puts
    .type   s3_win_puts, @function
s3_win_puts:
    /* §487：帧必须 ≥ 48。`entry a1,N` 在 CALLINC=2 时会把 a0..a3 存到
     * [a1+N-16, a1+N-4]、a4..a7 存到 [a1+N-32, a1+N-20] —— N=16 时
     * a4..a7 的槽落在**帧外**（[a1-16,a1-4)），虽然只是临时区，但这个
     * 壳被 windowed 岛在任意深度调用，溢出/欠载处理体会把 [sp-32,sp)
     * 当成溢出槽；帧外的槽位不属于任何帧，链就断了。N=48 时整个溢出区
     * 都在自己的帧里（[a1+16,a1+48)），我们只用 [a1+0,a1+16)。 */
    entry   a1, 48
    s32i    a0, a1, 0
    l32r    a8, .Lputs
#ifdef S3_ONE_ABI
    /* §880: 全 windowed 构建里 port_puts 是 windowed 函数，必须用 callx8；
     * 参数搬到 a10（call8 轮转后 callee 的 a2 = 调用者的 a10）。 */
    mov     a10, a2
    callx8  a8
#else
    callx0  a8
#endif
    l32i    a0, a1, 0
    retw

    .align  4
    .global s3_win_call_phy_cal_word
    .type   s3_win_call_phy_cal_word, @function
s3_win_call_phy_cal_word:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    call8   s3_win_phy_cal_word
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_deep12(u32 n) —— 验 call12 的窗口溢出/欠载 */
    .align  4
    .global s3_win_call_deep12
    .type   s3_win_call_deep12, @function
s3_win_call_deep12:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    call8   s3_win_deep12_entry
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* void s3_win_call_log(const char *s) —— 从 call0 让 ROM 打印一行 */
    .align  4
    .global s3_win_call_log
    .type   s3_win_call_log, @function
s3_win_call_log:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    call8   s3_win_log
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_phy_step(int step) */
    .align  4
    .global s3_win_call_phy_step
    .type   s3_win_call_phy_step, @function
s3_win_call_phy_step:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    mov     a10, a2
    call8   s3_win_phy_step
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_phy_funcs(void) —— 读 g_phyFuns 的内容 */
    .align  4
    .global s3_win_call_phy_funcs
    .type   s3_win_call_phy_funcs, @function
s3_win_call_phy_funcs:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_phy_funcs_word
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_cache_enable(void) —— 从 call0 调 windowed 孤岛，再由孤岛调
 * ROM 的 Cache_Enable_ICache(0x40001878)。ROM 是 windowed ABI，必须走这条路。 */
    .align  4
    .global s3_win_call_cache_enable
    .type   s3_win_call_cache_enable, @function
s3_win_call_cache_enable:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_enable
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_cache_invalidate(void) */
    .align  4
    .global s3_win_call_cache_invalidate
    .type   s3_win_call_cache_invalidate, @function
s3_win_call_cache_invalidate:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_invalidate
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_irom_mmu_end(void) */
    .align  4
    .global s3_win_call_irom_mmu_end
    .type   s3_win_call_irom_mmu_end, @function
s3_win_call_irom_mmu_end:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_irom_mmu_end
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_drom_mmu_end(void) */
    .align  4
    .global s3_win_call_drom_mmu_end
    .type   s3_win_call_drom_mmu_end, @function
s3_win_call_drom_mmu_end:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_drom_mmu_end
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_cache_invalidate_addr(u32 addr, u32 size) */
    .align  4
    .global s3_win_call_cache_invalidate_addr
    .type   s3_win_call_cache_invalidate_addr, @function
s3_win_call_cache_invalidate_addr:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_invalidate_addr
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* s3_win_call_ache_suspend_icache */
    .align  4
    .global s3_win_call_ache_suspend_icache
    .type   s3_win_call_ache_suspend_icache, @function
s3_win_call_ache_suspend_icache:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_suspend_icache
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* s3_win_call_ache_resume_icache */
    .align  4
    .global s3_win_call_ache_resume_icache
    .type   s3_win_call_ache_resume_icache, @function
s3_win_call_ache_resume_icache:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_resume_icache
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET
/* s3_win_call_cache_suspend_icache */
    .align  4
    .global s3_win_call_cache_suspend_icache
    .type   s3_win_call_cache_suspend_icache, @function
s3_win_call_cache_suspend_icache:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_suspend_icache
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET
/* s3_win_call_cache_resume_icache */
    .align  4
    .global s3_win_call_cache_resume_icache
    .type   s3_win_call_cache_resume_icache, @function
s3_win_call_cache_resume_icache:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cache_resume_icache
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_config_icache_mode(u32 size, u32 ways, u32 line) */
    .align  4
    .global s3_win_call_config_icache_mode
    .type   s3_win_call_config_icache_mode, @function
s3_win_call_config_icache_mode:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_config_icache_mode
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_wifi_init(void *cfg) */
    .align  4
    .global s3_win_call_wifi_init
    .type   s3_win_call_wifi_init, @function
s3_win_call_wifi_init:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_wifi_init
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_osi_init(void) —— 初始化 OSI 表并返回其指针 */
    .align  4
    .global s3_win_call_osi_init
    .type   s3_win_call_osi_init, @function
s3_win_call_osi_init:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_osi_init
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_fn1(u32 fn, u32 arg) */
    .align  4
    .global s3_win_call_fn1
    .type   s3_win_call_fn1, @function
s3_win_call_fn1:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_call1
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* 提供一个 .flash.rodata 里的数据对象：确保镜像里**总有 DROM 段**。
 * （bootloader 的 mmu_hal_map_region 在没有 DROM 段时会 assert 失败。） */
    .section .flash.rodata, "a", @progbits
    .align  4
    .global s3_flash_rodata_probe
    .type   s3_flash_rodata_probe, @object
s3_flash_rodata_probe:
    .word   0x12345678
    .size   s3_flash_rodata_probe, 4

/* void s3_win_call_osi_dump(void) —— 从 call0 调 windowed 的诊断输出 */
    .section .text, "ax", @progbits
    .align  4
    .global s3_win_call_osi_dump
    .type   s3_win_call_osi_dump, @function
s3_win_call_osi_dump:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_osi_dump
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_cfg_ptr(void) —— 从 call0 取 windowed 侧分配的地址 */
    .section .text, "ax", @progbits
    .align  4
    .global s3_win_call_cfg_ptr
    .type   s3_win_call_cfg_ptr, @function
s3_win_call_cfg_ptr:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_cfg_ptr
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_calls_ptr(void) —— 从 call0 取 windowed 侧分配的地址 */
    .section .text, "ax", @progbits
    .align  4
    .global s3_win_call_calls_ptr
    .type   s3_win_call_calls_ptr, @function
s3_win_call_calls_ptr:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_calls_ptr
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_bringup(void) */
    .section .text, "ax", @progbits
    .align  4
4:  .word   g_wb_in
    .align  4
5:  .word   g_wb_out
    .align  4
6:  .word   0x50001100
    .align  4
    .global s3_win_call_bringup
    .type   s3_win_call_bringup, @function
s3_win_call_bringup:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    /* 诊断：桥进入计数 -> RTC 0x50001100 */
    l32r    a14, 4b
    l32i    a15, a14, 0
    addi    a15, a15, 1
    s32i    a15, a14, 0
    l32r    a14, 6b
    s32i    a15, a14, 0
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_bringup
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    /* 诊断：桥离开计数 -> RTC 0x50001104 */
    l32r    a14, 5b
    l32i    a15, a14, 0
    addi    a15, a15, 1
    s32i    a15, a14, 0
    l32r    a14, 6b
    s32i    a15, a14, 4
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* u32 s3_win_call_wifi_start(void) —— 调 esp_wifi_start() */
    .section .text, "ax", @progbits
    .align  4
    .global s3_win_call_wifi_start
    .type   s3_win_call_wifi_start, @function
s3_win_call_wifi_start:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_win_wifi_start
    mov     a2, a10
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET

/* §642: void s3_win_call_wifi_isr(void) —— 从 call0 的 level-1 ISR 调 windowed 的库 ISR */
    .align  4
    .global s3_win_call_wifi_isr
    .type   s3_win_call_wifi_isr, @function
s3_win_call_wifi_isr:
    addi    a1, a1, -32
    s32i    a0, a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    rsr     a12, ps
    s32i    a12, a1, 20
    /* §704: call8 旋转 windowbase 8 个寄存器，但 call0 调用者返回时不会
     * 自动恢复。原来用 `ret` 返回 —— windowbase 每次 ISR 泄漏 2，真板 JTAG
     * 实测 windowbase=0xe、返回 call0 的 port_idle 里 a0 读到垃圾掉进
     * _vec_default。必须显式保存并在 call8 后恢复 windowbase。 */
    rsr     a12, windowbase
    s32i    a12, a1, 24
    movi    a13, 1
    slli    a13, a13, 18
    or      a12, a12, a13
    wsr     a12, ps
    rsync
    call8   s3_wifi_isr_dispatch
    l32i    a12, a1, 24
    wsr     a12, windowbase
    rsync
    l32i    a12, a1, 20
    wsr     a12, ps
    rsync
    l32i    a0, a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    BRIDGE_RET
