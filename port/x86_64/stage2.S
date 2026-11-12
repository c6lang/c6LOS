/* 64 位载荷：链接在 0x20000，由 stage1 进入 long mode 后跳入。 */
.section .bss
.align 16
stack2_bottom:
    .skip 65536
stack2_top:

.section .text.entry, "ax"
.code64
.global _start64
_start64:
    movabs $stack2_top, %rsp
    xorq %rbp, %rbp
    call os_main
1:
    hlt
    jmp 1b
