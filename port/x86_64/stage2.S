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
    /* 清 .bss。引导栈（stack2_*）本身就在 .bss 里，stage1 在跳进来之前
     * 已经用它压过东西；不先清干净，全局变量就会带着引导期的垃圾起来
     * —— 镜像大小一变就砸到不同的变量上（排查过一整天的那种）。 */
    cld
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    subq %rdi, %rcx
    xorl %eax, %eax
    rep stosb
    movabs $stack2_top, %rsp
    xorq %rbp, %rbp
    call os_main
1:
    hlt
    jmp 1b
