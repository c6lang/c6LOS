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
12: .word   g_s3_win_spill_en  /* spill 总开关（见 ctx.c / wifi_glue.c） */

/* ---- 把祖先窗口整圈 spill 到栈上（IDF SPILL_ALL_WINDOWS 的完整版）------
 * 与 `xt_asm_utils.h` 里那个裸宏相比，官方 `_xt_context_save` 还多做三件事，
 * 少做任何一件这套机制就静默失效（这是前几轮"签名没变"的根因）：
 *
 *   1) 寄存器窗口溢出是 **level-1 异常**，向量末尾的 `rfwo` 会消耗 EPC1，
 *      所以要先把 EPC1 存下来、转完再写回；
 *   2) 溢出向量只在 PS.WOE=1 且 PS.EXCM=0 时才生效 —— WOE=0 时整段 rotw
 *      是**静默空转**（官方注释原话 "becomes a silent noop"）。所以先把
 *      INTLEVEL 抬到 7（屏蔽所有可屏蔽中断，溢出异常不受 INTLEVEL 影响）、
 *      清 EXCM、置 WOE；
 *   3) 转完只有"当前窗口"该是 live 的，WINDOWSTART 必须显式写成
 *      1<<windowbase，否则就是 §197 那个签名（windowbase=3 / ws=0x20）。
 *
 * 转 16 个窗口（64 aregs / 4）正好一圈，WINDOWBASE 回到原值，所以**不需要**
 * （也没办法）写 WINDOWBASE。`and a12,a12,a12` 是恒等写：即使被溢出到栈上，
 * 写回去的还是原值，所以它"不占用任何寄存器"。
 *
 * 破坏 a8..a11（windowed ABI 的 caller-saved）；a4..a7 与 a12..a15 一个字节
 * 都不能动（a4..a7 = 调用者的 a12..a15 是 callee-saved，a12..a15 = 调用者的
 * a0..a3 是 retw 要用的返回地址和帧指针）。 */
    .macro  WIN_SPILL
    rsr     a8, ps
    extui   a9, a8, 4, 1
    bnez    a9, .Lspill_end\@   /* EXCM=1（正在异常里）：窗口不能碰，跳过 */
    rsr     a9, epc1
    movi    a10, -32
    and     a10, a10, a8         /* 清 INTLEVEL(0..3) 和 EXCM(bit4) */
    movi    a11, 1
    slli    a11, a11, 18
    addi    a11, a11, 7          /* WOE(bit18) | INTLEVEL=7 */
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
    wsr     a8, ps               /* 恢复原 PS */
    rsync
    wsr     a9, epc1
    rsr     a10, windowbase
    movi    a11, 1
    ssl     a10
    sll     a11, a11
    wsr     a11, windowstart
    isync
.Lspill_end\@:
    .endm

    .macro  W2C name, target
    /* l32r 只能往前引用，所以字面量必须放在指令之前；用数字标签 9 保证
     * 每次展开都指向紧邻自己的那一个。 */
    .align  4
9:  .word   \target
    .align  4
    .global \name
    .type   \name, @function
\name:
    /* 帧布局（128 字节）：
     *   [a1 + 0 .. 15]   ABI 保留，不碰
     *   [a1 + 16 .. 31]  自己的保存槽（带窗口标签的 a0）
     *   [a1 + 32 .. 127] 空闲
     * 帧**下方**再留 64 字节死区（addi -64）：spill 出来的窗口槽位落在
     * [a1-64, a1) 里，之后内核的调用帧从 a1-64 往下长，两者不重叠。
     * 这是本轮的关键顺序：**先 spill、再把 sp 降下去调内核**。
     * 反过来（先降 sp 再 spill）槽位就会落在内核帧里，等于踩内核的局部量。 */
#ifdef S3_W2C_MIN
    /* 极简版（§231）：不做任何窗口操作，只借 entry 的 32 字节帧保存带窗口
     * 标签的 a0，然后 callx0 进 call0 内核函数、取回 a0、retw。
     * 动机：真板上 W2C+WIN_SPILL 版本在 ppTask 上下文里出现过
     *   EXCCAUSE=3 LoadStoreError, EXCVADDR=0x40043ca4(ROM), EPC 在壳里,
     *   halt 时 windowbase=13 / windowstart=0xa002（明显是坏的窗口状态）。
     * 既然 spill 那套从没在真板上验证过，先退回最小实现看还炸不炸。 */
    entry   a1, 32
    s32i    a0, a1, 0
    l32r    a8, 9b
    callx0  a8
    l32i    a0, a1, 0
    retw
#else
    entry   a1, 128
    s32i    a0, a1, 16           /* 存下带窗口标签的返回地址 */
    l32r    a8, 12b
    l32i    a9, a8, 0
    beqz    a9, .Lno_spill\@
    WIN_SPILL
.Lno_spill\@:
    addi    a1, a1, -64
    l32r    a8, 9b
    callx0  a8
    addi    a1, a1, 64
    l32i    a0, a1, 16
    retw
#endif
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
    entry   a1, 128
    s32i    a0,  a1, 16
    s32i    a12, a1, 20
    s32i    a13, a1, 24
    l32r    a12, 6b
    l32i    a13, a12, 4
    s32i    a0, a12, 0
    addi    a13, a13, 1
    s32i    a13, a12, 4
    l32r    a8, 12b
    l32i    a9, a8, 0
    beqz    a9, .Lno_spill\@
    WIN_SPILL
.Lno_spill\@:
    addi    a1, a1, -64
    l32r    a8, 9b
    callx0  a8
    addi    a1, a1, 64
    l32i    a0,  a1, 16
    l32i    a12, a1, 20
    l32i    a13, a1, 24
    retw
    .endm

/* 注意：堆**不**走内核的 os_sys_*（那两个在 port/s3/fast.c，LEAN 构建里
 * 不编进来）。lwIP 的 malloc/calloc/realloc 直接落在 wifi_glue.c 的
 * osi_malloc 堆上，和闭源库的 free() 是同一个堆（见 §210）。 */

/* ---- 读当前 sp（诊断用，§239）---- */
    .align  4
9:  .word   0
    .align  4
    .global s3_w2c_sp_probe
    .type   s3_w2c_sp_probe, @function
s3_w2c_sp_probe:
    entry   a1, 32
    mov     a2, a1
    retw

/* ---- 看门狗（§228）：os_wdt_feed/os_wdt_start 是 call0 的 C 函数（flash.c），
 * windowed 岛里不能直接调 —— 必须过这个壳，否则返回地址带窗口标签、
 * 一返回就跳飞（真板实测：TRAP cause=0 pc=0x8037a7e9，正是带标签的 a0）。 */
    W2C s3_w2c_wdt_feed,       os_wdt_feed
    W2C s3_w2c_wdt_start,      os_wdt_start

/* ---- 关中断（OSI 的 _wifi_int_disable/_restore，§226）----
 * 内核的 port_irq_save/restore 是 rsil/wsr ps，正好是 IDF portENTER_CRITICAL
 * 的等价物。库在临界区里不会阻塞（IDF 下也是这么用的）。 */
    W2C s3_w2c_irq_save,       port_irq_save
    W2C s3_w2c_irq_restore,    port_irq_restore

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
    /* 这条壳是 ppTask 真正阻塞的地方，spill 必须在这里做（理由见文件头
     * WIN_SPILL 的注释）：此刻 a1 就是本壳的帧基址，帧下 64 字节还是空闲栈，
     * 祖先窗口的槽位全部落在自由区里。 */
    entry   a1, 128
    s32i    a12, a1, 20
    s32i    a13, a1, 24
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
    s32i    a0, a1, 16
    /* spill 要在把 a10/a11/a12 改成参数之前做：它会用到 a8..a11 */
    l32r    a8, 12b
    l32i    a9, a8, 0
    beqz    a9, .Lrecv_nospill
    WIN_SPILL
.Lrecv_nospill:
    mov.n   a10, a2
    mov.n   a11, a3
    mov.n   a12, a4
    l32r    a9, 11b
    s32i    a0, a9, 0            /* 诊断：callx8 之前的 a0 */
    addi    a1, a1, -64          /* 死区：spill 槽位在 [a1-64, a1) */
    l32r    a8, 8b
    callx8  a8
    addi    a1, a1, 64
    l32r    a9, 11b
    s32i    a0, a9, 4            /* 诊断：callx8 之后的 a0 */
    mov.n   a2, a10
    l32i    a0,  a1, 16
    l32i    a12, a1, 20
    l32i    a13, a1, 24
    retw
