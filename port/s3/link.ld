ENTRY(_start)

MEMORY
{
  IRAM (rx) : ORIGIN = 0x403C9000, LENGTH = 0x17000
  DRAM (rw) : ORIGIN = 0x3FCE3000, LENGTH = 0x1D000
}

SECTIONS
{
  /* 字面量池必须在使用它的 l32r 之前；内核镜像 <256KB 时全局前置是安全的
     （M1 之后代码变大时改用 -mno-text-section-literals + --auto-litpools）。 */
  .vector : {
    . = ALIGN(1024);
    KEEP(*(.vector))
  } > IRAM

  .text : {
    *(.literal.start)
    *(.text.start)
    *(.literal*)
    *(.text*)
  } > IRAM

  .data : {
    . = ALIGN(16);
    *(.rodata*)
    *(.data*)
    . = ALIGN(16);
  } > DRAM

  .bss (NOLOAD) : {
    . = ALIGN(16);
    _bss_start = .;
    *(.bss*)
    *(COMMON)
    _bss_end = .;
    . = ALIGN(16);
  } > DRAM

  /* 主栈从 0x3FCEF000 向下；链接期强制给栈留 8KB 安全垫，
     防止 BSS 增长后静默压栈。 */
  _stack_top = 0x3FCEF000;
  _stack_limit = _bss_end + 0x1F00;
  ASSERT(_stack_limit <= _stack_top, "S3 BSS/stack collision")

  _text_start = ADDR(.text);
  _text_end = ADDR(.text) + SIZEOF(.text);
  _data_start = ADDR(.data);
  _data_end = ADDR(.data) + SIZEOF(.data);
}
