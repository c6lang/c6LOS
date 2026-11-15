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

### 最终结论（构建 NuttX）

继续修配置时又出现 `CONFIG_NFILE_DESCRIPTORS_PER_BLOCK` 未定义等一串 **Kconfig 派生符号**
缺失 —— 说明手工补 `.config` 这条路收敛不了，NuttX 必须走它自己的 Kconfig 生成流程。

**但从源码已经拿到了我们要的答案**（见上面 NuttX 启动代码那一段），
构建本身对我们的目标不是必需的。已克隆的源码保留在 `/tmp/nuttx-full`（1.2G）与
`/tmp/apps-full`，需要时可以继续。

## 验证：把镜像烧到 0x0，让 **ROM 直接引导**（NuttX/Zephyr 的 simple-boot 方式）

Zephyr 的注释明确说："In simple boot mode, **ROM bootloader has already set up MMU
mappings** for flash access"。NuttX 的 `CONFIG_ESPRESSIF_SIMPLE_BOOT` 也是
`PRIMARY_SLOT_OFFSET = 0` —— 镜像烧在 flash 0x0，由 ROM 直接引导。

我们用 `mkimg.py ... 0x0` 生成镜像烧到 0x0 试了一次：

```
SHA-256 comparison failed: ... Attempting to boot anyway...
entry 0x403788f0
c6os s3 boot          <- 我们的应用起来了！
```

**ROM 完全接受我们的镜像并成功引导**（`Checksum valid`）—— 这本身是个好消息，
说明镜像格式没问题。

但是：**读 IROM 仍然是 0**。

⇒ 连"ROM 直接引导"这种方式都不行，说明**问题不在 bootloader 这一侧，
而在我们镜像本身**（链接/段属性/某些段字段）。
这是一个重要的收窄：之前一直怀疑"原厂 2nd-stage bootloader 没把 XIP 交给我们"，
现在这条也排除了。

## 逐段比对发现的**结构性差异**：DROM 被撑到 64KB，和 IROM 抢 MMU 表项

```
我们的镜像 (BASE=0)                      IDF 镜像
seg0 DROM vaddr 0x3C000020 len 0x0FFD8   seg0 DROM vaddr 0x3C010020 len 0x0913C
seg1 IROM vaddr 0x42030000 len 0x00018   seg3 IROM vaddr 0x42000020 len 0x0FEC0
     文件偏移 0xFFF8                            文件偏移 0x10018
```

我们的 DROM 段被撑到了 **0x0FFD8（约 64KB）** —— 这是为了让 IROM 段的
"vaddr 低 16 位 == 数据在 flash 上的低 16 位"这条约束成立而做的填充。

后果：bootloader 会认为 **DROM 占了 64 个 MMU 页（entry 0~63）**，
而我们的 IROM 又要求 **entry 3**（`(0x42030000 & 0x1FFFFFF) >> 16 = 3`）
—— 正好落在 DROM 的页范围内。

IDF/Zephyr 都会调 `Cache_Set_IDROM_MMU_Size(irom_pages, drom_pages, ...)`
把 512 项表在 IROM 和 DROM 之间切分（IDF 里 IROM 从 0 开始，DROM 从 irom_pages 开始），
所以正常镜像的 DROM vaddr 是 `0x3C010020` 这种"从 IROM 页数之后开始"的地址。
我们的布局很可能让这个切分算错，导致 IROM 的 entry 3 实际指向别处或被覆盖。

### 下一步（明确）

把 DROM 段做小（不做 64KB 填充），并让 IROM/DROM 的 vaddr 落在
`Cache_Set_IDROM_MMU_Size` 预期的那套切分里 —— 即模仿 IDF 的布局：
DROM 在 `0x3C0xxxxx`（x 按 IROM 页数定），IROM 在 `0x42000020` 起。
