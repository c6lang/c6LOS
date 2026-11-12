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

    .global _start
    .type   _start, @function
_start:
    /* 入口标记：立即写 RTC STORE0，验证 ROM 确实跳到 entry。 */
    l32r    a2, .Lmark
    l32r    a3, .Lmarkv
    s32i    a3, a2, 0

    /* 栈 */
    l32r    a1, .Lsp

    /* 关 TIMG0 看门狗：WDTWPROTECT=0x50D83AA1，WDTCONFIG0/1=0 */
    l32r    a2, .Ltimg
    l32r    a3, .Lwdtkey
    s32i    a3, a2, 0x64
    movi    a3, 0
    s32i    a3, a2, 0x48
    s32i    a3, a2, 0x4C
    l32r    a3, .Lwdtkey
    s32i    a3, a2, 0x64
    movi    a3, 1
    s32i    a3, a2, 0x50

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
