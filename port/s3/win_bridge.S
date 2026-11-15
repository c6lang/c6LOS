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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

/* ---- 放在 flash 段里的手写函数（没有 entry，只有 movi+retw） ---- */
    .section .flash.text, "ax", @progbits
    .align  4
    .global s3_asm_flash_42
    .type   s3_asm_flash_42, @function
s3_asm_flash_42:
    movi    a2, 42
    retw.n

/* 决定性测试：flash 里的函数**带 entry**（会分配寄存器窗口） */
    .align  4
    .global s3_asm_flash_entry
    .type   s3_asm_flash_entry, @function
s3_asm_flash_entry:
    entry   a1, 16
    movi    a2, 77
    retw.n

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
    ret

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
    ret

/* u32 s3_win_call_phy_cal_word(u32 i) —— 读回校准数据里的一个 32bit 字 */
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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

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
    ret
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
    ret
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
    ret

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
    ret

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
    ret

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
    ret

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
    ret

/* u32 s3_win_call_entry_test(void) —— 从 call0 调上面那个 flash 里的手写函数 */
    .align  4
    .global s3_win_call_entry_test
    .type   s3_win_call_entry_test, @function
s3_win_call_entry_test:
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
    call8   s3_win_asm_entry
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
    ret

