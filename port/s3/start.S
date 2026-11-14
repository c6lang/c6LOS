/* ESP32-S3 启动入口：ROM 把本镜像按段搬到 IRAM/DRAM 后跳到这里。
 * call0 ABI（-mabi=call0），无寄存器窗口溢出。 */

    .section .text.start, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lsp,      0x3FCEF000
    .literal .Lbss0,    _bss_start
    .literal .Lbss1,    _bss_end
    .literal .Lpms,     0x600C11C0
    /* RTC_SLOW_MEM：CPU 复位后不丢，用来定位"启动阶段就死了"的故障点。
     * 复位后用 `esptool read-mem 0x50000000` 读：
     *   0xA0 = 停在清 .bss 之前，0xA1 = .bss 清完（该进 port_init 了），
     *   0xB0..0xB3 = port_init 各阶段（见 console.c），
     *   0xBAD0 出现在 0x50000100 起 = 跑进了未处理向量（见 irq.S）。 */
    .literal .Lrtc,     0x50000000
    .literal .Lm0,      0x000000A0
    .literal .Lm1,      0x000000A1

    .global _start
    .type   _start, @function
_start:
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

    /* RTC 超级看门狗（SWD）：esptool 的 watchdog-reset 会把它也装上，超时几秒；
     * 它同样跨 CPU 复位保留，必须一起关掉。SWD_CONF.SWD_DISABLE = bit30。 */
    movi    a3, 0x8F1D312A
    s32i    a3, a2, 0xB8            /* SWD_WPROTECT */
    l32i    a3, a2, 0xB4
    movi    a4, 1
    slli    a4, a4, 30
    or      a3, a3, a4
    s32i    a3, a2, 0xB4            /* SWD_CONF |= SWD_DISABLE */
    movi    a3, 0
    s32i    a3, a2, 0xB8            /* 重新上锁 */

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
    s32i    a3, a2, 0            /* 标记 A1：清 bss 完成 */
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
