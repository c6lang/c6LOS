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

    /* 自己的向量表（必须在 IRAM，1KB 对齐） */
    movi    a2, _vecbase
    wsr     a2, vecbase
    isync

    /* 栈 */
    l32r    a1, .Lsp

    /* TIMG0 看门狗留给 C 侧在时钟确定后关闭；此处不碰外设。 */

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
    call0   port_init
    call0   os_main
3:
    j       3b

    .size _start, . - _start
