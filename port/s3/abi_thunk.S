/* 双 ABI 的 libc 小函数：call0 和 windowed（call8）调用者都能用。
 *
 * ---- 为什么需要 -------------------------------------------------------
 * IDF 的 WiFi/PHY 闭源库是 windowed ABI，它们用 `call8 memset` 调过来；
 * 我们自己的代码是 call0。两套 ABI 的差异（call8 的窗口轮转量是 8）：
 *
 *   调用者的 a10/a11/a12  ==  被调方的 a2/a3/a4     （参数）
 *   调用者的 a4..a7       ==  被调方的 a12..a15     （必须保留）
 *   调用者的 a0/a1        ==  返回地址 / 栈指针     （必须保留）
 *   调用者的 a2,a3,a8..a15                          （可以随便踩）
 *
 * 早先的两个版本都错了：
 *  1) 用 call0 实现（没有 entry/retw），windowed 调用者拿到的返回地址带窗口
 *     标签，跳过去就是 0x8203xxxx → EXCCAUSE 20；
 *  2) 用 `entry a1,16` + `retw`，但在 CALLINC==0 的调用上执行 retw 会抛
 *     IllegalInstruction（cause=0），而且 `entry` 之前踩 a4..a7 同样违规
 *     （曾经踩了 a5，害得 wifi_init_process 的 `l32i a9,a5,96` 以 vaddr=0x61 挂掉）。
 *
 * ---- 现在的做法 -------------------------------------------------------
 * **完全不碰寄存器窗口**：两条路径都不执行 entry，也就都不需要 retw。
 * 用 `rsr a8, ps` 读 PS.CALLINC 判断调用者用了哪套 ABI（call<n> 指令会把它
 * 设成 n/4-1，call0 是 0），然后：
 *
 *   call0 路径：按 call0 约定从 a2/a3/a4 取参，返回放 a2，用 `ret`；
 *   windowed 路径：从 a10/a11/a12 取参，返回写回 **a10**（= 被调方的 a2），
 *                  把 a0 的最高位（窗口标签）抹掉后用 `ret` 返回；
 *                  只用 a2/a3/a8..a15 作临时，a0/a1/a4..a7 一概不碰。
 *
 * 因为从头到尾没轮转过窗口，普通 `ret` 返回后窗口状态和调用者期待的一致。
 */

    .section .text, "ax", @progbits

/* ---- call0 路径的函数体（参数 a2/a3/a4，可用 a5..a11）----------------- */

    .macro  C0_MEMCPY
    beqz    a4, 9f
    movi.n  a9, 0
1:
    add.n   a10, a3, a9
    l8ui    a11, a10, 0
    add.n   a10, a2, a9
    s8i     a11, a10, 0
    addi.n  a9, a9, 1
    bltu    a9, a4, 1b
9:
    .endm

    .macro  C0_MEMSET
    beqz    a4, 9f
    movi.n  a9, 0
1:
    add.n   a10, a2, a9
    s8i     a3, a10, 0
    addi.n  a9, a9, 1
    bltu    a9, a4, 1b
9:
    .endm

    .macro  C0_MEMMOVE
    beqz    a4, 9f
    bgeu    a2, a3, 2f
    movi.n  a9, 0
1:
    add.n   a10, a3, a9
    l8ui    a11, a10, 0
    add.n   a10, a2, a9
    s8i     a11, a10, 0
    addi.n  a9, a9, 1
    bltu    a9, a4, 1b
    j       9f
2:
    add.n   a9, a4, a2
    add.n   a10, a4, a3
3:
    addi.n  a9, a9, -1
    addi.n  a10, a10, -1
    l8ui    a11, a10, 0
    s8i     a11, a9, 0
    bne     a9, a2, 3b
9:
    .endm

    .macro  C0_MEMCMP
    movi.n  a9, 0
    movi.n  a6, 0
1:
    beq     a4, a9, 3f
    add.n   a10, a2, a9
    l8ui    a11, a10, 0
    add.n   a10, a3, a9
    l8ui    a10, a10, 0
    bne     a11, a10, 2f
    addi.n  a9, a9, 1
    j       1b
2:
    sub     a6, a11, a10
3:
    mov     a2, a6
    .endm

/* ---- windowed 路径的函数体（参数 a10/a11/a12，只能动 a2/a3/a8..a15）-- */

    .macro  W_MEMCPY
    mov.n   a8, a10                     /* a8  = dst */
    mov.n   a9, a11                     /* a9  = src */
    beqz    a12, 9f
    movi.n  a13, 0
1:
    add.n   a14, a9, a13
    l8ui    a14, a14, 0
    add.n   a15, a8, a13
    s8i     a14, a15, 0
    addi.n  a13, a13, 1
    bltu    a13, a12, 1b
9:
    mov.n   a10, a8                     /* 返回值 = dst（被调方 a2 = 调用者 a10） */
    .endm

    .macro  W_MEMSET
    mov.n   a8, a10                     /* a8 = dst */
    beqz    a12, 9f
    movi.n  a9, 0
1:
    add.n   a13, a8, a9
    s8i     a11, a13, 0
    addi.n  a9, a9, 1
    bltu    a9, a12, 1b
9:
    mov.n   a10, a8
    .endm

    .macro  W_MEMMOVE
    mov.n   a8, a10                     /* a8 = dst */
    mov.n   a9, a11                     /* a9 = src */
    beqz    a12, 9f
    bgeu    a8, a9, 2f
    movi.n  a13, 0
1:
    add.n   a14, a9, a13
    l8ui    a14, a14, 0
    add.n   a15, a8, a13
    s8i     a14, a15, 0
    addi.n  a13, a13, 1
    bltu    a13, a12, 1b
    j       9f
2:
    add.n   a13, a8, a12
    add.n   a14, a9, a12
3:
    addi.n  a13, a13, -1
    addi.n  a14, a14, -1
    l8ui    a15, a14, 0
    s8i     a15, a13, 0
    bne     a13, a8, 3b
9:
    mov.n   a10, a8
    .endm

    .macro  W_MEMCMP
    mov.n   a8, a10                     /* a8 = a */
    mov.n   a9, a11                     /* a9 = b */
    movi.n  a13, 0
    movi.n  a14, 0
1:
    beq     a12, a13, 3f
    add.n   a15, a8, a13
    l8ui    a10, a15, 0
    add.n   a15, a9, a13
    l8ui    a15, a15, 0
    bne     a10, a15, 2f
    addi.n  a13, a13, 1
    j       1b
2:
    sub     a14, a10, a15
3:
    mov.n   a10, a14                    /* 返回值 */
    .endm

/* ---- 骨架宏 ------------------------------------------------------------ */

/* 读 PS.CALLINC 分发；只用 a8 暂存（两种 ABI 下调用者都允许踩 a8）。 */
    .macro  ABI_DISPATCH win_label
    rsr     a8, ps
    extui   a8, a8, 16, 2
    bnez    a8, \win_label
    .endm

/* 抹掉 a0 的窗口标签（只有 windowed 的返回地址带这一位）后返回 */
/* CALL<n> 把轮转量写进 a0 的 bit31:30（call8 → 2，即轮转 8/4 个寄存器），
 * 覆盖掉地址原来的最高两位。Xtensa 的代码地址都在 0x4000_0000~0x7FFF_FFFF，
 * 最高两位恒为 0b01，所以还原就是：清掉 bit31:30，再补回 0b01。
 * 实测 0x8203d39c -> 0x4203d39c。*/
    .macro  WIN_RETURN
    movi    a8, -1
    srli    a8, a8, 2           /* a8 = 0x3FFFFFFF */
    and     a0, a0, a8
    movi    a9, 1
    slli    a9, a9, 30          /* a9 = 0x40000000 */
    or      a0, a0, a9
    ret
    .endm

/* call0 路径要存/取 a12..a15（call0 下它们是 callee-saved） */
    .macro  C0_PROLOG
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12
    .endm

    .macro  C0_EPILOG
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 16
    ret
    .endm

    .align  4
    .global memcpy
    .type   memcpy, @function
memcpy:
    ABI_DISPATCH .Lmemcpy_win
    C0_PROLOG
    C0_MEMCPY
    C0_EPILOG
    .align  4
.Lmemcpy_win:
    W_MEMCPY
    WIN_RETURN

    .align  4
    .global memset
    .type   memset, @function
memset:
    ABI_DISPATCH .Lmemset_win
    C0_PROLOG
    C0_MEMSET
    C0_EPILOG
    .align  4
.Lmemset_win:
    W_MEMSET
    WIN_RETURN

    .align  4
    .global memmove
    .type   memmove, @function
memmove:
    ABI_DISPATCH .Lmemmove_win
    C0_PROLOG
    C0_MEMMOVE
    C0_EPILOG
    .align  4
.Lmemmove_win:
    W_MEMMOVE
    WIN_RETURN

    .align  4
    .global memcmp
    .type   memcmp, @function
memcmp:
    ABI_DISPATCH .Lmemcmp_win
    C0_PROLOG
    C0_MEMCMP
    C0_EPILOG
    .align  4
.Lmemcmp_win:
    W_MEMCMP
    WIN_RETURN
