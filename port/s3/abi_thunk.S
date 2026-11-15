/* 双 ABI 的 libc 小函数：既能被 call0 调用，也能被 windowed 的 call8 调用。
 *
 * 背景：IDF 的 WiFi/PHY 闭源库是 windowed ABI，它们 `call8 memset` 之后
 * 要求返回地址里的窗口标记（bit31，实测崩溃现场 a0 = 0x8203d39c，
 * 正是 0x4203d39c | 0x80000000）被 `retw` 消化掉；而我们自己的代码是
 * call0，必须用 `ret`。用错就跳到 0x8203d39c 这种地址，表现成
 * EXCCAUSE 20（InstrPageTableErr）的 TRAP。
 *
 * 用哪个返回指令可以由 a0 的 bit31 判定（windowed 的返回地址带这一位）。
 * 另外 windowed 调用者还要求：
 *   - 入口执行 `entry`（轮转寄存器窗口）后才能 `retw`，否则窗口对不上；
 *   - a12..a15 属于调用者所有，不能踩。
 * 所以这里两条路径分开：call0 路径存/取 a12..a15 后 `ret`，
 * windowed 路径 `entry` + `retw`（a12..a15 由 entry/retw 天然保护）。
 */

    .section .text, "ax", @progbits

/* ---- 函数体（宏，两条路径各展开一次）----------------------------------- */

    .macro  MEMCPY_BODY
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

    .macro  MEMSET_BODY
    beqz    a4, 9f
    movi.n  a9, 0
1:
    add.n   a10, a2, a9
    s8i     a3, a10, 0
    addi.n  a9, a9, 1
    bltu    a9, a4, 1b
9:
    .endm

    .macro  MEMMOVE_BODY
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

    .macro  MEMCMP_BODY
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

/* ---- 分发：a0 的 bit31 表明调用者用的是 windowed ABI ------------------- */

/* 每个函数：先判 ABI，再走对应路径。
 *
 * 判 ABI 只能用 **a8..a11**：apply `entry` 之前还没有发生窗口轮转，此刻踩的
 * 就是调用者自己的寄存器。windowed 调用者（call8 轮转 8）要求 a4..a7 必须
 * 保留（它们就是被调方的 a12..a15），a0 是返回地址，a1 是栈指针；只有
 * a8..a15 是调用者允许被踩的。call0 下 a8..a11 同样 caller-saved。
 * （之前用 a5 探测，把 wifi_init_process 的 a5 踩成了 1，它随后
 *  `l32i a9, a5, 96` 就以 vaddr=0x61 挂掉。） */

    .align  4
    .global memcpy
    .type   memcpy, @function
memcpy:
    extui   a8, a0, 31, 1
    bnez    a8, .Lmemcpy_win
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12
    MEMCPY_BODY
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 16
    ret
    .align  4
.Lmemcpy_win:
    entry   a1, 16
    MEMCPY_BODY
    retw

    .align  4
    .global memset
    .type   memset, @function
memset:
    extui   a8, a0, 31, 1
    bnez    a8, .Lmemset_win
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12
    MEMSET_BODY
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 16
    ret
    .align  4
.Lmemset_win:
    entry   a1, 16
    MEMSET_BODY
    retw

    .align  4
    .global memmove
    .type   memmove, @function
memmove:
    extui   a8, a0, 31, 1
    bnez    a8, .Lmemmove_win
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12
    MEMMOVE_BODY
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 16
    ret
    .align  4
.Lmemmove_win:
    entry   a1, 16
    MEMMOVE_BODY
    retw

    .align  4
    .global memcmp
    .type   memcmp, @function
memcmp:
    extui   a8, a0, 31, 1
    bnez    a8, .Lmemcmp_win
    addi    a1, a1, -16
    s32i    a12, a1, 0
    s32i    a13, a1, 4
    s32i    a14, a1, 8
    s32i    a15, a1, 12
    MEMCMP_BODY
    l32i    a12, a1, 0
    l32i    a13, a1, 4
    l32i    a14, a1, 8
    l32i    a15, a1, 12
    addi    a1, a1, 16
    ret
    .align  4
.Lmemcmp_win:
    entry   a1, 16
    MEMCMP_BODY
    retw
