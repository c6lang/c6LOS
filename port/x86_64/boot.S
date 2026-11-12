/* x86-64 裸机入口：multiboot1 头 → 32 位保护模式 → 恒等映射 2GB → long mode。 */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000000
.set MB_CHECK, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECK

.section .bss
.align 16
stack_bottom:
    .skip 65536
stack_top:

.section .rodata
.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF   /* 64-bit code, DPL0 */
    .quad 0x00AF92000000FFFF   /* 64-bit data, DPL0 */
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
_start:
    cli
    movl $stack_top, %esp

    /* PML4 @0x70000，PD @0x71000，先清零 0x3000 字节 */
    movl $0x70000, %edi
    xorl %eax, %eax
    movl $(0x3000 / 4), %ecx
    rep stosl

    /* PML4[0] -> PD，present|writable */
    movl $0x71000, %eax
    orl $0x3, %eax
    movl %eax, 0x70000
    movl $0, 0x70004

    /* PD：512 个 2MB 页，恒等映射前 1GB */
    movl $0x71000, %edi
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
    orl $0x20, %eax            /* PAE */
    movl %eax, %cr4

    movl $0xC0000080, %ecx     /* EFER */
    rdmsr
    orl $0x100, %eax           /* LME */
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax      /* PG | PE */
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode

.code64
long_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movabs $stack_top, %rsp
    call os_main
2:
    hlt
    jmp 2b
