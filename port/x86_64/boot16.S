/* 16 位引导扇区：int13h LBA 读平铺镜像到 0x10000，切保护模式后跳过去。 */
.code16
.section .text
.global _boot
_boot:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $0x7C00, %sp

    inb $0x92, %al
    orb $0x02, %al
    outb %al, $0x92

    /* ---- E820：物理内存表，给内核的物理帧分配器用 ----
     * count @0x5000（u32），条目 @0x5010（24B：base u64 / len u64 / type u32 / attr u32）。
     * 位置在引导扇区（0x7C00）与栈之下，安全。 */
    movl $0, 0x5000
    movw $0x5010, %di
    xorl %ebx, %ebx
e820_loop:
    movl $0xE820, %eax
    movl $0x534D4150, %edx
    movl $24, %ecx
    int $0x15
    jc e820_done
    cmpl $0x534D4150, %eax
    jne e820_done
    cmpl $0, 16(%di)              /* type（偏移 16；20 是 ACPI 属性位，别弄混） */
    je e820_skip
    incw 0x5000
    addw $24, %di
    cmpw $(0x5010 + 24 * 32), %di
    jae e820_done
e820_skip:
    testl %ebx, %ebx              /* ebx == 0 表示枚举结束 */
    jz e820_done
    jmp e820_loop
e820_done:

    movb $0x42, %ah
    movb $0x80, %dl
    movw $dap1, %si
    int $0x13
    jc 1f

    movb $0x42, %ah
    movb $0x80, %dl
    movw $dap2, %si
    int $0x13
    jc 1f

    movb $0x42, %ah
    movb $0x80, %dl
    movw $dap3, %si
    int $0x13
    jc 1f

    movb $0x42, %ah
    movb $0x80, %dl
    movw $dap4, %si
    int $0x13
    jc 1f

    /* dap5：stage2 增长后镜像越过 0x3FC00（前 4 个 DAP 的上限）。
     * 继续从 LBA383 读到 0x3FC00，覆盖到 0x4FA00 —— 给 stage2 留出余量。 */
    movb $0x42, %ah
    movb $0x80, %dl
    movw $dap5, %si
    int $0x13
    jc 1f

    lgdt gdt_desc
    movl %cr0, %eax
    orl $0x00000001, %eax
    movl %eax, %cr0
    ljmp $0x0008, $next32

.code32
next32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x00010000, %eax
    jmp *%eax
1:
    hlt
    jmp 1b

.align 4
dap1:
    .byte 0x10, 0
    .word 127
    .word 0x0000, 0x1000
    .quad 1
dap2:
    .byte 0x10, 0
    .word 120
    .word 0x0000, 0x1FE0
    .quad 128
dap3:
    .byte 0x10, 0
    .word 8
    .word 0x0000, 0x2EE0
    .quad 248
dap4:
    .byte 0x10, 0
    .word 127
    .word 0x0000, 0x2FE0
    .quad 256

dap5:
    .byte 0x10, 0
    .word 127
    .word 0x0000, 0x3FC0
    .quad 383

.align 8
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt

.org 510
.word 0xAA55
