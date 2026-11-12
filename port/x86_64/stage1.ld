ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .stage2 0x20000 : { *(.stage2) }
    .bss : {
        *(COMMON)
        *(.bss*)
    }
}
