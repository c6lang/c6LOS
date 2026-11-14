# TRM §17 系统寄存器中的 cache 相关控制（ESP32-S3 TRM v1.8）

## SYSTEM_CACHE_CONTROL_REG（0x60026048）

| 位 | 名称 | 说明（原文） |
|---|---|---|
| bit0 | `SYSTEM_ICACHE_CLK_ON`    | 置 1 使能 i-cache 时钟。(R/W) |
| bit1 | `SYSTEM_ICACHE_RESET`     | 置 1 复位 i-cache。(R/W) |
| bit2 | `SYSTEM_DCACHE_CLK_ON`    | 置 1 使能 d-cache 时钟。(R/W) |
| bit3 | `SYSTEM_DCACHE_RESET`     | 置 1 复位 d-cache。(R/W) |

**实测（真板）**：我们自己的固件和能正常跑 IROM 的原厂固件，这个寄存器**都读回
`0x00001000`**（bit0/bit2 都是 0）。手动置位后仍然取指失败 ⇒ **这两位与取指无关**，
不要在这上面浪费时间。

## SPI0/SPI1 控制器：文档不公开寄存器

TRM 第 30 章开头明确写道：

> SPI0 和 SPI1 控制器主要供**内部使用**以访问外部 flash 及 PSRAM。本章节主要介绍
> GP-SPI 控制器，即 GP-SPI2……

⇒ **cache 用的 `SPI_MEM_*` 寄存器块官方文档不公开**。所以这部分只能靠
（a）对照能工作的固件（我们已经做过，与原厂逐字节一致），或
（b）直接读 IDF 的 `soc/esp32s3/register/soc/spi_mem_reg.h`（寄存器名/偏移可信，
但"什么时候该写什么值"没有文档）。

## EXTMEM（cache 控制器）相关寄存器（来自 `extmem_reg.h`）

| 偏移 | 名称 | 备注 |
|---|---|---|
| 0x60 | `EXTMEM_ICACHE_CTRL` | bit0 ENABLE，bit1 WAY_MODE，bit2 SIZE_MODE，bit3 BLOCKSIZE_MODE |
| 0x64 | `EXTMEM_ICACHE_CTRL1` | bit0/1 = SHUT_CORE0/1_BUS |
| 0x68 | `EXTMEM_ICACHE_TAG_POWER_CTRL` | 实测两边都是 5 |
| 0x7C | `EXTMEM_ICACHE_LOCK_CTRL` | |
| 0x88 | `EXTMEM_ICACHE_SYNC_CTRL` | bit0 启动，bit1 SYNC_DONE（实测能完成） |
| 0x8C/0x90 | `EXTMEM_ICACHE_SYNC_ADDR/SIZE` | |
| 0x94 | `EXTMEM_ICACHE_PRELOAD_CTRL` | |
| 0x00/0x04 | `EXTMEM_DCACHE_CTRL` / `CTRL1` | 实测两边都是 1 / 0 |
