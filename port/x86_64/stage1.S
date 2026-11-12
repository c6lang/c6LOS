/* 32 位入口：恒等映射 1GB、开 SSE、切 long mode，跳 0x20000 的 64 位载荷。 */
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

    /* PML4 @0x70000, PDPT @0x71000, PD @0x72000 */
    movl $0x70000, %edi
    xorl %eax, %eax
    movl $(0x4000 / 4), %ecx
    rep stosl

    movl $0x71003, 0x70000
    movl $0, 0x70004
    movl $0x72003, 0x71000
    movl $0, 0x71004

    movl $0x72000, %edi
    movl $0x83, %eax
    movl $512, %ecx
1:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 1b

    movl $0x70000, %eax
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
    ljmp $0x08, $0x20000
