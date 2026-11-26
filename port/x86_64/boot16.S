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

    /* 单 DAP 循环：每趟 127 扇区（约 63.5KB），从 LBA1 装到 0x10000，
     * 连续 10 趟覆盖到 0xA0000。以前固定 11 个 DAP 只到 0x4FA00，镜像
     * 一过 320KB，.data 尾部（g_cur 等）就悄悄没装进来。 */
    movb $0x10, 0x6000
    movb $0, 0x6001
    movw $127, 0x6002
    movw $0, 0x6004
    movw $0x1000, 0x6006
    movl $1, 0x6008
    movl $0, 0x600C
    movw $9, 0x6010
load_loop:
    movw $0x6000, %si
    movb $0x42, %ah
    movb $0x80, %dl
    int $0x13
    jc 1f
    addw $0x0FE0, 0x6006
    addl $127, 0x6008
    decw 0x6010
    jnz load_loop
    /* 最后一趟：9 扇区，正好补到 0xA0000（9*127+9=1152 扇区 = 0x90000B）。 */
    movw $9, 0x6002
    movw $0x9EE0, 0x6006
    movl $1144, 0x6008
    movw $0x6000, %si
    movb $0x42, %ah
    movb $0x80, %dl
    int $0x13
    jc 1f

    /* ---- QEMU std VGA 的 Bochs VBE 扩展：设 800x600x32 LFB ----
     * 0x1CE=index, 0x1CF=data；ID 0xB0C0..0xB0C5 表示支持。失败不致命：
     * magic 保持 0，内核 /dev/fb0 只做 RAM 扫描缓冲。 */
    movw $0x1CE, %dx
    xorw %ax, %ax
    outw %ax, %dx
    incw %dx
    inw %dx, %ax
    cmpw $0xB0C0, %ax
    jb vbe_none
    cmpw $0xB0C5, %ax
    ja vbe_none
    movw $1, %ax
    movw $0x1CE, %dx
    outw %ax, %dx
    incw %dx
    movw $800, %ax
    outw %ax, %dx
    movw $2, %ax
    movw $0x1CE, %dx
    outw %ax, %dx
    incw %dx
    movw $600, %ax
    outw %ax, %dx
    movw $3, %ax
    movw $0x1CE, %dx
    outw %ax, %dx
    incw %dx
    movw $32, %ax
    outw %ax, %dx
    movw $6, %ax
    movw $0x1CE, %dx
    outw %ax, %dx
    incw %dx
    movw $800, %ax
    outw %ax, %dx
    movw $7, %ax
    movw $0x1CE, %dx
    outw %ax, %dx
    incw %dx
    movw $600, %ax
    outw %ax, %dx
    movw $4, %ax
    movw $0x1CE, %dx
    outw %ax, %dx
    incw %dx
    movw $0x41, %ax              /* ENABLED | LFB */
    outw %ax, %dx
    movl $0x42463643, 0x7080     /* 'C6FB' */
    movl $0xFD000000, 0x7084
    movl $800, 0x7088
    movl $600, 0x708C
    movl $32, 0x7090
    movl $3200, 0x7094
    jmp vbe_done
vbe_none:
    movl $0, 0x7080
vbe_done:
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
