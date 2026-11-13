    .section .text.start,"ax",@progbits
    .align 4
    .literal_position
    .literal .Lvec, _init_start
    .literal .Lstack, __stack_top
    .literal .Lbss0, _sbss
    .literal .Lbss1, _ebss
    .literal .Lpms, 0x600C11C0
    .global _start
_start:
    movi a2, 0x3FCF0000
    movi a3, 0x1111
    s32i a3, a2, 0
    rsr a2, ps
    movi a3, -33
    and a2, a2, a3
    movi a3, -16
    and a2, a2, a3
    wsr a2, ps
    rsync
    l32r a2, .Lpms
    l32i a3, a2, 0
    movi a4, -4
    and a3, a3, a4
    s32i a3, a2, 0
    movi a2, 0x3FCF0000
    movi a3, 0x2222
    s32i a3, a2, 0
    l32r a2, .Lvec
    wsr a2, vecbase
    isync
    l32r a1, .Lstack
    movi a2, 0x3FCF0000
    movi a3, 0x3333
    s32i a3, a2, 0
    l32r a2, .Lbss0
    l32r a3, .Lbss1
    movi a4, 0
1:  bgeu a2, a3, 2f
    s32i a4, a2, 0
    addi a2, a2, 4
    j 1b
2:  movi a2, 0x3FCF0000
    movi a3, 0x4444
    s32i a3, a2, 0
    movi a6, 0
    call4 main
3:  j 3b
