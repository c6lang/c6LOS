/* ESP32-S3 call0 上下文：统一用 80 字节陷阱帧保存 a0/a2..a15 + EPC + PS。
 * S3Ctx: r[0..15]@0..60（未用）、sp@64（指向帧）、stack@68、size@72。 */

    .section .text.ctx, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lg_sched, g_sched
    .literal .Lg_cur,   g_s3_cur

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
    rfe
    .endm

    .global port_task_switch
    .type   port_task_switch, @function
port_task_switch:
    /* a2 = 目标任务 ctx */
    mov     a4, a2
    l32r    a3, .Lg_sched

    /* 保存调度器：在调度器栈上压一个陷阱帧，恢复 PC 用 a0（call0 返回地址）。 */
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
    s32i    a1, a3, 64

    l32r    a3, .Lg_cur
    s32i    a4, a3, 0
    l32i    a1, a4, 64
    RESTORE_FRAME

    .global port_task_yield
    .type   port_task_yield, @function
port_task_yield:
    l32r    a4, .Lg_cur
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
    s32i    a0, a1, 60
    rsr     a5, ps
    movi    a6, -32
    and     a5, a5, a6
    s32i    a5, a1, 64
    s32i    a1, a4, 64

    l32r    a3, .Lg_sched
    l32i    a1, a3, 64
    RESTORE_FRAME
1:
    ret

    .global port_ps_get
    .type   port_ps_get, @function
port_ps_get:
    rsr     a2, ps
    ret

    .weak os_task_exit
    .global s3_task_tramp
    .type   s3_task_tramp, @function
s3_task_tramp:
    /* 初始帧把 entry 放在 a2、arg 放在 a3。 */
    mov     a12, a2
    mov     a2, a3
    callx0  a12
    call0   os_task_exit
1:  j       1b
