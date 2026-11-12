ENTRY(_start)

MEMORY
{
  IRAM (rx) : ORIGIN = 0x40370000, LENGTH = 0x70000
  DRAM (rw) : ORIGIN = 0x3FC88000, LENGTH = 0x78000
}

SECTIONS
{
  /* 字面量池必须在使用它的 l32r 之前；内核镜像 <256KB 时全局前置是安全的
     （M1 之后代码变大时改用 -mno-text-section-literals + --auto-litpools）。 */
  .text : {
    *(.literal.start)
    *(.text.start)
    *(.literal*)
    *(.text*)
  } > IRAM

  .rodata : {
    *(.rodata*)
  } > IRAM

  .data : {
    *(.data*)
  } > DRAM

  .bss (NOLOAD) : {
    _bss_start = .;
    *(.bss*)
    *(COMMON)
    _bss_end = .;
  } > DRAM

  _text_start = ADDR(.text);
  _text_end = ADDR(.text) + SIZEOF(.text);
  _data_start = ADDR(.data);
  _data_end = ADDR(.data) + SIZEOF(.data);
}
