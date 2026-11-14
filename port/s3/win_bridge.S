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
