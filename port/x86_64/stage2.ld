ENTRY(_start64)
PHDRS
{
    text PT_LOAD FLAGS(5);      /* R+X */
    data PT_LOAD FLAGS(6);      /* R+W */
}
SECTIONS
{
    /* 基址紧贴 stage1（0x10000，实际只占 ~200B）之后，别再留到 0x20000：
     * 0x11000 起镜像有 0x8F000（572KB）直到 0xA0000 空洞，旧基址白白浪费
     * 60KB —— Alpine 全量镜像 + 网络栈就是靠这 60KB 才重新塞得下。 */
    . = 0x11000;
    .text : { *(.text.entry) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    } :data
    /* 0xA0000-0xFFFFF 是 VGA/BIOS 空洞，不是 RAM：内核镜像（含 .bss）一旦越过
     * 0xA0000，静态变量就落在设备窗口上，写它就是写设备寄存器。在链接期拦住，
     * 比在 QEMU 里看 BQL 断言好查得多。 */
    /* 裸内核不跑 C++ 异常/unwind，.eh_frame 只是编译器附带的展开表；
     * 丢掉它省下约 6.7KB，给 0xA0000 以下的镜像留出余量。 */
    /DISCARD/ : { *(.eh_frame) *(.eh_frame_hdr) }
    ASSERT(__bss_end <= 0xA0000, "kernel image crosses the VGA/BIOS hole (0xA0000)")
}
