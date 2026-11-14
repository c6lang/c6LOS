# ESP32-S3 自建应用无法从 flash(IROM)执行 —— 问题报告

> 用途：可直接发到 Espressif 论坛 / 提交工单。
> 整理自 `docs/wifi-plan.md` §58~§74 与 `docs/refs/idf-reference-app.md` 的全部真板实验。

---

## 1. 背景

我们在 ESP32-S3 上移植一个自研 RTOS（c6LOS），不用 ESP-IDF 的框架，但希望复用
ESP-IDF 的闭源 WiFi 库（libphy/libpp/libnet80211/libcore/libwapi，合计约 500 KB 代码）。
可用的 IRAM 只有约 66 KB，所以**必须让这些代码跑在 flash 上（IROM）**。

为此我们自己生成了符合 IDF 格式的 app 镜像（`tools/mkimg.py`），用**原厂 bootloader**
引导。镜像本身已被接受并正常启动（`esptool image-info` 报
`Checksum: valid` + `Validation hash: valid`）。

## 2. 现象

应用能正常启动、跑 IRAM 里的代码、写 RTC 寄存器、打印串口。**唯独访问 IROM 窗口
不行**：

```c
/* IROM 段 vaddr = 0x42030000，对应的 flash 物理地址 0x30000，内容已用
   esptool read-flash + cmp 逐字节校验过：2c a2 1d f0（movi.n a2,42; retw.n）*/
uint32_t v = *(volatile uint32_t *)0x42030000u;   /* 得到 0x00000000，不报错 */
```

跳进去执行则：

```
*** TRAP cause=00000000 (IllegalInstruction)  epc1=0x42030014  excvaddr=0x00000000
```

即：**取指/数据读都被"静默地"返回 0**，而且**不抛 Cache error**。

作为对照，一个最小的 ESP-IDF 应用（只 `printf`）在同一块板子上完全正常，它能读回
自己 IROM 页的真实指令字（`0x42000000 -> 0xc0208890`）；而当它去读一个未映射的
IROM 页时会正常 `panic'ed (Cache error)`。

## 3. 硬件与软件环境

| 项目 | 值 |
|---|---|
| 芯片 | ESP32-S3（`esptool` 报 chip_id 9，Xtensa LX7 双核） |
| Flash | 16 MB，DIO，40 MHz |
| bootloader | 原厂固件自带的 2nd-stage bootloader（未修改） |
| 分区表 | `nvs@0x9000`、`phy_init@0xf000`、`factory/app@0x20000`（4 MB） |
| app 镜像 | 我们自己生成，烧到 0x20000 |
| 段布局 | DROM vaddr `0x3C000020`；IROM vaddr `0x42030000`（数据在 flash 0x30000）；其余为 IRAM/DRAM 段 |

## 4. 已经在本板上逐项实测排除的因素

每一项都做过真板实验并有记录（寄存器写后都回读确认生效）：

1. **镜像格式**：`esp_app_desc_t` 在偏移 0x20、段表连续、长度含补齐、末尾 1 字节 XOR
   校验和、SHA-256 附录 —— `esptool image-info` 全部 valid，bootloader 也接受并跳转。
2. **MMU 表项**：`MMU[3] = 0x00000003`（按 `SOC_MMU_INVALID=BIT(14)` 的语义，bit14 为 0
   即有效；页号 3 = flash 0x30000）——**映射是正确的**。恒等映射、重映射、cache 关闭
   状态下手写表项都试过。
3. **ICache/DCache**：`ENABLE` 已置位；`SHUT_CORE0/1_BUS` 为 0；`TAG_POWER_CTRL=5`。
4. **Cache 模式位**：已按 IDF app 的运行态值设为 `ICACHE_CTRL=0x0b`、
   `DCACHE_CTRL=0x09`（含 `WAY_MODE`、`BLOCKSIZE`），回读一致。
5. **ICACHE 失效**：`EXTMEM_ICACHE_SYNC_CTRL(0x600C4088)` 写 `INVALIDATE_ENA`(bit0)
   并轮询 `DONE`(bit1) —— 实测能完成。ROM 的 `Cache_Invalidate_ICache_All()` 调了不返回。
6. **SPI_MEM**：`SPI_MEM_CLOCK` 已按 IDF 值设为 `0x00010001`、`SPI_MEM_CTRL` 设为
   `0x00000000`（均回读确认）。也试过 DIO/DOUT/QOUT 三种 flash 模式。
7. **权限**：`SYSCON_FLASH_ACEn_ATTR/ADDR/SIZE` 实测全 `0xFF`（完全开放）；
   TRM §16 说明 World 控制器默认关闭。`SYSTEM_CACHE_CONTROL_REG` 的
   `ICACHE_CLK_ON/DCACHE_CLK_ON` 位与 IDF app 相同。
8. **时钟**：`SYSTEM_SYSCLK_CONF` 等 SYSCON 时钟寄存器与 IDF app 完全一致。

## 4b. 另外三项排除（后续补充）

- **eFuse**：`espefuse summary` 显示 `DIS_ICACHE=False`、`DIS_DCACHE=False`、
  `DIS_DIRECT_BOOT=False`、`FLASH_TYPE=4 data lines` —— 没有任何熔丝位禁用 cache 或直启。
- **flash 芯片内部状态**：`esptool read-flash-status` 在"我们固件跑过"和"原厂固件跑过"
  两种状态下都读到 `Flash memory status: 0x0200` —— 芯片模式一致。
- **`spi_flash_init_chip_state()` 本身**：读 `components/spi_flash/flash_ops.c:180` 可知，
  在"四线 + 未开 HPM"的默认配置下它是**空函数**（两个分支都不满足，直接 `return ESP_OK`），
  所以它不是我们缺的那一步。真正写 `SPI_MEM_*` 的是 flash 驱动的
  `spi_flash_chip_generic_config_host_io_mode()`。

## 4c. 又排除三条（cache 错误可观测性 / 时钟电源 / MMU 表项范围）

- **cache 错误状态寄存器**（IDF 靠 `esp_cache_err_int_init()` 才打开的中断）：
  清状态后做一次 IROM 读，再读：
  `EXTMEM_CACHE_ILG_INT_ST = 0`、`EXTMEM_MMU_FAULT_VADDR = 0`
  ⇒ **没有任何非法访问、也没有 MMU 表项错误记录**。
- **cache 时钟/电源**：`SYSTEM_CACHE_CONTROL_REG(0x600C0048) = 0x5`（两个 cache 时钟都开）、
  `EXTMEM_CLOCK_GATE(0x600C4164) = 0x1`、`EXTMEM_CACHE_MMU_POWER_CTRL(0x600C412C) = 0x5`。
- **MMU 表项范围**：`Cache_Get_IROM_MMU_End() = 0x200`（=512，整张表归 IROM），
  `Cache_Get_DROM_MMU_End() = 0x400`；我们用的 entry 3 在 IROM 范围内。

## 5. 最关键的对照数据

两边都用**各自应用内的代码**读取（JTAG 读这个寄存器块不可靠，实测 IDF app 的 MMU 表
经 JTAG 全读成 `0x4000`，实际是 `0x00000002`）：

| 寄存器 | IDF app（IROM 正常） | 我们的 app |
|---|---|---|
| `MMU[0]` 0x600C5000 | `0x00000002` | `0x00000002` |
| `EXTMEM_ICACHE_CTRL` 0x600C4060 | `0x0000000b` | `0x00000001` → 已改为 `0x0b` |
| `EXTMEM_DCACHE_CTRL` 0x600C4000 | `0x00000009` | `0x00000001` → 已改为 `0x09` |
| `SPI_MEM_CLOCK` 0x60002014 | `0x00010001` | `0x80000000` → 已改为 `0x00010001` |
| `SPI_MEM_CTRL` 0x60002008 | `0x00000000` | `0x012ca008` → 已改为 `0x00000000` |
| 读 IROM 页 | `0xc0208890`（真实指令） | `0x00000000` |

**把上表右列全部改成和左列一致后，IROM 访问依然返回 0，且依然不抛 Cache error。**

## 6. 想请教的问题

1. ESP32-S3 上，**除了 MMU 表项、Cache 使能/模式位、SPI_MEM 时钟与控制、PMS 权限**
   之外，还有哪些**一次性/启动阶段**的配置决定"CPU 能否通过 cache 访问外部 flash"？
   （我们怀疑差异在 bootloader 引导阶段，事后写寄存器无法弥补。）
2. 为什么访问 IROM 窗口时是**静默返回 0** 而不是像 IDF app 那样抛 `Cache error`？
   这种"不响应"是否对应某个具体的权限/时钟/复位状态？
3. 用**自定义镜像 + 原厂 2nd-stage bootloader** 时，是否有一些隐式契约（例如
   bootloader 只在特定条件下才真正启用 cache）需要满足？
4. 是否提供 `spi_flash_init_chip_state()` / `ext_mem_init()` 这类初始化在
   **非 IDF 环境**下的最小可用参考实现？

## 6b. 一个重要澄清（避免误判）

我们一开始把"不抛 Cache error"当作"访问没走到 cache"的证据，这是**错的**：

- IDF 源码注释（`esp_system/port/soc/esp32s3/cache_err_int.c:11-15`）说明，cache 在
  未使能时对外部存储区域的访问会触发中断，但**该中断复位默认是关闭的**
  （`EXTMEM_CACHE_ILG_INT_ENA(0x600C40DC)`、`EXTMEM_CORE0_ACS_CACHE_INT_ENA(0x600C40E8)`
  全为 0）。IDF app 会 panic 只是因为它显式调用了 `esp_cache_err_int_init()`。
- TRM §15.6 明确写：非法访问会被**过滤并返回默认值**（取指/读返回 0 或 `0xdeadbeaf`），
  中断是否触发取决于是否使能。

所以正确表述是：**我们看到的 `0x00000000` 与"访问被过滤后返回默认值"一致，
只是没有装报警器所以看不到错误事件**。

## 7. 可提供的复现材料

- 我们生成的 app 镜像与生成脚本（`tools/mkimg.py`，纯 Python，可读）
- 全部实验记录：`docs/wifi-plan.md` §58~§74、`docs/refs/*.md`
- 一个最小 ESP-IDF 参照工程（同一块板上正常），含寄存器探测代码

---

## 附：SPI_MEM 区块的运行态对比（两边都用各自应用内的代码读取）

```
偏移    IDF app（IROM 正常）   我们的 app        备注
0x00    0x00000000            0x00000000        CMD
0x04    0x00000000            0x00000000        ADDR
0x08    0x00000000            0x012ca008    ★   CTRL
0x0C    0x00000ffc            0x00000ffc        CTRL1  一致
0x10    0x00000000            0x00000000        CTRL2  一致
0x14    0x00010001            0x80000000    ★   CLOCK
```

只有 `CTRL` 和 `CLOCK` 两个不同。**我们已经把这两个寄存器按左列的值写入并回读确认生效，
IROM 访问仍然返回 0。** 因此可以确认差异不在这些"事后可写"的寄存器上。

## 附二：**在自己的环境里**写这两个 SPI_MEM 寄存器会把应用弄死

进一步实验发现：

- 写 `SPI_MEM_CTRL(0x60002008) = 0x00000000` → 应用当场失去对 SPI 控制器的访问（崩溃）
- 只写 `SPI_MEM_CLOCK(0x60002014) = 0x00010001` → 同样崩溃

也就是说 **bootloader 留给我们的 `0x012ca008` / `0x80000000` 才是本环境下"能用"的值**
（bootloader 正是用这套配置把我们镜像读进来并校验通过的）。IDF app 之所以显示
`0 / 0x00010001`，是它**自己初始化后**的状态，不能简单照搬。

⇒ 更加确认：问题不在这些"事后可写"的寄存器，而在**引导阶段完成的一次性配置**。

---

## 附三：**把同一个 MMU 表项写进去，IDF app 能读，我们不能**

这是目前为止最有力的一组对照实验。

在**能正常工作的 IDF app** 里加三行：

```c
printf("before mmu3=%08x\n", *(volatile unsigned*)0x600C500C);   // 0x00004000（无效）
*(volatile unsigned*)0x600C500C = 3u;                             // 映射 entry3 -> flash 页 3
printf("after  mmu3=%08x\n", *(volatile unsigned*)0x600C500C);   // 0x00000003（有效）
printf("read 0x42030000 = %08x\n", *(volatile unsigned*)0x42030000u);
```

真板输出：

```
after  mmu3=00000003
read 0x42030000 = 20c02088      ← 真实数据（不是 0）
```

在**我们的 app** 里做同样的事（bootloader 本来就把 entry3 映射成页 3）：

```
irom3=0x00000000                ← 永远返回 0
```

补充：我们还把
- cache 模式位对齐成 IDF 运行态的 `ICACHE_CTRL=0x0b` / `DCACHE_CTRL=0x09`
- 经 windowed 孤岛成功调用了 ROM 的 `Cache_Invalidate_ICache_All()`（信标确认返回）

之后再读，**仍然是 0**。

⇒ 结论：**问题不在 MMU 表项、不在 cache 模式、不在 flash 内容**，
而在于「我们这个 app 的运行时状态和 IDF app 有某种本质差别」，
使得 cache 对 IROM 的访问被静默吞掉。

**这一条最适合作为排查线索**：请 Espressif 帮我们指出，还有什么东西会让
"cache 访问外部存储器的请求"被静默丢弃（不响应、不报 Cache error）。
