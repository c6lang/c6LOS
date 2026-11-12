/* 机器态定时器陷阱：保存 32 个寄存器，调用 C，mret 返回。 */
    .section .bss
    .align 4
    .globl risc6_trap_save
risc6_trap_save:
    .space 4 * 40 * 4

    .section .text
    .align 2
    .globl rv_bare_trap
rv_bare_trap:
    csrrw   t0, mscratch, t0
    sw      x1,   4*1(t0)
    sw      x2,   4*2(t0)
    sw      x3,   4*3(t0)
    sw      x4,   4*4(t0)
    sw      x6,   4*6(t0)
    sw      x7,   4*7(t0)
    sw      x8,   4*8(t0)
    sw      x9,   4*9(t0)
    sw      x10,  4*10(t0)
    sw      x11,  4*11(t0)
    sw      x12,  4*12(t0)
    sw      x13,  4*13(t0)
    sw      x14,  4*14(t0)
    sw      x15,  4*15(t0)
    sw      x16,  4*16(t0)
    sw      x17,  4*17(t0)
    sw      x18,  4*18(t0)
    sw      x19,  4*19(t0)
    sw      x20,  4*20(t0)
    sw      x21,  4*21(t0)
    sw      x22,  4*22(t0)
    sw      x23,  4*23(t0)
    sw      x24,  4*24(t0)
    sw      x25,  4*25(t0)
    sw      x26,  4*26(t0)
    sw      x27,  4*27(t0)
    sw      x28,  4*28(t0)
    sw      x29,  4*29(t0)
    sw      x30,  4*30(t0)
    sw      x31,  4*31(t0)
    csrr    t1, mcause
    sw      t1,   4*33(t0)
    csrr    t1, mepc
    sw      t1,   4*34(t0)
    lw      t1,   4*32(t0)
    addi    t1, t1, 1
    sw      t1,   4*32(t0)

    mv      s0, t0
    call    rv_bare_tick_irq
    mv      t0, s0

    lw      x1,   4*1(t0)
    lw      x2,   4*2(t0)
    lw      x3,   4*3(t0)
    lw      x4,   4*4(t0)
    lw      x6,   4*6(t0)
    lw      x7,   4*7(t0)
    lw      x8,   4*8(t0)
    lw      x9,   4*9(t0)
    lw      x10,  4*10(t0)
    lw      x11,  4*11(t0)
    lw      x12,  4*12(t0)
    lw      x13,  4*13(t0)
    lw      x14,  4*14(t0)
    lw      x15,  4*15(t0)
    lw      x16,  4*16(t0)
    lw      x17,  4*17(t0)
    lw      x18,  4*18(t0)
    lw      x19,  4*19(t0)
    lw      x20,  4*20(t0)
    lw      x21,  4*21(t0)
    lw      x22,  4*22(t0)
    lw      x23,  4*23(t0)
    lw      x24,  4*24(t0)
    lw      x25,  4*25(t0)
    lw      x26,  4*26(t0)
    lw      x27,  4*27(t0)
    lw      x28,  4*28(t0)
    lw      x29,  4*29(t0)
    lw      x30,  4*30(t0)
    lw      x31,  4*31(t0)
    csrrw   t0, mscratch, t0
    mret
