/* port/x86_64/cd0.S —— El Torito no-emulation 引导块（M11-0）
 *
 * BIOS 把本块（2048 字节）装到 0x7C00 并跳进来。这里干三件事：
 *   1. 用 int 13h AH=42h 从光盘按 2048 字节块把载荷读进内存
 *      （光盘的逻辑块就是 2048，DAP 的"块数"也是 2048 字节单位）；
 *   2. 写引导信息块（0x7000，魔数 'C6IS'），把内核/rootfs 的物理地址与长度交给内核；
 *   3. 切到 32 位保护模式远跳内核 stage1（0x10000，.code32）。
 *
 * 载荷参数由 tools/mkiso.py 直接改写 magic 后面那 24 个字节——ISO 布局是我们自己排的，
 * LBA 固定，所以既不需要 ISO9660 解析器，也不需要 ATAPI 驱动。
 *
 * 目标地址按 64KB 段推进（每块 2048 字节 = 0x80 段），DAP 偏移恒为 0，不会跨段回绕；
 * 这条路径覆盖 1MB 以内的载荷（内核 110KB 绰绰有余）。超过 1MB 的 rootfs 需要
 * unreal mode 或 ATAPI，留给 M11-1（见 docs/m11-docker-iso-design.md §3.3）。
 *
 * 踩过的坑（都是"改一行就好、但找半天"的那种）：
 *   - .code16/.code32 必须在真模式切换处成对出现，辅助例程落进 .code32 就全乱；
 *   - 清 PE 之后的远跳必须用 16 位编码（32 位 EA 会被实模式读错位）；
 *   - 串口输出要轮询 THR 空，而且打印例程不能把值留在 EAX 里（put_chr 会动 AL）。
 */
    .code16
    .section .text.entry, "ax"
    .global _start
_start:
    ljmp $0x0000, $start2           /* BIOS 可能用 CS=0x07C0:IP=0 进来，先归一化 */
start2:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    cld
    movb %dl, drive                 /* 引导盘号（光盘一般是 0xE0 起步） */
    sti
    call serial_init

    /* ---- E820：物理内存表（boot16 在盘引导路径里做的，光盘这边自己做）----
     * count @0x5000（u32），条目 @0x5010（24B：base u64 / len u64 / type u32 / attr u32）。
     * 内核的物理帧分配器只认这张表，少了它 port_mmu_init 直接失败。 */
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

    /* A20（1MB 以内的载荷用不到，但顺手打开，免得后面忘） */
    movw $0x2401, %ax
    int $0x15
    inb $0x92, %al
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92

    /* 打一行参数：出问题时一眼能看出 mkiso 有没有把 LBA 填对 */
    movw $msg_magic, %si
    call print
    movzbl magic, %eax
    call put_hex
    movw $msg_param, %si
    call print
    movl magic+8, %eax
    call put_hex
    movb $'/', %al
    call put_chr
    movl magic+12, %eax
    call put_hex
    movw $msg_r, %si
    call print
    movl magic+20, %eax
    call put_hex
    movb $'/', %al
    call put_chr
    movl magic+24, %eax
    call put_hex
    movw $msg_eol, %si
    call print

    /* 内核载荷 */
    movl magic+8, %eax
    movl %eax, cur_lba
    movl magic+12, %eax
    movl %eax, blocks_left
    movl magic+16, %eax
    movl %eax, dst_pa
    movb $'A', %al
    call put_chr
    call load_payload
    movb $'K', %al
    call put_chr

    /* rootfs 载荷（块数 0 就跳过） */
    movl magic+20, %eax
    movl %eax, cur_lba
    movl magic+24, %eax
    movl %eax, blocks_left
    testl %eax, %eax
    jz no_rootfs
    movl magic+28, %eax
    movl %eax, dst_pa
    cmpl $0x00100000, %eax
    jae 1f
    call load_payload               /* 目标在 1MB 以下：实模式逐块直读就够 */
    jmp 2f
1:
    call load_payload_hi            /* 目标在 1MB 以上：unreal mode 搬运 */
2:
no_rootfs:
    movw $msg_loaded, %si
    call print

    /* 引导信息块（0x7000，32 字节） */
    movl $0x53493643, 0x7000        /* 'C','6','I','S'（小端） */
    movl $1, 0x7004                 /* version */
    movl $1, 0x7008                 /* flags: bit0 = 从光盘启动 */
    movl magic+16, %eax
    movl %eax, 0x700C               /* kernel_pa */
    movl magic+12, %eax
    shll $11, %eax                  /* 块 * 2048 = 字节 */
    movl %eax, 0x7010               /* kernel_len */
    movl magic+28, %eax
    movl %eax, 0x7014               /* rootfs_pa */
    movl magic+24, %eax
    shll $11, %eax
    movl %eax, 0x7018               /* rootfs_len */
    movl $0, 0x701C                 /* data_lba（数据盘留给 M11-2） */

    /* 内核 stage1 是 .code32，必须在 32 位保护模式 + 平坦段下进去 */
    movw $msg_pm, %si
    call print
    cli
    lgdt gdt_desc
    movl %cr0, %eax
    orl $0x00000001, %eax
    movl %eax, %cr0
    ljmp $0x08, $enter32
    .code32
enter32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movl $0x00010000, %eax
    jmp *%eax

    .code16                        /* 下面的辅助例程都是 16 位 */

/* ---- 读载荷（< 1MB）：cur_lba / blocks_left / dst_pa 是入参，会被改写 ----
 * 一次一块（2048 字节）。试过"一次读完/multi-block"：SeaBIOS 的 ATAPI 读
 * 第二次调用会挂住（第一块成功、第二块 DRQ 等不到），所以老老实实逐块读。 */
load_payload:
    movl dst_pa, %eax
    shrl $4, %eax                   /* 物理地址 → 段 */
    movw %ax, dap+6
    movw $0, dap+4
1:
    movl blocks_left, %eax
    testl %eax, %eax
    jz 9f
    movw $1, dap+2
    movl cur_lba, %eax
    movl %eax, dap+8                /* 64 位 LBA 的低半 */
    movl $0, dap+12
    movb drive, %dl
    movw $dap, %si
    movb $0x42, %ah
    sti
    int $0x13
    jc fail
    addl $1, cur_lba
    addw $0x0080, dap+6             /* 目标段 +2048/16 */
    subl $1, blocks_left
    jmp 1b
9:
    ret

/* ---- 读大于 1MB 的载荷：逐块读进 0x80000 暂存区，再进 unreal mode
 *      用 32 位寻址搬到 dst_pa ----
 * int 13h 的 DAP 只有"段:偏移"，实模式到不了 1MB 以上；unreal mode 让 ds/es
 * 的缓存描述符保持 4GB 限长，回到实模式后用 32 位寄存器寻址搬运。
 * 每块进出一次 unreal mode，BIOS 调用始终在干净的实模式段下做。 */
load_payload_hi:
1:
    movl blocks_left, %eax
    testl %eax, %eax
    jz 9f
    movw $1, dap+2
    movw $0, dap+4
    movw $0x8000, dap+6             /* 暂存区 0x80000 */
    movl cur_lba, %eax
    movl %eax, dap+8
    movl $0, dap+12
    movb drive, %dl
    movw $dap, %si
    movb $0x42, %ah
    sti
    int $0x13
    jc fail
    call unreal_on
    movl $0x00080000, %esi
    movl dst_pa, %edi
    movl $2048, %ecx
    call copy_flat
    call unreal_off
    movb $'h', %al
    call put_chr
    addl $1, cur_lba
    subl $1, blocks_left
    addl $2048, dst_pa
    jmp 1b
9:
    ret

fail:
    movw $fail_msg, %si
    call print
1:
    hlt
    jmp 1b

/* 进 unreal mode：ds/es 保留 4GB 限长的数据段描述符缓存。
 * 关键：不玩"清 PE 之后再远跳"——那一步要 16 位编码的 EA 才能被实模式正确译码，
 * 稍不注意就飞。这里改成先跳到 16 位代码段（选择子 0x18，D=0、base 0），
 * 在 PM 里就把 CS 换成 16 位形式，然后清 PE 直接接着跑，不用再远跳。 */
unreal_on:
    lgdt gdt_desc
    movl %cr0, %eax
    orl $0x00000001, %eax
    movl %eax, %cr0
    ljmp $0x08, $unreal_pm
    .code32
unreal_pm:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ljmp $0x18, $unreal_pm16       /* 16 位代码段（D=0，base 0） */
    .code16
unreal_pm16:
    movl %cr0, %eax
    andl $0xFFFFFFFE, %eax
    movl %eax, %cr0                /* 回实模式：CS 已经是 16 位/base 0 */
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    ret

/* 把 ds/es 换回普通实模式段（64KB），BIOS 调用前用 */
unreal_off:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    ret

/* esi = 源（32 位物理地址），edi = 目标，ecx = 字节数 */
copy_flat:
    movl %ecx, %ebx
    shrl $2, %ecx
1:
    movl (%esi), %eax
    movl %eax, %es:(%edi)
    addl $4, %esi
    addl $4, %edi
    decl %ecx
    jnz 1b
    andl $3, %ebx
2:
    jz 3f
    movb (%esi), %al
    movb %al, %es:(%edi)
    incl %esi
    incl %edi
    decl %ebx
    jmp 2b
3:
    ret

/* COM1 115200 8N1 —— 与 console.c 一致，内核起来后会再初始化一遍 */
serial_init:
    movw $0x3F9, %dx
    movb $0x00, %al
    outb %al, %dx
    movw $0x3FB, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3F8, %dx
    movb $0x01, %al
    outb %al, %dx
    movw $0x3F9, %dx
    movb $0x00, %al
    outb %al, %dx
    movw $0x3FB, %dx
    movb $0x03, %al
    outb %al, %dx
    movw $0x3FA, %dx
    movb $0xC7, %al
    outb %al, %dx
    movw $0x3FC, %dx
    movb $0x0B, %al
    outb %al, %dx
    ret

/* al = 一个字符（COM1，轮询 THR 空） */
put_chr:
    pushw %bx
    pushw %dx
    movb %al, %bl
1:
    movw $0x3FD, %dx
    inb %dx, %al
    testb $0x20, %al
    jz 1b
    movw $0x3F8, %dx
    movb %bl, %al
    outb %al, %dx
    popw %dx
    popw %bx
    ret

/* eax = 32 位值，打成 0x????????（值放 esi：put_chr 会动 al） */
put_hex:
    pushl %esi
    pushl %ebx
    pushl %ecx
    movl %eax, %esi
    movb $'0', %al
    call put_chr
    movb $'x', %al
    call put_chr
    movl $28, %ecx
1:
    movl %esi, %ebx
    shrl %cl, %ebx
    andl $0x0F, %ebx
    cmpl $10, %ebx
    jb 2f
    addl $('a' - 10), %ebx
    jmp 3f
2:
    addl $'0', %ebx
3:
    movb %bl, %al
    call put_chr
    subl $4, %ecx
    jns 1b
    popl %ecx
    popl %ebx
    popl %esi
    ret

/* si = 以 NUL 结尾的字符串 */
print:
    movb (%si), %al
    testb %al, %al
    jz 2f
    call put_chr
    incw %si
    jmp print
2:
    ret

/* ---- 数据 ---- */
    .align 4
magic:
    .ascii "C6ISOLDR"
    .long 0                         /* +8  kernel_lba    */
    .long 0                         /* +12 kernel_blocks */
    .long 0                         /* +16 kernel_dst    */
    .long 0                         /* +20 rootfs_lba    */
    .long 0                         /* +24 rootfs_blocks */
    .long 0                         /* +28 rootfs_dst    */
fail_msg:
    .asciz "cd0: read error\r\n"
msg_magic:
    .asciz "cd0: magic0="
msg_param:
    .asciz " k="
msg_r:
    .asciz " r="
msg_eol:
    .asciz "\r\n"
msg_loaded:
    .asciz "cd0: payload loaded\r\n"
msg_pm:
    .asciz "cd0: enter protected mode -> 0x10000\r\n"
drive:
    .byte 0
    .align 4
cur_lba:
    .long 0
blocks_left:
    .long 0
dst_pa:
    .long 0
chunk:
    .long 0
    .align 8
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF        /* 32 位代码，base 0，4GB */
    .quad 0x00CF92000000FFFF        /* 32 位数据，base 0，4GB */
    .quad 0x00009A000000FFFF        /* 16 位代码，base 0，64KB（unreal 回程用） */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
    .align 4
dap:
    .byte 0x10, 0
    .word 0                         /* count（2048 字节块） */
    .word 0                         /* offset */
    .word 0                         /* segment */
    .long 0, 0                      /* lba64 */
