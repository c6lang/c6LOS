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
    .word 8
    .word 0x0000, 0x1FE0
    .quad 128

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
