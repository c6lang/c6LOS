/* ESP32-S3 启动入口：ROM 把本镜像按段搬到 IRAM/DRAM 后跳到这里。
 * call0 ABI（-mabi=call0），无寄存器窗口溢出。 */

    .section .text.start, "ax", @progbits
    .align 4
    .literal_position
    .literal .Lsp,      0x3FCEF000
    .literal .Ltimg,    0x6001F000
    .literal .Lwdtkey,  0x50D83AA1
    .literal .Lbss0,    _bss_start
    .literal .Lbss1,    _bss_end
    .literal .Lmark,    0x3FCF0000
    .literal .Lmarkv,   0x0000CAFE
    .literal .Lpms,     0x600C11C0

    .global _start
    .type   _start, @function
_start:
    /* 入口标记：立即写 RTC STORE0，验证 ROM 确实跳到 entry。 */
    l32r    a2, .Lmark
    l32r    a3, .Lmarkv
    s32i    a3, a2, 0

    /* 确保内核模式（UM=0）且 INTLEVEL=0。 */
    rsr     a2, ps
    movi    a3, -33
    and     a2, a2, a3
    movi    a3, -16
    and     a2, a2, a3
    movi    a3, -17
    and     a2, a2, a3
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

    /* 清 .bss */
    l32r    a2, .Lbss0
    l32r    a3, .Lbss1
    movi    a4, 0
1:
    bgeu    a2, a3, 2f
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       1b

2:
    call0   port_init
    call0   os_main
3:
    j       3b

    .size _start, . - _start
