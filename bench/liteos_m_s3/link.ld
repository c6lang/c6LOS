ENTRY(_start)
MEMORY {
  IRAM (rx) : ORIGIN = 0x403C9000, LENGTH = 0x17000
  DRAM (rw) : ORIGIN = 0x3FCE3000, LENGTH = 0x1D000
}
SECTIONS {
  .iram0.vectors : {
    _init_start = ABSOLUTE(.);
    . = 0x0; KEEP(*(.WindowVectors.text));
    . = 0x180; KEEP(*(.InterruptEntry2.text));
    . = 0x1c0; KEEP(*(.InterruptEntry3.text));
    . = 0x200; KEEP(*(.InterruptEntry4.text));
    . = 0x240; KEEP(*(.InterruptEntry5.text));
    . = 0x280; KEEP(*(.DebugExceptionHandler.text));
    . = 0x2c0; KEEP(*(.NMIExceptionHandler.text));
    . = 0x300; KEEP(*(.KernelExceptionHandler.text));
    . = 0x340; KEEP(*(.UserExceptionHandler.text));
    . = 0x3C0; KEEP(*(.DoubleExceptionHandler.text));
    . = 0x400; *(.userVector) *(.*_vector.literal)
    . = ALIGN(16);
    *(.entry.text) *(.init.literal) *(.init)
    _init_end = ABSOLUTE(.);
  } > IRAM
  .text : { *(.literal .text .literal.* .text.* .iram* .iram1 .iram1.*) } > IRAM
  .data : { . = ALIGN(16); *(.rodata*) *(.data*) . = ALIGN(16); } > DRAM
  .bss (NOLOAD) : {
    . = ALIGN(8); _sbss = .;
    *(.bss*) *(COMMON)
    . = ALIGN(16); __heap_start = .; . += 0x8000; __heap_end = .;
    . = ALIGN(16); _ebss = .;
    . += 0x4000; __stack_top = .;
  } > DRAM
}
