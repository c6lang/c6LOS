# Cache 错误的可观测性（子代理研究结论 + 真板实测）

## 两个必须先纠正的方法论错误

### ① `excvaddr=0` 不是证据

Xtensa 的 `EXCCAUSE=0`（IllegalInstruction）**根本不写 `EXCVADDR`**，那个 0 是无效值。
唯一有效信息是"取回的字是非法指令"（`0x00000000` / `0xFFFFFFFF` 都非法）。

### ② "不抛 Cache error" 不能推出 "访问没走到 cache"

IDF 源码注释（`components/esp_system/port/soc/esp32s3/cache_err_int.c:11-15`）：

> The cache has an interrupt that can be raised as soon as an access to a cached region
> is done **without the cache being enabled**. We use that here to panic the CPU, which
> from a debugging perspective, is better than **grabbing bad data from the bus**.

而这个中断**复位默认是关的**：

```
EXTMEM_CACHE_ILG_INT_ENA        (0x600C40DC)  default: 全 0
EXTMEM_CORE0_ACS_CACHE_INT_ENA  (0x600C40E8)  default: 全 0
```

IDF app 会 panic 只是因为它调了 `esp_cache_err_int_init()` **装上了报警器**。
**我们看到的"静默"= 错误被静默 + 没人装报警器，不等于没出错。**

### TRM §15.6 原文

> 非法访问 → **过滤该访问请求，并返回默认值（取指/读返回 0 或 `0xdeadbeaf`）**……
> 触发相应中断（**如相应中断已使能**）。

⇒ 我们读到的 `0x00000000` 与"被过滤后返回默认值"完全一致。

## 一个被忽略的关键旧数据

项目早期记录过（`docs/wifi-plan.md` §53 附近）：

```
cache 关 + 总线 SHUT 时读 0x42000000  ->  0xbad00bad    （总线没接上，毒值）
清 SHUT + 开 cache 后再读            ->  0x00000000    （访问进了 cache，被过滤后返回 0）
```

后半段正是"进了 cache 但没读到有效数据"。

## 真板实测（本轮）

最小用例：只读一次 IROM，然后读状态寄存器：

```
slot0 = 0xB0000042   进 os_main
slot1 = 0x00000000   IROM 读结果
slot2 = 0x00000000   EXTMEM_CACHE_ILG_INT_ST      <- 没有任何非法访问记录
slot3 = 0x00000000   EXTMEM_MMU_FAULT_VADDR       <- 没有 MMU 表项错误
```

⇒ **既不非法、映射也没错，就是"合法地返回 0"。**

## 建议的下一步（按优先级）

1. **装报警器**：`0x600C40DC |= 0x3F`、`0x600C40E8 |= 0x1F`，并把
   `ETS_CACHE_IA_INTR_SOURCE` / `ETS_CACHE_CORE0_ACS_INTR_SOURCE` 路由到 trap，
   这样失败会变成可观测的。
2. 读 `EXTMEM_CACHE_STATE (0x600C4130)` 看 cache FSM 是否卡在非 idle。
3. 读 `SYSTEM_CACHE_CONTROL_REG (0x600C0048)`（期望 0x5）、
   `EXTMEM_CLOCK_GATE (0x600C4164)`、`EXTMEM_CACHE_MMU_POWER_CTRL (0x600C412C)`。
4. **用 SPI_MEM 的 USER 通路手工读 flash**（`SPI_MEM_USER` 置 `USR_MISO`，命令 `0x03`
   + 24 位地址，轮询 `CMD.USR` 自清，读 `SPI_MEM_W0`），与 cache 读对比 ——
   这是唯一能把"flash 通路"和"cache/MMU 通路"分开的实验。

## 两条已被证伪的"原厂魔数"

`port/s3/flash.c` 里这两行是从"JTAG 不可靠读数"抄来的，**都是复位默认值，写了等于没写**：

```c
*(volatile u32 *)0x600C4068u = 0x5u;   /* EXTMEM_ICACHE_TAG_POWER_CTRL 复位值就是 0x5 */
*(volatile u32 *)0x600C407Cu = 0x4u;   /* bit2 是只读的 LOCK_DONE，复位就是 1 */
```

## `SPI_MEM_CLOCK = 0x80000000` 不是问题

解出来是 `CLK_EQU_SYSCLK=1`（SPI 时钟 = CPU 40MHz XTAL），是自洽配置；
IDF app 在 160MHz PLL 下用 `0x00010001`（÷2 = 80MHz），也自洽。
**别再在运行期写 `0x60002008/0x60002014`** —— 从 flash 取指时改自己的读时钟就是当场自杀，
"写就崩"是这个原因，不是"配置缺失"的证据。

## 又排除两条（真板实测）

### H7：cache 的时钟/电源/MMU 内存供电全部正常

```
SYSTEM_CACHE_CONTROL_REG (0x600C0048) = 0x00000005   ← ICACHE_CLK_ON + DCACHE_CLK_ON 都开
EXTMEM_CLOCK_GATE        (0x600C4164) = 0x00000001   ← 时钟门控打开
EXTMEM_CACHE_MMU_POWER_CTRL (0x600C412C) = 0x00000005 ← MMU 内存已上电
```

### H5：IROM 在 512 项表里的范围是完整的

```
Cache_Get_IROM_MMU_End() = 0x200 (=512)   ← IROM 拥有整张表
Cache_Get_DROM_MMU_End() = 0x400 (=1024)
```

我们的 entry 3 落在 IROM 范围内，切分点没有问题。

### 小结

至此，cache 之前/之后的每一层都验证过是正常的：
时钟 ✓ 电源 ✓ MMU 供电 ✓ 表项有效 ✓ cache 使能与模式 ✓ 总线未 SHUT ✓
flash 内容 ✓ 无非法访问记录 ✓ 无 MMU 错误记录 ✓ —— **读回仍是 0**。

唯一还没做的是 **H8**：用 SPI_MEM 的 USER 通路（不走 cache）手工读 flash，
把"flash 通路"和"cache 通路"彻底分开。这是下一步最有价值的实验。

## H8 实测：SPI 控制器本身是活的（能完成 flash 读命令）

绕过 cache，用 `SPI_MEM` 的 USER 通路手工发一条 `0x03` 读命令（目标 flash 0x30000）：

```c
os_reg_wr(0x60002018u, (1u<<31)|(1u<<30)|(1u<<28));  /* USER: COMMAND|ADDR|MISO */
os_reg_wr(0x6000201Cu, 24u << 26);                   /* ADDR_BITLEN = 24 */
os_reg_wr(0x60002020u, (8u<<28)|32u|0x03u);          /* CMD_BITLEN=8, CMD=0x03, MISO=32bit */
os_reg_wr(0x60002004u, 0x00030000u);                 /* ADDR */
os_reg_wr(0x60002000u, 1u<<18);                      /* CMD.USR = 1 触发 */
while (cmd & (1u<<18)) ;                             /* 轮询完成 */
```

真板结果：

```
slot0 = 0xB0000060   进 os_main
slot1 = 0x00000001   轮询 1 次就完成   <- SPI 控制器正常工作！
slot2 = 0x00000010   W0 读回的值（数据相位/位序还需调整，不是期望的字节）
slot3 = 0x00000000   CMD.USR 已清零
```

⇒ **SPI 控制器能正常执行并完成 flash 读命令**，说明 flash 通路是活的。
读回的具体字节不对是 MISO 位序/相位配置问题（另一件事），不影响这个结论。

**因此问题确实是 cache 侧特有的**：SPI 能读、bootloader 能读、`esptool` 能读，
唯独 cache 从 flash 取数返回 0。

## 时钟源也排除了：我们其实就跑在 PLL 上

`SYSTEM_SYSCLK_CONF` 的真实地址是 **0x600C0060**（不是 0x60026000）：

```
0x600C0060 = 0x000A8400   -> SOC_CLK_SEL(bits 11:10) = 1 = PLL_CLK  ✓
0x600C0010 = 0x0000000C   -> CPU_PER_CONF
```

⇒ **我们的 app 本来就在 PLL 时钟源下**，和 IDF app 一样。
"两边 CPU 频率不同（40MHz XTAL vs 160MHz PLL）"这条最后也排除了。

## E1/E2 实测：计数器无效，该结论**已撤回**

按子代理 E1 的方法，清计数器后做两次 IROM 读，再读差值，得到全 0：

```
DBUS_ACS_CNT 增量 = 0 / DBUS_FLASH_MISS 增量 = 0 / ICACHE_FREEZE = 0
```

**但做了对照实验后发现这个判读是错的**：在**能正常读 IROM** 的 IDF app 里做同样的事：

```
CNT irom=0020c012 dbus_delta=0 miss_delta=0 freeze=00000000
```

IDF app 读到了真实数据（`0x0020c012`），**计数器增量同样为 0**
⇒ **这两个计数器在我们需要的场景下根本不计数**（需要额外的使能/配置），
所以"计数器不动"**不能**推出"请求没进 cache"。**该结论撤回。**

仍然成立的只有一条：两边 `EXTMEM_ICACHE_FREEZE = 0`
⇒ "假命中（FREEZE_MODE）"这个假设被排除（不是它的原因）。


## E3 反向对照（在能工作的 IDF app 里装我们的条件）

| 装上的条件 | IDF app 读 IROM 的结果 |
|---|---|
| `SPI_MEM_CLOCK = 0x80000000`（我们固件的值） | `irom=0x00f01d12` **照常** ⇒ 排除 |
| `rsil 15`（INTLEVEL=15，和 `_start` 一样） | `irom=0x0020c012` **照常** ⇒ 排除 |
| 关三个看门狗 + `rsil 15` 一起 | 应用挂住（`vTaskDelay`/printf 在高 INTLEVEL 下不可用），无法判读 |

⇒ 我们在 `_start` 里做的这些动作（关看门狗、INTLEVEL=15）**都不会让 IROM 读失效**。

**结合前面所有实验，差异已经不在"运行时我们能改的东西"里** ——
既不在我们自己代码的行为里，也不在那些事后可写的寄存器里。
剩下的只能是**镜像/引导路径本身**造成的某种一次性状态。

## 又排除一条：镜像烧录偏移不相关

把我们的 app 挪到 **0x10000**（和 IDF app 完全相同的偏移，配对应分区表，镜像用
`mkimg.py ... 0x10000` 重新生成以保持段偏移关系）：

```
c6os s3 boot
spiclk=0x80000000     <- 仍然是我们的值
irom3=0x00000000      <- 仍然读不回来
```

⇒ 烧录偏移不是差别来源。同时注意：即使在同一偏移，bootloader 给 IDF 镜像配的是
`0x00010001`、给我们的仍是 `0x80000000` —— 差别确实在**镜像内容**里。

## SPI0 的 `CACHE_FCTRL.CACHE_REQ_EN` 是开着的（排除）

之前只读了 `0x6000203C`（SPI1 的镜像，值为 0），漏了 SPI0 的那份：

```
0x6000303C (SPI0 CACHE_FCTRL) = 0x00000005   -> bit0 CACHE_REQ_EN = 1（已使能）
0x6000203C (SPI1 CACHE_FCTRL) = 0x00000000
```

⇒ cache 请求转发到 SPI0 的总闸是开的，这条也排除。
（`REG_SPI_MEM_BASE(i) = DR_REG_SPI0_BASE - i*0x1000`，所以 `0x6000303C` 才是 SPI0 的那份。）

## ★ 关键判别：**DROM 窗口能读，IROM 窗口不能**

```c
*(volatile u32 *)0x3C000020u   /* DROM，我们的 .flash.rodata（app_desc 魔数） */
*(volatile u32 *)0x42030000u   /* IROM，我们的 .flash.text */
```

真板结果：

```
slot1 = 0xABCD5432   <- DROM 读成功了！读到的正是 app_desc 魔数（映射 entry0 -> flash 页 2）
slot2 = 0x00000002   <- MMU[0] = 2，映射正常
slot3 = 0x00000000   <- IROM 仍然是 0
```

### 这条的意义

**cache 从 flash 读数据这件事本身是通的**（DROM 窗口证明了：MMU、SPI、DCache、
整个数据通路都工作正常）。坏掉的**只有 IROM 窗口 / I-Cache 这一侧**。

这就把范围从"cache 完全不能用"收窄到"**I-Cache 无法从 flash 取数**"，
和"取指得到非法指令"这个原始症状完全对得上。

注意：我们的代码全部跑在 IRAM，**I-Cache 从来没有被真正用过**，
所以它坏掉不会影响其它功能——这和观察完全一致。
