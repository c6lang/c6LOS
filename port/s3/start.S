/* ESP32-S3 启动入口：ROM 把本镜像按段搬到 IRAM/DRAM 后跳到这里。
 * call0 ABI（-mabi=call0），无寄存器窗口溢出。 */

    .section .text.start, "ax", @progbits
    .align 4
    .literal_position
    /* 栈顶用链接符号 _stack_top（port/s3/link.ld 里定义在 DRAM 坏区以下）。
     * 注意：不要在这里硬编码地址 —— 原来硬写 0x3FCEF000，改 link.ld 是不生效的，
     * 栈的高端会一直压在 0x3FCD9500 以上的坏区里。 */
    .literal .Lsp,      _stack_top
    .literal .Lbss0,    _bss_start
    .literal .Lbss1,    _bss_end
    .literal .Lpms,     0x600C11C0
    /* RTC_SLOW_MEM：CPU 复位后不丢，用来定位"启动阶段就死了"的故障点。
     * 复位后用 `esptool read-mem 0x50000000` 读：
     *   0xA0 = 停在清 .bss 之前，0xA1 = .bss 清完（该进 port_init 了），
     *   0xB0..0xB3 = port_init 各阶段（见 console.c），
     *   0xBAD0 出现在 0x50000100 起 = 跑进了未处理向量（见 irq.S）。 */
    .literal .Lrtc,     0x50000000
    /* 诊断区起止：每次启动清零，保证 read-mem 读到的都是本次启动写的值
     * （RTC 跨复位不丢，之前多次被上一轮的残留值误导）。 */
    .literal .Ldiag,     0x50000200
    .literal .Ldiag_end, 0x50001F00
    .literal .Lm0,      0x000000A0
    .literal .Lm1,      0x000000A1
    .literal .Lmarks,   g_boot_marks

    .global _start
    .type   _start, @function
_start:
    /* 【诊断信标】RTC_CNTL_STORE7(0x600080CC)：掉电域、CPU 复位不丢、
     * esptool 可以直接 read-mem 读。这里写 0xA0 表示"进了 _start"。
     * 用它可以把"芯片到底跑没跑我们的代码"和串口是否可用彻底分开。 */
    movi    a3, 0x600080CC
    movi    a4, 0xA0A0A0A0
    s32i    a4, a3, 0

    /* 确保内核模式（UM=0）；**INTLEVEL 先拉到 15 关中断**。
     *
     * 原来是清成 0（开中断），启动期就有中断可乘之机：.bss 还没清、内核结构
     * 还没初始化、看门狗也还没关（那是 port_init 的第一件事），这时候来一个
     * 中断就会跳进半初始化的处理函数。现在启动期全程关中断，由 s3_irq_init()
     * 末尾的 rsil 0 正式打开。 */
    rsr     a2, ps
    movi    a3, -33
    and     a2, a2, a3              /* UM = 0（内核模式） */
    movi    a3, 15
    or      a2, a2, a3              /* INTLEVEL = 15：屏蔽所有中断 */
    movi    a3, -17
    and     a2, a2, a3              /* EXCM = 0 */
    wsr     a2, ps
    rsync

    /* 关闭 PMS VECBASE override，否则 wsr vecbase 不生效。 */
    l32r    a2, .Lpms
    l32i    a3, a2, 0
    movi    a4, -4
    and     a3, a3, a4
    s32i    a3, a2, 0

#ifndef __XTENSA_CALL0_ABI__
    /* windowed ABI：windowstart 只标当前窗口有效；PS.WOE=1 让 call8/12 能正常
     * 分配新窗口（否则每次调用都会走窗口溢出向量，功能上能跑但慢很多）。 */
    movi    a3, 1
    wsr     a3, windowstart
    rsr     a2, ps
    movi    a3, 1
    slli    a3, a3, 18              /* PS.WOE */
    or      a2, a2, a3
    wsr     a2, ps
    rsync
#endif

    /* ★ 无条件做一遍窗口状态归一（call0 构建也要做！§202）。
     *
     * ROM 跳到我们这里时留下的 WINDOWSTART 是**有空洞**的活动窗口集合
     * （真板实测 ws=0x28：窗口 3 和 5 都标着活）。我们内核是 call0，自己从不
     * 轮转窗口，所以这堆"活窗口"会一直挂着；一旦后面调 windowed 闭源库，
     * `entry` 触发的溢出处理会去 spill 那个早已不存在的老窗口，用它的 a1
     * 当栈指针 → 落到 0x40 之类的地方，报 StoreProhibited（cause=29, excvaddr=0x40）。
     *
     * call0 代码不依赖任何祖先窗口，所以"只有当前窗口活"是唯一自洽的状态。
     * 同时把 PS.OWB 清掉（跳转链条到这里已经断开了）。
     * 注意 windowstart 只能写成 1<<windowbase，不能写 windowbase（§189）。 */
    rsr     a2, windowbase
    movi    a3, 1
    ssl     a2
    sll     a3, a3
    wsr     a3, windowstart
    rsr     a2, ps
    movi    a3, -257                /* 清 PS.OWB(bit8) */
    and     a2, a2, a3
    wsr     a2, ps
    rsync

    /* ★ 窗口"预热"：把 4 个窗口的 a1 都变成**合法栈指针**（§236）。
     *
     * 为什么必须做：Xtensa 只有 4 个窗口，寄存器文件里可能残留 ROM/bootloader
     * 阶段的窗口内容（真板实测有窗口的 a1 = 0x40043ca4，一个 ROM 地址）。
     * 我们的 w2c 壳用 `rotw` 转一整圈来强制 spill 全部窗口（和
     * xthal_window_spill 一样），转到一个 a1 是垃圾的窗口时，硬件溢出向量就会
     * 按那个垃圾地址写 → LoadStoreError（真板实测 cause=3, excvaddr=0x40043ca4）。
     *
     * 办法：在进 C 代码之前先用 4 层嵌套 call8 把每个窗口都"占"一遍
     * （每层 entry 都会给该窗口一个真实帧），于是所有窗口的 a1 都是本栈上的
     * 合法地址；之后不管谁用 rotw spill，写到的都是我们的栈，不会炸。 */
    rsr     a2, ps
    movi    a3, -32                /* 清 INTLEVEL + EXCM */
    and     a2, a2, a3
    movi    a3, 1
    slli    a3, a3, 18             /* PS.WOE=1：让 rotw 真正轮转 */
    or      a2, a2, a3
    wsr     a2, ps
    rsync
    l32r    a1, .Lsp
    call8   .Lprime1
    j       .Lprime_done
.Lprime1:
    entry   a1, 32
    call8   .Lprime2
    retw
.Lprime2:
    entry   a1, 32
    call8   .Lprime3
    retw
.Lprime3:
    entry   a1, 32
    call8   .Lprime4
    retw
.Lprime4:
    entry   a1, 32
    retw
.Lprime_done:

    /* 自己的向量表（必须在 IRAM，1KB 对齐） */
    movi    a2, _vecbase
    wsr     a2, vecbase
    isync

    /* 栈 */
    l32r    a1, .Lsp

    /* **先把看门狗关掉，再干活**：TIMG0 / RTC 的看门狗寄存器跨 CPU 复位不清零
     * （只有掉电才复位），之前任何固件（或 esptool 的 watchdog-reset）把它武装成
     * 很短的超时后，就会出现"每次都还没跑到 port_init 就被复位"——现象是串口只有
     * ROM 的 load 两行、RTC 标记全是乱值。这里用最少的指令先关 TIMG0 和 RTC 的狗。 */
    movi    a2, 0x6001F000          /* TIMG0 */
    movi    a3, 0x50D83AA1
    s32i    a3, a2, 0x64            /* WDTWPROTECT */
    movi    a3, 0
    s32i    a3, a2, 0x48            /* WDTCONFIG0：关 */
    s32i    a3, a2, 0x4C
    s32i    a3, a2, 0x64            /* 重新上锁 */

    movi    a2, 0x60008000          /* RTC_CNTL */
    s32i    a3, a2, 0xB0            /* 先写 0（解锁前的旧 key 之后会被覆盖） */
    movi    a3, 0x50D83AA1
    s32i    a3, a2, 0xB0            /* WDTWPROTECT */
    movi    a3, 0
    s32i    a3, a2, 0x98            /* RTC WDTCONFIG0：关 */
    s32i    a3, a2, 0xB0            /* 重新上锁 */

#ifndef S3_KEEP_SWD
    /* RTC 超级看门狗（SWD）：esptool 的 watchdog-reset 会把它也装上，超时几秒；
     * 它同样跨 CPU 复位保留，必须一起关掉。SWD_CONF.SWD_DISABLE = bit30。
     * 诊断时可用 -DS3_KEEP_SWD 保留它：应用挂住 -> 几秒后自动复位重跑，
     * 而 RTC 域（信标）不会被清，于是下一次运行就能报告"上一轮卡在哪"。 */
    movi    a3, 0x8F1D312A
    s32i    a3, a2, 0xB8            /* SWD_WPROTECT */
    l32i    a3, a2, 0xB4
    movi    a4, 1
    slli    a4, a4, 30
    or      a3, a3, a4
    s32i    a3, a2, 0xB4            /* SWD_CONF |= SWD_DISABLE */
#endif /* S3_KEEP_SWD */
    movi    a3, 0
    s32i    a3, a2, 0xB8            /* 重新上锁 */

    /* 进 _start 的第一件事：往 .data 里的 marks[1] 写 0xA0（这个不会被
     * 清 .bss 抹掉，比 RTC 可靠）。看到 0xEE 就说明连 _start 都没进来。 */
    l32r    a2, .Lmarks
    movi    a3, 0xA0
    s32i    a3, a2, 4

    /* 清 .bss。
     *
     * **注意 .bss 不能长到 0x3FCD9500 那一带**：这块板实测有一段"密集读写就
     * 把 CPU 挂住"的坏区（稀疏读写都正常，所以早年整段探针全绿）。一旦 .bss
     * 盖过去，就是在这个循环里静默死掉：不打印、不进异常向量、十几秒后被看门
     * 狗整片复位（表现为串口一个字都没有 + 屏幕全黑）。细节见
     * docs/todo.md「音频」一节。 */
    l32r    a2, .Lrtc
    l32r    a3, .Lm0
    s32i    a3, a2, 0            /* 标记 A0：清 bss 之前 */
    l32r    a2, .Lbss0
    l32r    a3, .Lbss1
    movi    a4, 0
1:
    bgeu    a2, a3, 2f
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       1b

2:
    l32r    a2, .Lrtc
    l32r    a3, .Lm1
    s32i    a3, a2, 0            /* RTC 里的 A1（可能被 ROM 覆写，仅作参考） */
    l32r    a2, .Lmarks
    l32r    a3, .Lm1
    s32i    a3, a2, 0            /* DRAM 标记 [0] = 0xA1：清 bss 完成（可靠） */
#ifdef __XTENSA_CALL0_ABI__
    call0   port_init
    call0   os_main
#else
    /* windowed ABI：C 函数入口是 `entry sp,N`、出口是 `retw`，必须用 call8 进去
     * （窗口 base += 8），否则它 retw 时会退回到错误的窗口直接跑飞。
     * 之前就是这里用 call0 调 port_init：banner 打完一返回就炸。 */
    call8   port_init
    call8   os_main
#endif
3:
    j       3b

    .size _start, . - _start
