/* ROM 函数表：WiFi/PHY 这些闭源库要调用 ROM 里的函数（memcpy、ets_delay_us…）。
 * IDF 官方就是用这几个 linker script 把符号直接钉到 ROM 地址上的，这里原样引用。 */
INCLUDE port/s3/rom/esp32s3.rom.ld
INCLUDE port/s3/rom/esp32s3.rom.api.ld
/* rom.libc.ld / rom.newlib.ld 必须保持关闭：它们把 memcpy/memset 钉到 ROM，
 * 实测运行期立刻 LoadProhibited（pc=0x40056fc0）。我们用自己的 rt/c6rt.c 实现。 */
INCLUDE port/s3/rom/esp32s3.rom.version.ld

ENTRY(_start)

MEMORY
{
  /* IRAM 只能用 0x403C9000..0x403D9500 这 66KB：再往上 0x403D9500 是 SRAM 坏区
     （DRAM 侧别名 0x3FCD9500，密集 CPU 访问会挂死 CPU，实测越过它的镜像根本
     启动不了）。所以 IDF 闭源大库（libphy/libpp/libnet80211）的代码要放 flash。 */
  IRAM (rx) : ORIGIN = 0x403C9000, LENGTH = 0x10500
  /* 原来只用 DRAM 顶部 116KB（0x3FCE3000 起）。实测 0x3FCA0000 起、下方留 96KB
     给 ROM 之后，RAM-only 镜像在 app 运行期可自由读写整段，240x240 RGB565
     帧缓冲（115200B）因此可以放进内部 RAM。 */
  DRAM (rw) : ORIGIN = 0x3FCA0000, LENGTH = 0x4F000
}

SECTIONS
{
  /* IDF 闭源库的代码放 flash（IROM 0x42000000，LMA 在 flash 0x20000）。
     只在真的链接了这些库时才非空，普通构建里是空段、不产生 flash 段。
     注意字面量池必须跟代码一起搬：l32r 是 PC 相对（往低地址 ±256KB），
     代码和字面量都在 flash 段里就没问题。 */
  /* 放在镜像最前面：VMA 的页内偏移必须和 flash 偏移一致（这里都是 0x20，
     IDF 的 IROM 段也是从 0x42000020 开始），否则 ROM 建 MMU 映射会错位。
     放在最前面还能避免 elf2image 为了对齐插入"load 地址=0"的 PADDING 段
     （那个段会让 ROM 往地址 0 写数据，直接挂）。 */
  /* 先是 DROM（0x3C000000），开头必须是 256 字节的 esp_app_desc_t；
     LMA 从 0x20 开始（镜像头之后）。 */
  .flash.rodata 0x3C000000 : AT (0x20)
  {
    _flash_rodata_lma = LOADADDR(.flash.rodata);
    _flash_rodata_start = .;
    *(.flash.rodata.desc)
    *(.flash.rodata) *(.flash.rodata.*)
    *(.rodata.flash*)
    _flash_rodata_end = .;
  }

  .flash.text 0x42000020 : AT (LOADADDR(.flash.rodata) + SIZEOF(.flash.rodata))
  {
    _flash_text_lma = LOADADDR(.flash.text);
    _flash_text_start = .;
    *(.flash.text) *(.flash.text.*) *(.flash.rodata) *(.flash.rodata.*)
    /* 闭源库暂时**放回 IRAM**（放 flash 需要 IROM 取指，还没打通）：
     * LEAN 构建下 libphy 闭包约 38KB、我们自己的代码约 26KB，合计约 64KB，
     * 正好在 IRAM 预算（66.8KB，坏区 0x403D9500 以下）之内。 */
    _flash_text_end = .;
  }

  /* 字面量池必须在使用它的 l32r 之前；内核镜像 <256KB 时全局前置是安全的
     （M1 之后代码变大时改用 -mno-text-section-literals + --auto-litpools）。 */
  /* 全部可加载段的 LMA 都排在 flash 里（标准 ESP app 镜像布局，
     这样镜像里没有巨大的空洞、IDF bootloader 才会接受）。 */
  .vector : AT (LOADADDR(.flash.text) + SIZEOF(.flash.text))
  {
    . = ALIGN(1024);
    KEEP(*(.vector))
  } > IRAM

  .text : AT (LOADADDR(.vector) + SIZEOF(.vector))
  {
    *(.literal.start)
    *(.text.start)
    *(.literal*)
    *(.text*)
    /* IDF 闭源库（libphy/libpp/libnet80211…）把自己的代码放在 .iram1 里，
     * 不显式收集就会变成"孤儿段"，被链接器塞到段尾甚至不该去的地方。 */
    *(.iram1*)
    *(.iram.*)
    *(.iram0*)
  } > IRAM

  .data : AT (LOADADDR(.text) + SIZEOF(.text))
  {
    . = ALIGN(16);
    *(.rodata*)
    *(.data*)
    *(.dram1*)
    *(.dram0.data*)
    *(.ext_ram.data*)
    . = ALIGN(16);
  } > DRAM

  .bss (NOLOAD) : {
    . = ALIGN(16);
    _bss_start = .;
    *(.bss*)
    *(.dram0.bss*)
    *(.ext_ram.bss*)
    *(COMMON)
    _bss_end = .;
    . = ALIGN(16);
  } > DRAM

  /* 主栈从 0x3FCEF000 向下；链接期强制给栈留 8KB 安全垫，
     防止 BSS 增长后静默压栈。 */
  /* **重要**：这块板子的 DRAM 从 0x3FCD9500 往上是不稳定的（纯填充实验证明：
     代码/数据越过它就起不来）。栈顶原来设在 0x3FCEF000，等于把栈的高端
     ~106KB 放进了坏区 —— 我们自己用栈浅，没踩到；但闭源 PHY 初始化调用很深
     （寄存器窗口 spill 会一路往下写），写到坏区就丢数据，`retw` 恢复出错误
     返回地址，表现就是 "PC 落在指令中间" 的 IllegalInstruction。
     因此栈顶压到坏区以下，栈区间 = [_bss_end+0x1F00, 0x3FCD9000)。 */
  /* 栈顶可由构建选项覆盖（Makefile 的 S3_STACK_TOP）：
   *   - 普通/音频图像构建（bss 很大）：默认 0x3FCEF000
   *   - PHY/闭源库调试构建（深调用）：用 make wphy 传 0x3FCD9000，避开坏区 */
  _stack_top = S3_STACK_TOP;   /* 由 Makefile 用 --defsym 传入，永远有值 */
  _stack_limit = _bss_end + 0x800;   /* 2KB 护栏；大 bss 的构建（音频/图像）也能容下 */
  ASSERT(_stack_limit <= _stack_top, "S3 BSS/stack collision")

  _text_start = ADDR(.text);
  _text_end = ADDR(.text) + SIZEOF(.text);
  _data_start = ADDR(.data);
  _data_end = ADDR(.data) + SIZEOF(.data);
}
