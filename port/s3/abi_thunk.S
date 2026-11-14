/* 双 ABI 的 libc 小函数：既能被 call0 调用，也能被 windowed 的 call8 调用。
 *
 * 为什么需要：IDF 的闭源库是 windowed ABI，它们 `call8 memcpy` 时 a0 里带
 * "窗口增量"位（bit31:30），返回必须用 `retw`（先转回窗口再跳）；而我们自己的
 * 代码是 call0，返回必须用 `ret`。用错就会跳到 0x803cd397 这种"带窗口位的
 * 返回地址"，表现为随机 TRAP。
 *
 * 实现上刻意**不用栈、不调其它函数**：直接内联循环，最后按 a0 的窗口位选返回指令。
 * 参数寄存器 a2..a7 在两种 ABI 下一致，直接透传。
 */

    .section .text, "ax", @progbits

    .macro  ABI_RETURN
    extui   a5, a0, 30, 2
    beqz    a5, 9f
    retw
9:
    ret
    .endm

/* void *memcpy(void *dst, const void *src, u32 n) */
    .align  4
    .global memcpy
    .type   memcpy, @function
memcpy:
    beqz    a4, 2f
    movi.n  a9, 0
1:
    add.n   a10, a3, a9
    l8ui    a11, a10, 0
    add.n   a10, a2, a9
    s8i     a11, a10, 0
    addi.n  a9, a9, 1
    bne     a4, a9, 1b
2:
    ABI_RETURN

/* void *memset(void *dst, int c, u32 n) */
    .align  4
    .global memset
    .type   memset, @function
memset:
    beqz    a4, 2f
    movi.n  a9, 0
1:
    add.n   a10, a2, a9
    s8i     a3, a10, 0
    addi.n  a9, a9, 1
    bne     a4, a9, 1b
2:
    ABI_RETURN

/* void *memmove(void *dst, const void *src, u32 n) —— 前向拷贝（够用） */
    .align  4
    .global memmove
    .type   memmove, @function
memmove:
    beqz    a4, 2f
    movi.n  a9, 0
1:
    add.n   a10, a3, a9
    l8ui    a11, a10, 0
    add.n   a10, a2, a9
    s8i     a11, a10, 0
    addi.n  a9, a9, 1
    bne     a4, a9, 1b
2:
    ABI_RETURN

/* int memcmp(const void *a, const void *b, u32 n) */
    .align  4
    .global memcmp
    .type   memcmp, @function
memcmp:
    movi.n  a9, 0
    movi.n  a6, 0
1:
    beq     a4, a9, 3f
    add.n   a10, a2, a9
    l8ui    a11, a10, 0
    add.n   a10, a3, a9
    l8ui    a10, a10, 0
    bne     a11, a10, 2f
    addi.n  a9, a9, 1
    j       1b
2:
    sub     a6, a11, a10
3:
    mov     a2, a6
    ABI_RETURN
