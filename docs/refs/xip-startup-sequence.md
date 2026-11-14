# ESP32-S3 从 flash 取指(XIP)的完整启动序列（子代理源码分析结论）

> 分析对象：IDF v6.0.1 @ `/media/winger/inst_26/esp/esp-idf`，对照本项目 `port/s3/`。
> 所有地址来自 `components/soc/esp32s3/register/soc/*.h`（TRM 的机器可读形式）。

## 三个贯穿全文的关键结论

1. **MMU 表项 bit14 = 1 表示"无效"**（`SOC_MMU_INVALID = BIT(14)`，`SOC_MMU_VALID = 0`）。
   有效映射就是**只写页号** `paddr >> 16`。我们 `os_flash_map()` 曾多写 `| 0x4000u`，
   等于把每个映射都标成无效 —— **已修正**。
2. **`ext_mem_init()` 根本不建 MMU 映射**。建映射是 bootloader 的
   `set_cache_and_start_app()` 干的（`bootloader_utility.c:1035-1136`）；
   app 侧只做"重新配置 cache 形态"。所以"照抄 `cpu_start.c`"永远建不出映射。
3. `spi_flash_init_chip_state()` 在非 OPI、非 HPM 配置下是**空函数**
   （`spi_flash/flash_ops.c:180-190`），它救不了取指。

## bootloader 阶段（真正建立 XIP 的地方）

| 步骤 | 做什么 | 寄存器 / 值 |
|---|---|---|
| B2 | 打开 dummy 输出与数据线极性 | `SPI_MEM_CTRL(0x60002008) \|= FDUMMY_OUT(bit3) \| D_POL(bit19) \| Q_POL(bit18)` |
| B4 | 唤醒 flash（退出 deep power-down） | flash 命令 `0xAB` |
| B7 | **关 cache → 改 flash 参数 → 开 cache** | `cache_hal_disable` / `esp_rom_spiflash_config_param` / `cache_hal_enable` |
| B8 | 开 cache + 打通总线 | `ICACHE_CTRL(0x600C4060) \|= 1`、`DCACHE_CTRL(0x600C4000) \|= 1`、`ICACHE_CTRL1(0x600C4064) &= ~0x3`、`DCACHE_CTRL1(0x600C4004) &= ~0x3` |
| B9 | MMU 复位（512 项全置无效） | `0x600C5000 + i*4 = 0x4000` |
| B10 | **关 cache（改映射前必须）** | ROM `Cache_Disable_ICache/DCache` |
| B12/B14 | **映射 DROM / IROM** | `0x600C5000 + entry*4 = (paddr>>16)`，`entry = (vaddr & 0x1FFFFFF) >> 16` |
| B15 | 放通 core0 总线（复位默认是关的！） | `0x600C4064 &= ~BIT0`、`0x600C4004 &= ~BIT0` |
| B16 | 开 cache | ROM `Cache_Enable_ICache(autoload)` + `Cache_Enable_DCache(autoload)` |

## 最小可复刻序列（只要"能从 IROM 取指"）

```c
/* 1) 关 cache（改表前必须） */
*(volatile u32*)0x600C4060 &= ~1u;
*(volatile u32*)0x600C4000 &= ~1u;

/* 2) 建映射：entry = (V & 0x1FFFFFF) >> 16，值 = P >> 16（bit14 = 0 才有效！） */
for (i = 0; i < pages; i++)
    *(volatile u32*)(0x600C5000u + (entry + i) * 4) = (P >> 16) + i;

/* 3) 放通 core0 总线（复位默认关） */
*(volatile u32*)0x600C4064 &= ~0x3u;
*(volatile u32*)0x600C4004 &= ~0x3u;

/* 4) 清 I-cache tag（ROM 是 windowed ABI，必须走 win_island） */
s3_win_cache_invalidate();      /* -> Cache_Invalidate_ICache_All() @0x400016d4 */

/* 5) 开 cache */
*(volatile u32*)0x600C4060 |= 1u;
*(volatile u32*)0x600C4000 |= 1u;

/* 6) 推荐：走 ROM 官方入口，它会顺带把 autoload / CACHE_REQ_EN 配好 */
s3_win_cache_enable();          /* -> Cache_Enable_ICache(0) @0x40001878 */
```

## 本项目代码里的问题（子代理指出，均已记录）

| # | 问题 | 状态 |
|---|---|---|
| 1 | `os_flash_map()` 写 `\| 0x4000u` ⇒ 映射被标成无效 | **已修正** |
| 2 | `t_s3_irom*.c6` 里映射的页（0x42100000/entry 16）与 `.flash.text` 实际所在页（0x42030000/entry 3）不一致 | 待修 |
| 3 | `os_flash_init()` 被 `#ifdef S3_FLASH_AUTO_MAP` 包住，而全仓库没定义该宏 ⇒ 是空函数 | 待处理 |
| 4 | `os_icache_enable()` 只开 I 侧，DCACHE 只在 DROM 分支才开 | 待处理 |
| 5 | `SPI_MEM_CTRL` 被写成 `0x00608000`（`FASTRD_MODE=0`、`D_POL/Q_POL` 被清零 = 慢读），而 `0x00ECA008` 才是正确 DIO 快读 | 待复核 |
| 6 | `SPI_MEM_CACHE_FCTRL(0x6000203C)` 的 `CACHE_REQ_EN(bit0)` 从没检查过 | **实测为 0** |

## 唯一还需上板确认的一点

`SPI_MEM_CACHE_FCTRL`（**0x6000203C**）bit0 `CACHE_REQ_EN`：

> "Set this bit to enable Cache's access and SPI0's transfer"，复位默认 **0**。

IDF 全树没有任何 C 代码写它 —— 是 ROM 的 `Cache_Enable_ICache`/`Cache_Read_Enable` 置位的。
我们绕过了 ROM 只写 `0x600C4060`，所以它可能是 0。

**实测：我们固件里 `0x6000203C = 0x00000000`（`CACHE_REQ_EN = 0`）**，而手工写 bit0
不生效、调 ROM 的 `Cache_Enable_ICache` 会让应用崩溃 —— 这条线索还悬着。

## 实测：`CACHE_REQ_EN` 也不是原因（它本来就是 0）

给能正常工作的 IDF app 加打印：

```
FCTRL 00000000  ICACHE 0000000b  IROM c0208890  SPICLK 00010001
```

- `0x6000203C (CACHE_FCTRL) = 0` —— **`CACHE_REQ_EN` 本来就是 0**，和我们的固件一样。
  而且它 `IROM = 0xc0208890` 读得好好的 ⇒ 这一位不是取指的前提。该假设排除。
- 唯一的差别仍然只有 **`SPICLK`**：IDF `0x00010001` vs 我们 `0x80000000`。

⇒ 到这一步，两边**唯一**可测到的差异就是 `SPI_MEM_CLOCK` 这一个寄存器，
而它是 **bootloader 根据我们的镜像配出来的**。手工写 `0x00010001` 会让我们的应用崩溃，
说明这个值必须和本环境的时钟源匹配（IDF app 跑 160MHz PLL，我们跑 40MHz XTAL）。

**因此下一步的重点不是"值该填多少"，而是"为什么 bootloader 给我们填了 0x80000000"。**
