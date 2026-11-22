/* 32 位入口：恒等映射 1GB、开 SSE、切 long mode，跳 0x11000 的 64 位载荷。 */
.section .rodata
.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF   /* 64-bit code */
    .quad 0x00CF92000000FFFF   /* data */
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .text
.code32
.global _start
_start:
    cli
    movl $0x80000, %esp

    /* PML4 @0x200000, PDPT @0x201000, PD @0x202000
     * 别放 0x70000：内核 .bss 随镜像长大，0x70000~0x73000 迟早被全局变量
     * 压上来（曾经把 g_mmio 写成 0x10800083 这种鬼值）。 */
    movl $0x200000, %edi
    xorl %eax, %eax
    movl $(0x4000 / 4), %ecx
    rep stosl

    movl $0x201003, 0x200000
    movl $0, 0x200004
    movl $0x202003, 0x201000
    movl $0, 0x201004

    movl $0x202000, %edi
    movl $0x83, %eax
    movl $512, %ecx
1:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 1b

    movl $0x200000, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $0x620, %eax          /* PAE | OSFXSR | OSXMMEXCPT */
    movl %eax, %cr4

    movl %cr0, %eax
    andl $0xFFFFFFF3, %eax    /* 清 EM/TS */
    orl $0x2, %eax            /* MP */
    movl %eax, %cr0

    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax          /* LME */
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax     /* PG | PE */
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $0x11000
