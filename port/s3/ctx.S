/* ESP32-S3 call0 上下文切换。
 * S3Ctx: r[0..15] @0..60, sp @64, stack @68, size @72。 */

    .section .text.ctx, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lg_sched, g_sched
    .literal .Lg_cur,   g_s3_cur

    .global port_task_switch
    .type   port_task_switch, @function
port_task_switch:
    /* a2 = 目标任务 ctx */
    mov     a4, a2
    l32r    a3, .Lg_sched
    s32i    a0, a3, 0
    s32i    a1, a3, 4
    s32i    a2, a3, 8
    s32i    a3, a3, 12
    s32i    a4, a3, 16
    s32i    a5, a3, 20
    s32i    a6, a3, 24
    s32i    a7, a3, 28
    s32i    a8, a3, 32
    s32i    a9, a3, 36
    s32i    a10, a3, 40
    s32i    a11, a3, 44
    s32i    a12, a3, 48
    s32i    a13, a3, 52
    s32i    a14, a3, 56
    s32i    a15, a3, 60
    s32i    a1, a3, 64

    /* 记录当前任务 ctx，yield 时才知道往哪保存。 */
    l32r    a3, .Lg_cur
    s32i    a4, a3, 0

    l32i    a0, a4, 0
    l32i    a1, a4, 64
    l32i    a2, a4, 8
    l32i    a3, a4, 12
    l32i    a5, a4, 20
    l32i    a6, a4, 24
    l32i    a7, a4, 28
    l32i    a8, a4, 32
    l32i    a9, a4, 36
    l32i    a10, a4, 40
    l32i    a11, a4, 44
    l32i    a12, a4, 48
    l32i    a13, a4, 52
    l32i    a14, a4, 56
    l32i    a15, a4, 60
    l32i    a4, a4, 16
    ret

    .global port_task_yield
    .type   port_task_yield, @function
port_task_yield:
    l32r    a4, .Lg_cur
    l32i    a4, a4, 0
    beqz    a4, 1f
    s32i    a0, a4, 0
    s32i    a1, a4, 4
    s32i    a2, a4, 8
    s32i    a3, a4, 12
    s32i    a5, a4, 20
    s32i    a6, a4, 24
    s32i    a7, a4, 28
    s32i    a8, a4, 32
    s32i    a9, a4, 36
    s32i    a10, a4, 40
    s32i    a11, a4, 44
    s32i    a12, a4, 48
    s32i    a13, a4, 52
    s32i    a14, a4, 56
    s32i    a15, a4, 60
    s32i    a1, a4, 64
1:
    l32r    a3, .Lg_sched
    l32i    a0, a3, 0
    l32i    a1, a3, 4
    l32i    a2, a3, 8
    l32i    a4, a3, 16
    l32i    a5, a3, 20
    l32i    a6, a3, 24
    l32i    a7, a3, 28
    l32i    a8, a3, 32
    l32i    a9, a3, 36
    l32i    a10, a3, 40
    l32i    a11, a3, 44
    l32i    a12, a3, 48
    l32i    a13, a3, 52
    l32i    a14, a3, 56
    l32i    a15, a3, 60
    l32i    a3, a3, 12
    ret

    .weak os_task_exit
    .global s3_task_tramp
    .type   s3_task_tramp, @function
s3_task_tramp:
    /* a2 = entry, a3 = arg */
    mov     a12, a2
    mov     a2, a3
    callx0  a12
    call0   os_task_exit
1:  j       1b
