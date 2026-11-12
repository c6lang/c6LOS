ENTRY(_start)
SECTIONS
{
  . = 0x403C9000;
  .text : { *(.text*) *(.literal*) *(.rodata*) }
}
