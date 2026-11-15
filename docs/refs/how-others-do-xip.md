# 别人是怎么让 ESP32-S3 从 flash 执行代码的（源码调研）

源码已克隆在 `/tmp/xipresearch/`：`nuttx` / `esp-hal` / `zephyr` / `esp32-baremetal-zig`。

## 结论先行：**非 IDF 项目的 app 会自己建立 XIP，而不是完全依赖 IDF 的 2nd-stage bootloader**

NuttX 的 ESP32-S3 启动代码（`arch/xtensa/src/esp32s3/esp32s3_start.c:530-560`）：

```c
#ifdef CONFIG_ESPRESSIF_SIMPLE_BOOT
  if (bootloader_init() != 0) {          /* ← 在 app 里重新跑一遍 bootloader 的初始化 */
      ets_printf("Hardware init failed, aborting\n");
      while (true);
  }
#endif
  if (map_rom_segments(app_drom_start, app_drom_vaddr, app_drom_size,
                       app_irom_start, app_irom_vaddr, app_irom_size) != 0) {
      ets_printf("Failed to setup XIP, aborting\n");   /* ← 映射失败就明确报错 */
      while (true);
  }
```

`map_rom_segments()` 在 `arch/xtensa/src/common/espressif/esp_loader.c`：
- 先按 `MMU_FLASH_MASK` 对齐 flash 起始地址与 vaddr；
- 在 SIMPLE_BOOT 模式下**自己读镜像头、遍历段表**（`bootloader_flash_read` +
  `esp_image_header_t` / `esp_image_segment_header_t`）；
- 再逐页写 MMU 表。

而且 NuttX 的 `CONFIG_ESPRESSIF_SIMPLE_BOOT` 把 `PRIMARY_SLOT_OFFSET` 定义为 **0**
—— 也就是说这种模式下**镜像本身就是烧在 flash 0x0 的那个"bootloader"**，
它自己完成硬件初始化 + 自我映射，然后才跑 NuttX 内核。

## 另一条路：esp-hal（Rust）

esp-hal 同时提供两种 bootloader crate：

- `esp-bootloader-esp-idf` —— 用原厂 IDF bootloader
- `esp-bootloader-direct-boot` —— **直接启动**（跳过 IDF 2nd-stage bootloader）

说明"用原厂 bootloader 引导非 IDF 应用"这条路**存在**，但作者专门做了 direct-boot
方案，意味着前者有坑。

esp-hal 的 flash/MMU 代码（`esp-hal/src/flash/mmu.rs`）里有两句注释很关键：

> Writing the entry and making it visible to the cache must happen in one cache-off window.

> The cache must stay on. Callers park the other core and disable interrupts.

它用的是**按地址失效**（`invalidate_cache(vaddr, page_size)` → ROM `Cache_Invalidate_Addr`
@ `0x400016b0`），而不是整片失效。

## 对我们的启示（按优先级）

1. **不要假设"原厂 bootloader 会把 XIP 完整交给我们"** —— NuttX 的做法是
   在 app 里再跑一次 `bootloader_init()` 并**自己调 map_rom_segments**。
   我们现在只依赖 bootloader 建的 `MMU[3]=3`，很可能缺了 `bootloader_init()`
   里那些"让 flash/cache 进入正式工作状态"的部分。
2. 映射失败要有明确判据（NuttX 就是 `ets_printf("Failed to setup XIP")` 然后停住），
   而不是像我们这样静默读回 0。
3. 失效用 **`Cache_Invalidate_Addr`（按地址）**，并且**写表项 + 失效要在同一个
   cache 关闭窗口内**完成。
4. 可以考虑改用 `esp-bootloader-direct-boot` 那种模式：镜像烧 0x0、自己初始化、自己映射。
