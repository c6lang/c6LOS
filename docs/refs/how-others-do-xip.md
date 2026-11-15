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

## 我们照着做了，还是不行

按 NuttX / esp-hal 的做法，在 app 里自己建立 XIP：

```c
st = s3_win_call_cache_suspend_icache();          /* ROM Cache_Suspend_ICache @0x4000189c */
os_reg_wr(0x600C500C, 0x00000003);                 /* entry3 -> flash 页 3 */
os_reg_wr(0x600C5000, 0x00000002);                 /* entry0 -> flash 页 2 */
os_reg_wr(0x600C4064, os_reg_rd(0x600C4064) & ~3); /* 总线不关 */
s3_win_call_cache_invalidate_addr(0x42030000, 0x10000);  /* ROM Cache_Invalidate_Addr @0x400016b0 */
s3_win_call_cache_resume_icache(st);               /* ROM Cache_Resume_ICache @0x400018a8 */
```

真板结果：

```
slot0 = 0xB00000B0   进 os_main
slot3 = 0x00000000   suspend 返回的旧状态（可疑：应为 1）
slot2 = 0x00000000   IROM 读仍是 0
slot1 = 0x00000000   取指调用没返回
```

⇒ **在 app 里重建 XIP 这条路也走不通**。`suspend` 返回值就是 0 这一点也可疑，
说明这些 ROM cache 函数在我们这个环境里行为不正常（之前也观察到
`Cache_Enable_ICache` 会让应用崩溃）。

**这条信息本身很有价值**：ROM 的 cache 维护函数在 IDF app 里正常、在我们 app 里异常，
进一步说明差异在**更底层/更早的阶段**，不是"应用该怎么调函数"的问题。

## 尝试真正构建 NuttX（进展与卡点）

按用户要求，实际尝试在开发机上构建 NuttX 跑这块板子：

**已完成**
- `git clone --depth 1` 完整克隆 nuttx（578M）+ nuttx-apps
- 装好构建依赖：`gperf`、`bison`、`flex`、`ninja`、`kconfiglib`（`pip3 --break-system-packages`）
- 按 NuttX 期望的目录布局放成 `../nuttx` + `../apps` 兄弟目录
- `./tools/configure.sh esp32s3-devkit:nsh` **成功**（rc=0）
- `git submodule update --init --recursive` 拉下 Espressif HAL（仓库涨到 1.2G）
- 交叉编译器认对：`xtensa-esp32s3-elf-gcc`

**卡点**
1. `-Wstack-usage=`（空参数）→ 手工补 `CONFIG_STACK_USAGE_WARNING=2048` 解决
2. `#error Unknown XTENSA architecture` → 手工补 `CONFIG_ARCH_FAMILY_LX7=y` 解决
3. 最后卡在：`没有规则可制作目标 .../bootloader_flash_config_.c`
   —— 文件名里的 `$(CHIP)` 后缀展开为空，说明**手工编辑 `.config` 不足以让 Kconfig
   派生变量全部正确**，需要走 NuttX 的完整 `menuconfig` 流程

**结论**：NuttX 能配起来，但正确构建要走它自己的 Kconfig 生成流程（或用它的 CI 脚本），
不是手工补 `.config` 能糊过去的。**不过这轮真正的收获是它的源码** ——
`esp32s3_start.c` 里那两行（`bootloader_init()` + `map_rom_segments()`）已经明确告诉我们
"非 IDF 应用是怎么建立 XIP 的"，这比把它编出来更有价值。
