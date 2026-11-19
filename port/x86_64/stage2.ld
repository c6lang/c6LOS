ENTRY(_start64)
PHDRS
{
    text PT_LOAD FLAGS(5);      /* R+X */
    data PT_LOAD FLAGS(6);      /* R+W */
}
SECTIONS
{
    . = 0x20000;
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
    ASSERT(__bss_end <= 0xA0000, "kernel image crosses the VGA/BIOS hole (0xA0000)")
}
