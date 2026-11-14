# 可用的 IDF 参照 app（二分 IROM 问题的基准）

## 为什么需要它

我们自己的固件跳进 flash 时取指/数据读都返回 0，而**原厂固件和任何 IDF app 都能正常从
flash 执行**。为了定位差异，需要一个"最小、干净、一定能跑"的 IDF app 作为对照物。

## 环境（已装好，工具链在数据盘）

```sh
export IDF_TOOLS_PATH=/media/winger/inst_26/esp/.espressif   # 6.2G，放数据盘
. /media/winger/inst_26/esp/esp-idf/export.sh
```

- IDF 源码：`/media/winger/inst_26/esp/esp-idf`（v6.0.1）
- 工具链/虚拟环境：`/media/winger/inst_26/esp/.espressif`
  （原来是 `~/.espressif`，因为 `/home` 满而迁到数据盘；`tools/dbg.sh` 已跟着改）
- 踩过的坑：IDF 5.3 的 venv 不适用于 6.0.1；`xtensa-esp-elf esp-15.2.0` 曾因安装中断
  而缺 `cc1`，删掉该目录重装即可

## 最小 app

`/tmp/idfref/`：`app_main` 里循环 `printf("IDFREF alive n=%d\n")`。
**实测真板输出正常**：

```
IDFREF alive n=0
IDFREF alive n=1
...
```

⇒ 这块板子的 IROM 取指完全正常，问题在我们自己的镜像/启动路径。

## 寄存器对照（IDF 参照 app 运行中 vs 我们的 app）

同一批地址（EXTMEM 0x600C4000+0x100、SPI_MEM 0x60002000+0x80、
PMS 0x60026028+0x30、MMU 0x600C5000+0x80 共 140 个）逐字 diff，只剩 9 处不同：

```
0x600C402C  idf=0x3c010000 ours=0x3c800000   DCACHE_SYNC_ADDR（噪声）
0x600C40C8  idf=0x00000000 ours=0x00000001   访问计数器（噪声）
0x600C40CC  idf=0x00000000 ours=0x00000001
0x600C40D4  idf=0x00000030 ours=0x00000d55   计数器（噪声）
0x600C40D8  idf=0x0000007e ours=0x000034e7
0x60002008  idf=0x00608000 ours=0x016ca008   ★ SPI_MEM_CTRL 不同
0x60002014  idf=0x00030103 ours=0x80000000   ★ SPI_MEM_CLOCK 完全不同
0x600C5000  idf=0x00004000 ours=0x00000002   ★ MMU[0]
0x600C500C  idf=0x00004000 ours=0x00000003   ★ MMU[3]
```

两个 **★** 结论：

1. **MMU 表项**：IDF app 里未使用的表项是 `0x4000`（bit14=valid + 页 0）；
   我们被 bootloader 映射的表项是 `2`/`3`（**没有 bit14**）。
2. **SPI 控制器**：`SPI_MEM_CLOCK` 在 IDF app 里是 `0x00030103`，我们的是 `0x80000000`
   —— **时钟配置完全不同**，这正是 cache 从 flash 读不回数据的直接嫌疑。

## 下一步

把这两个寄存器按 IDF 的值写回去（尤其 `SPI_MEM_CLOCK`），确认写入是否生效
（回读 + 信标），然后再做数据读/取指测试。

## 用参照 app 做的两个决定性实验（真板）

### 实验一：让它去读我们那一页（0x42030000）

```c
printf("IDFREF n=%d own_irom=%08x page3=%08x ...", ..., *(uint32_t*)0x42030000u, ...);
```

真板输出：

```
I (166) cpu_start: cpu freq: 160000000 Hz
Guru Meditation Error: Core / panic'ed (Cache error)
```

**两个重要信息：**

1. **IDF app 运行在 160 MHz（PLL）** —— 我们跑的是 40 MHz XTAL。
2. **读未映射的 IROM 页时，IDF app 会触发 `Cache error` 并 panic；而我们的固件是
   "安静地返回 0"** ✗ —— 这个差别很关键：说明我们的读**根本没有走到 cache 的错误路径**，
   要么 cache 没被真正查询，要么错误被吞掉了。正常的行为应该是抛 Cache error。

### 实验二：按 IDF 的运行态值修 SPI 控制器（我们的固件里）

```
写 0x60002014 = 0x00030103  -> 回读 0x00030103  ✓ 写入生效
写 0x60002008 = 0x00608000
MMU[3] = 0x4003（带 valid 位）-> 回读 0x00004003 ✓ 写入生效
关双 cache -> 修上面这些 -> 开双 cache -> 数据读 0x42030000
```

结果：**仍然读回 0，而且不报 Cache error** ✗。

⇒ 就是说：寄存器值可以和 IDF app 完全相同、写入也确实生效，但我们的访问依旧静默返回 0。
这个"静默"本身就是最有价值的线索——**正常路径应该抛 Cache error**。

## 逐页探测：整个 IROM 窗口对我们的 app 都不响应

```c
s3_beacon(1u, *(volatile u32 *)0x42000000u);   /* 页0 -> flash 0x20000（我们的镜像头）*/
s3_beacon(2u, *(volatile u32 *)0x42010000u);   /* 页1 */
s3_beacon(3u, *(volatile u32 *)0x42030000u);   /* 页3 -> flash 0x30000 */
```

真板信标：**三个页全部读回 0**。

⇒ 不是"某一页没映射"，而是**整个 IROM 窗口对我们的 app 都不响应**，
而且不抛 Cache error（正常路径应该抛）。

这与 TRM §15.5 那句"**SPI1、GDMA 或 CACHE 如需访问外部存储器，必须首先获得相应的
访问权限**……未授权则访问不被响应"的描述高度一致——但 `SYSCON_FLASH_ACEn_*`
实测是全 0xFF（完全开放），所以问题可能在更上层的 **World 控制（TRM §16 WCL）**
或 cache 通路本身。

## 新线索：时钟树（RTC_CNTL）差异巨大

对比两个 app 运行时的 `0x60008000 ~ 0x600080FF`（RTC_CNTL 时钟区，共 64 个寄存器），
**26 个不同**，其中几个很关键：

```
0x60008000 RTC_CNTL_OPTIONS0  idf=0x1c008002  ours=0x1c00a000   <- PLL 相关位不同
0x60008010 RTC_CNTL_CLK_CONF  idf=0x0009e249  ours=0x00000000   <- 我们这边整个是 0
0x6000801C                    idf=0x14140143  ours=0x28140143
0x60008054                    idf=0x003be033  ours=0x00000000
...
```

**这个发现的意义**：SPI flash 控制器的时钟是从 PLL/APB 派生的。如果时钟树没建立起来，
SPI 控制器就拿不到有效时钟 —— 这正好能解释：

- 为什么 `SPI_MEM_CLOCK(0x60002014)` 在 IDF app 里是合理的分频值 `0x00030103`，
  在我们这边却是 `0x80000000`
- 为什么我们的 IROM 访问**静默返回 0**（时钟不工作 → 读请求得不到响应 → 也不会报 Cache error）

而 SYSCON 那几个时钟寄存器（0x60026000 等）两边完全一致，说明差异确实在 RTC_CNTL/PLL 这一层。

### 试过的粗暴办法（失败）

把 IDF app 那 26 个 RTC 寄存器值原样写进我们的 app —— 结果我们的 app 直接起不来
（信标还是 bootloader 的残留值），说明直接写 RTC 时钟寄存器有副作用（可能触发了复位）。
**下次要用正规做法**：按 IDF 的 `rtc_clk_cpu_freq_set_config()` 流程
（含 LDO slave 配置、REGI2C 的 dbias 调整），而不是快照移植。

## 干净的对照（重要纠正）

之前那份"IDF app 寄存器"是在**带 panic 探测代码**的版本上 dump 的，结果不可靠。
重建干净的 `IDFREF alive` 版本再 dump，得到的结论**和之前一致**（只有 9 处差异），
但有一件事需要纠正：

```
MMU 表项 0x600C5000..0x600C503C 在健康的 IDF app 里全部是 0x00004000
我们的：MMU[0] = 0x00000002, MMU[3] = 0x00000003
```

IDF app 的 IROM 段 vaddr 是 `0x42000020`（对应表项 0），它的表项却是 `0x4000`。
如果 `0x4000` 表示"valid + flash 页 0"，那它的代码应该来自 flash 0x0 —— 但它明明在
flash 0x20000 且运行正常。

⇒ **说明我对 S3 MMU 表项格式/索引的理解还是不对**（很可能 DROM/IROM 用不同的表区间，
或者还有别的基址），所以"我们的表项缺 valid 位"这个结论**不能成立**，之前基于它的
推理需要推翻。

## 目前真正站得住的差异（9 处，其中 5 处是计数器/同步地址噪声）

```
0x60002008  SPI_MEM_CTRL   idf=0x00608000 ours=0x016ca008
0x60002014  SPI_MEM_CLOCK  idf=0x00030103 ours=0x80000000
0x600C5000  MMU[0]         idf=0x00004000 ours=0x00000002
0x600C500C  MMU[3]         idf=0x00004000 ours=0x00000003
```

注意：`SPI_MEM_CLOCK=0x80000000` 是**bootloader 留下的正常状态**（IDF app 在启动早期
也是这个值，那时它已经能执行 IROM 了），所以这个值本身也不是问题。它后来被 app 自己的
flash 初始化改成 `0x00030103`。

### 结论

在"寄存器状态"这个层面，我们已经找不到能解释差异的东西了。剩下最靠谱的做法是：
**在 IDF app 里单步跟踪它的 `spi_flash_init_chip_state()` / `esp_mmu_map()`，
把我们缺的那一步找出来**；或者把这些对照实验整理成报告找 Espressif 支持。

## 重大纠正：MMU 表项的 bit14 是 **INVALID**，不是 valid

IDF 的定义（`soc/esp32s3/include/soc/ext_mem_defs.h`）：

```c
#define SOC_MMU_INVALID   BIT(14)   // bit14 = 1 表示【无效】
#define SOC_MMU_VALID     0         // bit14 = 0 表示【有效】
#define SOC_MMU_TYPE      BIT(15)   // 类型位
```

⇒ 完全理解反了：

- IDF app 里那些 `0x4000` 表项 = **bit14 置位 = 无效/未映射**（正常的默认值）
- 我们被 bootloader 映射的表项 `MMU[0]=2`、`MMU[3]=3`（bit14 清零）= **有效**
  ⇒ 说明 bootloader 给我们的 MMU 映射**本来就是对的**

而我在前面几轮实验里"补 valid 位"写的 `0x4003`，其实是**把表项标成无效** ✗，
等于自己破坏了本来正确的映射。之前基于"表项缺 valid 位"的所有推理都要推翻。

### 正确的理解

```
vaddr 0x42030000 -> entry 3，值 0x00000003 = 有效 + flash 页 3 = 0x30000 ✓ 完全正确
```

也就是说：**MMU 一直是对的**，flash 内容也是对的（已 read-flash + cmp 校验），
但 cache 从 flash 读数据依旧返回 0。问题不在 MMU。
