/* windowed → call0 的 trampoline。
 *
 * 我们的内核（kernel/*.c6 生成的 C）是 **call0** ABI，而 port/s3/wifi_glue.c
 * 是 `-mabi=windowed`（必须的，见 docs/wifi-plan.md §120）。windowed 代码直接
 * 调 call0 函数会同时踩两个坑：
 *
 *   1) call<n> 把窗口轮转量写进 a0 的 bit31:30，call0 返回用的 `ret`
 *      （= jx a0）会跳到 0x82xxxxxx 这种地址；
 *   2) call0 被调方不 `entry`、不轮转窗口，直接作用在调用者的物理寄存器上。
 *
 * 所以每个内核函数配一个薄壳：`entry` 建立自己的窗口 → 调内核 → `retw`
 * 正常回到 windowed 调用者。
 *
 * 两个细节：
 *   * 目标可能被链接脚本放进 flash（0x42xxxxxx），离 IRAM 超过 `call0` 的
 *     ±512KB 范围，所以用 `l32r` + `callx0`；
 *   * 内层调用会把 a0 改成壳内部的返回地址，而 `retw` 要用带窗口标签的那个，
 *     所以在壳自己的栈帧里（entry 之外再多要 16 字节）存一份再取回来。
 *
 * 参数/返回值完全透传（call0 和 windowed 的 a2..a7 位置一样）。
 */

    .section .text, "ax", @progbits

    .align  4
7:  .word   0x50000F08   /* 诊断区：记录 **库** 调用 _queue_recv 的返回地址 */
    .align  4
6:  .word   0x50000F00   /* 诊断区 2：s3_w2c_queue_get 的调用者 */
    .align  4
12: .word   g_win_spilled  /* "切走前 spill 过"标志（见 ctx.c / ctx.S） */

    .macro  W2C name, target
    /* l32r 只能往前引用，所以字面量必须放在指令之前；用数字标签 9 保证
     * 每次展开都指向紧邻自己的那一个。 */
    .align  4
9:  .word   \target
    .align  4
    .global \name
    .type   \name, @function
\name:
    /* entry a1,48 里前 32 字节是留给 call8 的窗口 spill 区，**不能**拿来
     * 存自己的东西：硬件欠载/溢出会往那里面写。局部量从 a1+32 开始。
     * 之前写成 entry 16 + addi -16，保存槽正好压进 spill 区，真板症状是
     * 欠载时读到 excvaddr=0xfffffff5（帧指针被踩坏）。 */
    entry   a1, 48
    s32i    a0, a1, 32           /* 存下带窗口标签的返回地址 */
    l32r    a8, 9b
    callx0  a8
    l32i    a0, a1, 32
    retw
    .endm


/* ---- 把祖先窗口整圈 spill 到栈上 -------------------------------------
 * 转 16 个窗口（64 aregs / 4）正好一圈，WINDOWBASE 回到原值 —— 官方
 * xt_asm_utils.h 的 SPILL_ALL_WINDOWS 做法（这颗芯片上不能写 WINDOWBASE）。
 * 转完只有当前窗口 live，所以恢复时把 WINDOWSTART 归成 1<<windowbase 是
 * 唯一正确的选择 —— 但**必须**跟这里配对（见 ctx.c 的 g_win_spilled）。
 * `and a12,a12,a12` 是占位：写回自身，即使被溢出到栈上也仍然正确。 */
    .macro  WIN_SPILL8
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
    l32r    a9, 12b
    movi    a10, 1
    s32i    a10, a9, 0          /* g_win_spilled = 1 */
    .endm

/* 带调用点追踪的版本：在 entry 之后记录 a0（此刻它就是 windowed 调用者的
 * 真实返回地址），存进 RTC 0x50000F00 开始的环（每项 2 字：返回地址、计数）。 */
    .macro  W2C_TRACE name, target
    .align  4
9:  .word   \target
    .align  4
    .global \name
    .type   \name, @function
\name:
    entry   a1, 48
    s32i    a0,  a1, 32
    s32i    a12, a1, 36
    s32i    a13, a1, 40
    l32r    a12, 6b
    l32i    a13, a12, 4
    s32i    a0, a12, 0
    addi    a13, a13, 1
    s32i    a13, a12, 4
    l32r    a8, 9b
    callx0  a8
    l32i    a0,  a1, 32
    l32i    a12, a1, 36
    l32i    a13, a1, 40
    retw
    .endm

/* ---- 信号量 / 互斥 ---- */
    W2C s3_w2c_sem_init,       os_sem_init
    W2C s3_w2c_sem_destroy,    os_sem_destroy
    W2C s3_w2c_sem_pend,       os_sem_pend
    W2C s3_w2c_sem_post,       os_sem_post

/* ---- 递归互斥（内核有 owner 追踪，IDF 的 API 锁需要它）---- */
    W2C s3_w2c_mutex_init,     os_mutex_init
    W2C s3_w2c_mutex_destroy,  os_mutex_destroy
    W2C s3_w2c_mutex_pend,     os_mutex_pend
    W2C s3_w2c_mutex_post,     os_mutex_post

/* ---- 队列 ---- */
    W2C s3_w2c_queue_init,     os_queue_init_ex
    W2C s3_w2c_queue_put,      os_queue_put_item
    W2C_TRACE s3_w2c_queue_get, os_queue_get_item
    W2C s3_w2c_queue_count,    os_queue_count
    W2C s3_w2c_queue_try_get,  os_queue_try_get_item
    W2C s3_w2c_queue_try_put,  os_queue_try_put_item
    W2C s3_w2c_queue_destroy,  os_queue_destroy

/* ---- 事件组 ---- */
    W2C s3_w2c_event_init,     os_event_init
    W2C s3_w2c_event_set,      os_event_set
    W2C s3_w2c_event_clear,    os_event_clear
    W2C s3_w2c_event_wait,     os_event_wait
    W2C s3_w2c_event_destroy,  os_event_destroy
    W2C s3_w2c_event_get,      os_event_get

/* ---- 任务 / 时间 ---- */
    W2C s3_w2c_task_create,    os_task_create
    W2C s3_w2c_task_delete,    os_task_delete
    W2C s3_w2c_task_suspend,   os_task_suspend
    W2C s3_w2c_task_resume,    os_task_resume
    W2C s3_w2c_task_delay,     os_task_delay
    W2C s3_w2c_task_yield,     os_task_yield
    W2C s3_w2c_tick_get,       os_tick_get
    W2C s3_w2c_task_probe,     os_task_probe
    W2C s3_w2c_tick_hz,        os_tick_hz
    W2C s3_w2c_current_task,   os_task_current_slot
    W2C s3_w2c_slot_of_id,     os_task_slot_of_id


/* 在 osi_queue_recv 入口处记录 **库** 的返回地址（windowed 的 call8 值），
 * 用来定位到底是库里的哪个函数在收队列。
 * 参数按 windowed 约定在 a2/a3/a4，转发给 C 侧要用 a10/a11/a12；
 * a12/a13 属于调用者（callee-saved），必须先存栈再借用。 */
    .section .text, "ax", @progbits
    .align  4
11: .word   0x50001560
    .align  4
10: .word   g_lra_seq   /* 壳**自己**的序号：以前和函数体共用 g_recv_seq，
                         双方的写入会互相覆盖，导致 task1 的返回地址被冲掉 */
    .align  4
8:  .word   osi_queue_recv_body
    .align  4
    .global osi_queue_recv
    .type   osi_queue_recv, @function
osi_queue_recv:
    entry   a1, 48
    s32i    a12, a1, 36
    s32i    a13, a1, 40
    /* 按调用序号把库侧返回地址存进 4 个槽（0x50000F08+4*i） */
    l32r    a8, 10b
    l32i    a9, a8, 0
    addi    a10, a9, 1
    s32i    a10, a8, 0
    extui   a9, a9, 0, 2
    slli    a9, a9, 2
    l32r    a8, 7b
    add     a8, a8, a9
    s32i    a0, a8, 0
    s32i    a0, a1, 32
    mov.n   a10, a2
    mov.n   a11, a3
    mov.n   a12, a4
    l32r    a9, 11b
    s32i    a0, a9, 0            /* 诊断：callx8 之前的 a0 */
    l32r    a8, 8b
    callx8  a8
    l32r    a9, 11b
    s32i    a0, a9, 4            /* 诊断：callx8 之后的 a0 */
    mov.n   a2, a10
    l32i    a0,  a1, 32
    l32i    a12, a1, 36
    l32i    a13, a1, 40
    retw
