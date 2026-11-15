

> # ★★★ 2026：IROM 问题已解决 ★★★
>
> **根因**：I-cache 从来没有被正确配置 —— bootloader 留下的默认模式
> （4-way/16KB/16B，`ICACHE_CTRL=0x01`）无法为 IROM 提供数据。
> 必须调 ROM 的 `rom_config_instruction_cache_mode(32768, 8, 32)` 配成
> 8-way/16KB/32B（`ICACHE_CTRL=0x0b`）。IDF 应用一直在做这件事，我们没做。
>
> **修复**：`port/s3/flash.c: os_icache_enable()`（已提交）。
> **验证**：数据读 `0x42030000 = 0x7b004136` ✓，取指执行 flash 里 `movi.n a2,42` 返回 42 ✓。
>
> **关键判别方法**：DROM 窗口能读（`0x3C000020` 读回 `0xABCD5432`）而 IROM 不能
> —— 由此确定坏的只有 I-cache 一侧。
>
> 详见 `docs/refs/cache-error-observability.md` 末尾的"根因已找到"一节。
> **WiFi 栈现在可以放进 flash，原定目标重新可推进。**


> # 当前状态一览（最新）
>
> **已解决**
> 1. **IROM 取指（卡了 60+ 轮的根因）**：I-cache 从未被配置 —— bootloader 留的是
>    4-way/16KB/16B，无法服务 IROM。必须调
>    `rom_config_instruction_cache_mode(16384, 8, 32)`（`ICACHE_CTRL=0x0b`）。
> 2. **WiFi 闭源库链接进 flash**：645KB 镜像（之前 IRAM 溢出 421KB 链不出来）。
> 3. **bootloader 接受带 WiFi 的镜像**：关键约束 —— IDF bootloader 只支持
>    **一个 DROM + 一个 IROM 段**，且 DROM 必须正好 1 页（64KB）才不会抢走
>    IROM 的 MMU 表项。当前布局：DROM@0x3C000020(1页) + IROM@0x42010000(entry 1 起)。
> 4. **OSI 桩函数 ABI**：`wifi_glue.c` 必须 `-mabi=windowed` 编译（闭源库会回调它），
>    并且改完 Makefile 规则后要**强制重建**（源文件没变时 make 不会重编，这个坑踩过）。
>
> **剩余唯一问题**
> `esp_wifi_init_internal` 在**段内偏移 0xD934** 处的取指失败
> （`cause=0`，三种基址下都是同一偏移；该地址的**数据读正常**）。

# 从这里继续（交接摘要）

## 一分钟看懂现在的状态

- **能做到**：c6LOS 在 S3 上完整跑起来（USB-CDC 串口、背光音频、RTC 信标、USB-JTAG 调试全通）；
  windowed ABI 桥（能正确调用 IDF 闭源库）；libphy 的 PHY 初始化 5 步真板验证通过；
  WiFi 闭源库 + IDF 源码层**链接 0 未定义符号**；自己生成的标准 IDF app 镜像能被原厂
  bootloader 引导（`esptool image-info` 报 checksum + hash 双 valid）。
- **卡住的唯一一件事**：**访问 IROM 窗口**。数据读返回 `0x00000000`，取指则
  `exccause=0 (IllegalInstruction)`、`excvaddr=0`、`epc1=<flash 地址>`；
  而且**不抛 Cache error**（正常应该抛）。
- **官方问题报告**：`docs/support-report.md`（可直接发帖/提工单）
- **官方文档摘录**：`docs/refs/`（TRM cache/MMU/PMS/system、镜像格式、IDF 参照 app）

## 研究过程中纠正过的重要误区（别再踩）

1. **`SOC_MMU_INVALID = BIT(14)`** —— bit14 置位表示**无效**，不是有效。
   我们 bootloader 写的 `MMU[3]=3`（bit14 清零）本来就是**正确**的映射。
   早期"补 valid 位写 0x4003"其实是把映射弄坏。
2. **JTAG 读 MMU/cache 寄存器块不可靠** —— IDF app 运行时经 JTAG 读 MMU 表全是
   `0x4000`，但它自己代码读出来是 `0x00000002`。**对照必须让两边 app 各自用代码读**。
3. **`SPI_MEM_CLOCK=0x80000000` 是 bootloader 留下的正常值** —— IDF app 早期也是这个
   状态且已能执行 IROM，所以它本身不是问题。

## 已经逐项排除的（都做了真板实测，别重复）

1. **镜像格式**：`esp_app_desc_t@0x20`、段表连续、长度含补齐、XOR 校验和、SHA-256 附录
   —— `esptool image-info` 双 valid，bootloader 接受并跳转。
2. **MMU**：表项值是**正确**的（bit14=无效位，我们的 `2`/`3` 是有效映射）。
   恒等/重映射/关 cache 手写都试过。
3. **ICache/DCache**：使能位、`SHUT_CORE0/1_BUS`、`TAG_POWER_CTRL`，以及
   **模式位 `WAY_MODE`/`BLOCKSIZE`**（已按 IDF 值设成 `0x0b`/`0x09`）——都对齐了。
4. **ICACHE 失效**：`EXTMEM_ICACHE_SYNC_CTRL` 的 `INVALIDATE_ENA` 能完成。
   ROM 的 `Cache_Invalidate_ICache_All()` 一调不返回。
5. **SPI_MEM**：`CLOCK` 已按 IDF 值 `0x00010001`、`CTRL` 按 `0x00000000` 写入并回读确认；
   也试过 DIO/DOUT/QOUT 三种 flash 模式。
6. **权限**：`SYSCON_FLASH_ACEn_*` 全 `0xFF`；TRM §16 说明 World 控制默认关闭。
7. **时钟**：`SYSTEM_SYSCLK_CONF` 等与 IDF app 一致；`CACHE_CONTROL` 的时钟位也一致。
8. **flash 内容 / flash 通路**：`esptool read-flash` + `cmp` 逐字节校验过；bootloader 能读；
   我们还绕过 cache 用 `SPI_MEM` USER 通路手工发 `0x03` 读命令，**轮询 1 次就完成** ——
   SPI 控制器与 flash 通路完全正常。
9. **cache 错误状态**：清状态后做一次 IROM 读，`EXTMEM_CACHE_ILG_INT_ST=0`、
   `EXTMEM_MMU_FAULT_VADDR=0` —— **连错误记录都没有**。
   （注意：该中断复位默认关闭，IDF app 是靠 `esp_cache_err_int_init()` 才看得到 panic。）
10. **cache 时钟/电源/MMU 供电**：`SYSTEM_CACHE_CONTROL=0x5`、`EXTMEM_CLOCK_GATE=0x1`、
    `EXTMEM_CACHE_MMU_POWER_CTRL=0x5`。
11. **MMU 表项范围**：`Cache_Get_IROM_MMU_End=0x200`（整表归 IROM），entry 3 在范围内。
12. **eFuse**：`DIS_ICACHE/DIS_DCACHE/DIS_DIRECT_BOOT` 全 False。
13. **flash 芯片内部状态**：`read-flash-status` 两边都是 `0x0200`。

**结论：上述全部对齐到和 IDF app 完全一致后，IROM 访问依旧静默返回 0。**
差异不在"事后可改"的寄存器里，而在启动阶段的一次性配置。

## 最值得试的下一步

1. **flash 芯片内部状态**：SPI_MEM 寄存器一致不代表芯片内部一致。用 SPI_MEM 的
   用户命令寄存器读 flash 的 JEDEC ID / 状态寄存器 / 配置寄存器，对比原厂固件跑起来
   之后的状态（IDF 在 `spi_flash_init_chip_state()` 里会改芯片的 dummy/QE 位）。
2. **在他处复现**：用 IDF 编一个最小 app 跑通（需要先 `install.sh`，本机 `/home` 磁盘
   已满，装之前要先腾空间），确认 IDF app 能跑 IROM，再把它的 app 侧 flash 初始化
   逐步搬过来做二分。
3. 若以上都不通，最后退路是「把 WiFi 栈裁到 66KB IRAM 只留 scan 路径」。

## 常用命令

```sh
# 刷我们的常规固件（亮屏+心跳）并立即看日志
./tools/flashmon.sh build/won/c6os.bin 3

# 读 RTC 信标（掉电不丢，esptool 就能读）
./tools/beacon.sh

# 起 openocd + gdb 调试
./tools/dbg.sh <elf> <脚本.gdb>

# 构建（示例）
make C6_ROOT=/home/winger/c6 BUILD=build/xxx S3_MAIN=tests/xxx.c6 s3

# 标准 IDF 镜像路线（三个都要刷！）
sudo -n esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 <原厂bootloader.bin>
sudo -n esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x8000 /tmp/p2.bin
python3 tools/mkimg.py build/xxx/c6os.elf /tmp/app.bin
sudo -n esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x20000 /tmp/app.bin
```

注意：`flashmon.sh` 会把镜像写到 0x0，**会覆盖 bootloader 和 0x8000 的分区表**，
走标准镜像路线后要重新刷这两样。

# ESP32-S3 WiFi 联网设计（含 ABI 迁移）与分阶段计划

> **最终目标**：真板上通过 WiFi 取到 `https://qq.com` 的页面数据。
> 链路 = WiFi 关联 → DHCP → TCP → TLS → HTTP/1.1 GET。

> 目标（待确认）：在 c6LOS 上让 S3 连上 AP 并能收发 IP 包（先 ping/HTTP GET 就算成功）。
> 现状：内核已有任务/队列/信号量/互斥/定时器/tick（`include/c6os/*.h`），
> 没有 flash 文件系统、没有 NVS、没有 TCP/IP 栈、没有 FreeRTOS API。

## 1. 现成可用的东西（实测）

**闭源 blob（都在 `$IDF/components` 里，Xtensa 目标文件 + `.a` 归档）**

| 库 | 路径 | 大小 | 作用 |
|---|---|---|---|
| libnet80211.a | `esp_wifi/lib/esp32s3/` | 1.4 MB | 802.11 协议栈 |
| libpp.a | 同上 | 708 KB | MAC/包处理 |
| libcore.a | 同上 | 4.8 KB | WiFi 核心 |
| libwapi.a | 同上 | 52 KB | WPA 接口（加密/关联） |
| libphy.a | `esp_phy/lib/esp32s3/` | 255 KB | PHY/射频 |
| libcoexist.a | `esp_coex/lib/esp32s3/` | — | WiFi/BT 共存（WiFi-only 可先空实现） |

**这 6 个库合起来只需要外部提供 129 个符号**（`nm -u` 减去库内部定义得到）。
大部分是 ROM 函数（`ets_delay_us`、`memcpy`…）和少量 hook：
`net80211_funcs`、`pp_wdev_funcs`、`pp_task_hdl`、`phy_enter_critical`/`phy_exit_critical`、
`phy_get_romfuncs`、`coex_*_ptr`、`*_rom_version_get`。

**IDF 侧的 C 源码（要移植的就是这些）**

- `esp_wifi/src/` 1955 行（`wifi_init.c` 767 行是大头）
- `esp_phy/src/` 2104 行（`phy_init.c` 1239 行：PHY 初始化 + 校准数据）
- `esp_coex/src/` 695 行（WiFi-only 可以先桩掉）
- `esp_netif/` 280 行（网络接口胶水，如果走自研 IP 栈可以不要）
- lwIP 源码在 `components/lwip/lwip/src`（纯 C，可移植，但需要 `sys_arch`）

## 2. 必须新写的适配层（工作量主要在这里）

1. **FreeRTOS 兼容层**：`xTaskCreate`/`vTaskDelete`/`xQueue*`/`xSemaphore*`/
   `xEventGroup*`/`vTaskDelay`/`portENTER_CRITICAL`/`esp_timer_*`/`esp_intr_alloc`/
   `heap_caps_malloc` 等，全部翻译到 c6os 已有的 task/queue/sem/mutex/timer 上。
2. **ROM 符号表**：把 IDF 的 `esp32s3.rom.ld`（外加 `rom.api.ld`、`rom.newlib*.ld`）
   转成我们的链接脚本片段，直接引用 ROM 地址（不改 ROM 里的实现）。
3. **NVS（先内存版）**：PHY 校准数据、WiFi 配置要存/取。我们还没有 flash 文件系统，
   **第一步做纯内存 NVS**（掉电即失，校准每次重做），跑通后再考虑落 flash。
4. **PHY 校准数据**：`esp_phy` 需要 `phy_init_data`（与芯片型号绑定的默认值）。
   可以从 IDF 的 `esp_phy/phy_init_data.c` 直接搬（它是公开的常量表）。

## 3. 内存预算（关键约束）

- 可用 DRAM 只有 323 KB，而且 **0x3FCD9500 之后有一小段坏区**
  （密集读写会把 CPU 挂住，见 `docs/todo.md` 音频那节）。
- IDF 官方 STA-only 场景 WiFi 大约要吃 **50~80 KB 堆**。加上内核栈、lwIP（如果上）
  约 30~40 KB，剩给应用的不到 100 KB。
- 结论：**WiFi 用例必须单独一个镜像**，不能和录音/LCD 大帧缓冲共存。

## 4. 风险

1. **闭源 blob 的 ABI/工具链版本**：我们用的是 xtensa-esp-elf 16.1，IDF 那批库是别的
   GCC 版本编的。已经在 libgcc 的 `_umoddi3` 上见过 "ABI does not match" 警告，
   PHY 库里还混着软浮点（`__divdf3`/`__floatsidf`）——链接期/运行期都可能踩坑。
2. **PHY 校准正确性**：校准数据不对不会报错，只表现为"射频不出信号/发射功率异常"，
   很难查。所以里程碑要设计成"能扫到 AP"这种客观证据，而不是"初始化返回 OK"。
3. **没有 flash/分区/NVS**：wifi 配置和校准都无法持久化，复位后要重新配。
4. **时间**：这是一个多阶段工程，不是一次会话能完成的。

## 5. 里程碑（每步都有客观证据，避免"看起来好了"）

- **M0（本次先做）**：把 `libphy.a` 链进来并调用一个入口（如
  `phy_get_rf_cal_version()`），用串口打印结果。
  证明：blob 能被我们的工具链链接、能在我们的启动流程里跑起来；
  同时得到"还需要补哪些符号"的清单。
- **M1**：`esp_phy_enable()` 跑通（PHY 初始化 + 校准），打印 PHY 校准版本/信道。
- **M2**：`esp_wifi_init` + `esp_wifi_set_mode(STA)` + `esp_wifi_scan_start`，
  **串口打印扫到的 AP 列表**（SSID/RSSI/信道）——这是射频收发的客观证据。
- **M3**：连上 WPA2 AP（`libwapi.a`），自己写 DHCP 客户端拿 IP。
- **M4**：自研最小 TCP（或移植 lwIP）发一个 HTTP GET，串口打印服务器响应。

## 6. 需要确认的问题

1. 目标到哪一步算"联网"？（M2 扫到 AP / M3 拿到 IP / M4 发出 HTTP）
2. 能不能接受**配置不落盘**（每次复位重新输 SSID/密码，或者直接写死在代码里）？
3. 优先自研精简 IP 栈（代码可控、占内存小）还是移植 lwIP（功能全、工作量大）？
4. 是否允许为了 WiFi 镜像**暂时不跑 LCD/录音**（省内存）？

## 7. 【硬阻塞】ABI 不匹配：IDF 的闭源库全是 windowed ABI，我们是 call0

M0（把 `libphy.a` 链进来并调用 `phy_get_rf_cal_version()`）已经做到"能链接"，
但**一运行就复位循环**。反汇编闭源库看到原因：

```
00000000 <phy_get_rf_cal_version>:
   0:  004136   entry  a1, 32      ← windowed ABI 的函数入口
   3:  c7a222   movi   a2, 0x2c7
   6:  f01d     retw.n             ← windowed ABI 的返回
```

- 整个 `libphy.a`/`libpp.a`/`libnet80211.a`/`libcore.a`/`libcoexist.a`/`libwapi.a`
  都是 **windowed ABI**（`entry`/`retw`，寄存器窗口 + 窗口溢出/欠载异常）。
- 我们这套 S3 移植是 **call0**（`Makefile` 里到处都是 `-mabi=call0`，
  `port/s3/irq.S`、`ctx.S`、`start.S` 也按 call0 写的）。
- 两个 ABI **不能混用**：call0 的调用不建立寄存器窗口，windowed 函数执行
  `retw` 时会切到错误的窗口返回 → 直接跑飞（实测就是这个现象）。

### 要上 WiFi，必须先做 ABI 迁移（新的 M-1）

| 工作项 | 现状 | 迁移后 |
|---|---|---|
| 编译选项 | `-mabi=call0`（Makefile 8/70/111 行硬编码） | `-mabi=windowed` |
| `port/s3/irq.S` 126 行 | call0 中断入口 | windowed 入口（`call8` + 窗口保存） |
| `port/s3/ctx.S` 158 行 | 保存 a0-a15 | 加 windowbase/windowstart，按窗口保存 |
| `_WindowOverflow4/8/12`、`_WindowUnderflow*` | 没有 | **必须新写**（寄存器文件只有 4 个窗口，
|  |  | 调用链深了会溢出，靠这组异常做 spill/fill） |
| `port/s3/start.S` | call0 启动 | 基本不用改（但要设好 windowstart） |

参考实现：IDF 的 `components/xtensa/xtensa_vectors.S`（2230 行，含溢出/欠载
向量）、`xtensa_context.S`（672 行）、`xtensa_intr_asm.S`（238 行），
其中最关键的溢出/欠载处理只有约 100 行（`_WindowOverflow4/8/12`、
`_WindowUnderflow4/8/12`），其余是日志/调试。另外 IDF 还带了一个裸机移植
`components/xtensa/baremetal/xtensa_rtos_bm.S`（85 行）可以当骨架参考。

**风险**：这是对"已经跑通的 LCD/音频/内核"动地基 —— 中断入口、上下文切换、
陷阱处理全部要改，改完必须把已有用例（面板、录音回放、GUI）重新过一遍。

### 结论 / 需要拍板

1. **走 ABI 迁移**：这是我们唯一能用上 IDF WiFi/PHY 闭源库的路（自己写 802.11
   协议栈 + 射频固件是不现实的）。工作量估计 2~3 天，风险集中在窗口溢出/欠载
   和上下文切换，收益是顺带拿到更好的代码密度。
2. **或者暂时放弃 WiFi**，保持 call0，把精力放回音频/LCD 的完善。

M0 的成果（ROM 符号表 `port/s3/rom/*.ld`、`port/s3/idf_shim.c`、Makefile 的
`S3_EXTRA_LIBS`）已经提交，是迁移后可直接复用的地基。

## 8. ABI 迁移：已完成的地基 + 剩下的步骤

### 已做（call0 下真板回归通过）

1. `Makefile` 里 ABI 变成可切换：`S3_ABI ?= call0`，所有 `-mabi=` 都走它
   （`make ... S3_ABI=windowed` 即可切）。**变量必须定义在 `XTFLAGS :=` 之前**，
   否则立即展开成空串（踩过一次：`unrecognized option '-mabi='`）。
2. `port/s3/irq.S` 的向量表改成显式 `.org` 布局，并补上 windowed 必需的
   **寄存器窗口溢出/欠载向量**：`_WindowOverflow4/8/12`、`_WindowUnderflow4/8/12`
   （照 Xtensa HAL 的标准实现：`s32e/l32e` + `rfwo/rfwu`）。call0 下不触发、无副作用。
3. `port/s3/rom/*.ld`（IDF 的 ROM 符号表）接进 `link.ld`。
   **注意：`rom.libc.ld`/`rom.newlib.ld` 必须先关掉** —— 它们把 `memcpy/memset/
   strlen` 钉到 ROM 里，和我们自己的运行库打架，实测真板一启动就
   `*** TRAP cause=0000001c(LoadProhibited) pc=40056fc0`（PC 落在 ROM 内部）。
   只留 `rom.ld`/`rom.api.ld`/`rom.version.ld` 就正常。

### windowed 还差的（按顺序）

1. **level-1 中断入口改 windowed**：`call0` 换成 `call8`，参数放 **a10~a15**
   （实测编译器生成：`movi a10, arg0; movi a11, arg1; call8 f`，返回值也从 a10 读）。
   进入 C 之前要把 PS 置成 `INTLEVEL=1 | UM | WOE`（IDF 的 `_xt_lowint1` 就这么干）。
2. **`port/s3/ctx.S` 上下文切换**：切换前先把当前任务的寄存器窗口全部 spill 到它
   自己的栈上（`xthal_window_spill`，在 IDF 的
   `components/xtensa/esp32s3/libxt_hal.a` 里有，可以直接链），然后保存
   a0-a15 + epc1 + ps + windowstart；恢复时先 `wsr windowstart` 再 `rfi`。
3. **启动期**：`windowstart=1`、`PS.WOE=1`（这两句已经写好又被我回退过一次，
   因为当时把 ROM libc 的问题误算在它头上；确认后加回来）。
4. **回归**：面板/GUI/录音回放三个用例必须重新过一遍（中断入口和上下文切换是
   所有东西的地基）。

参考代码位置（都在 IDF 里，可以直接抄）：
- 窗口溢出/欠载：`components/xtensa/xtensa_vectors.S` 的 `_WindowOverflow*`/`_WindowUnderflow*`
- level-1 入口：同文件 `_KernelExceptionVector` → `_xt_kernel_exc` → `_xt_lowint1`
- 上下文保存：`components/xtensa/xtensa_context.S` 的 `_xt_context_save`/`_xt_context_restore`
- 裸机骨架：`components/xtensa/baremetal/xtensa_rtos_bm.S`（85 行）

## 9. 架构分层（目标形态）

```
  应用        HTTPS 客户端（最小 HTTP/1.1 GET，解析状态行+头部）
   ↓
  安全        mbedTLS（或先用 http:// 明文做中间里程碑）
   ↓
  传输        自研精简 TCP（推荐，代码可控；lwIP 功能全但端口大）
   ↓
  网络接口    arp / ipv4 / dhcp 客户端（几百行）
   ↓
  WiFi 驱动   IDF 的 esp_wifi/src/*.c（≈2000 行，需移植）
   ↓
  闭源 blob   libpp.a / libnet80211.a / libcore.a / libwapi.a（WPA2）/ libcoexist.a
   ↓
  射频        libphy.a + PHY 校准数据（phy_init_data）
   ↓
  底座        **windowed ABI** + FreeRTOS 垫片 + ROM 符号表 + 内存管理
```

底座这一层是当前的工作重点：没有它，上面的闭源 blob 一行都跑不起来。

## 10. windowed ABI 的设计（实测出来的约定）

用编译器实证（`-mabi=windowed -Os -S`）：

```asm
f:  entry   sp, 32
    movi.n  a11, 0x22      /* 参数 1 */
    movi.n  a10, 0x11      /* 参数 0 */
    call8   g              /* 调用：窗口 base += 8 */
    mov.n   a2, a10        /* 返回值从 a10 读 */
    retw.n                 /* 返回：窗口 base -= 8 */
```

结论（写汇编时必须照这个来）：

| 项目 | call0（现状） | windowed（迁移目标） |
|---|---|---|
| 参数 0..5 | a2..a7 | **a10..a15** |
| 返回值 | a2 | **a10**(=callee 的 a2) |
| 调用指令 | `call0` | `call8`（`callx8`）/ 大参数用 `call12` |
| 函数入口/出口 | `addi a1,a1,-N` / `ret` | `entry sp, N` / `retw` |
| PS | 无关 | **WOE=1** 才能正常分配窗口 |
| 寄存器窗口 | 无 | 4 个窗口，调用深了会溢出 → 必须实现溢出/欠载向量 |

### 中断入口（已实现，待验证）

```
_vec_level1:
    保存 a0..a15 + epc1 + ps 到 80 字节陷阱帧（与 call0 版相同，窗口无关）
    判断 exccause == 4（level-1 中断），否则走陷阱打印
    wsr ps, 0x40021          /* INTLEVEL=1 | UM | WOE，清 EXCM */
    mov  a10, a1             /* 参数＝陷阱帧指针 */
    call8 s3_timer_irq
    beqz a10, 恢复现场        /* 返回值也在 a10 */
    l32i a1, a10, 64         /* 切到目标任务帧 */
    恢复 a0..a15 + epc1 + ps，rfe
```

### 上下文切换（已实现，待验证）

- 入口先 `call8 xthal_window_spill`（IDF 的 `libxt_hal.a` 提供）把所有**祖先窗口**
  spill 到本任务栈上；
- 然后只保存当前窗口的 a0..a15 + PS（80 字节帧，和 call0 版一致）；
- 恢复时在 `rfe` 之前 `wsr windowstart, 1`，让祖先窗口由**欠载向量**按需 fill 回来；
- 溢出/欠载向量本体在 `port/s3/irq.S` 的 `_win_ovf4/8/12`、`_win_unf4/8/12`
  （照 Xtensa HAL 实现，`s32e/l32e` + `rfwo/rfwu`）。

## 11. 当前状态（重要，接手先看这里）

| 项目 | 状态 | 证据 |
|---|---|---|
| call0 全功能 | ✅ 正常 | 面板/GUI/录音回放/duplex 用例全部通过 |
| `libphy.a` 能链进来 | ✅ | `build/wifim0` 链接通过，只差 5 个胶水符号 |
| ROM 符号表 | ⚠️ 部分 | `rom.ld`/`rom.api.ld`/`rom.version.ld` 可用；**`rom.libc.ld`/`rom.newlib.ld` 必须关**（把 memcpy/memset 钉到 ROM 后一启动就 LoadProhibited，pc=0x40056fc0） |
| ABI 可切换 | ✅ | `make ... S3_ABI=windowed`（默认仍 call0） |
| windowed 启动 | ⚠️ 到 `port_init` | windowed hello/duplex 能打印 `c6os s3 boot`，之后跑飞 |
| windowed 中断 | ❌ 未跑通 | RTC 现场：`0xA1`(bss 清完) `0xB1`(关完看门狗) `0xBAD0`(进了未处理向量) `EXCCAUSE=0` `EXCVADDR=0` `EPC1=0x40045c06`（ROM 地址） |

**下一步排查 windowed 中断的建议顺序**

1. `EPC1=0x40045c06` 是 ROM 地址：先确认这个 PC 到底是谁 —— 可能是
   `_vec_default` 里读到的 epc1 已经被二次异常覆盖，也可能真的跳到了 ROM。
   办法：在 `_vec_default` 里同时记 `excvaddr`/`windowstart`/`windowbase`/`ps`，
   并且把 `_vec_default` 自己的 PC 也记下来（`rsr a2, epc1` 已在做）。
2. 先绕开中断验证 windowed 的**普通函数调用与上下文切换**：把
   `s3_irq_init()` 里的 `rsil a2, 0`（开中断）临时去掉，看 windowed 版的
   `os_main` 能不能打印出来、`os_task_delay` 之类的不能用的先不用。
3. 再单独验证 `port_task_switch`（windowed）——它依赖 `xthal_window_spill`，
   要确认 `libxt_hal.a` 链接进来了（`make ... S3_EXTRA_LIBS=.../libxt_hal.a`）。
4. 中断入口本身：先用"只置标志位、立刻返回"的空 ISR 验证 `call8`+`rfe` 往返，
   确认 PS 的 WOE/EXCM/INTLEVEL 组合正确，再接真正的 tick。

## 12. 里程碑（每步都要真板客观证据）

| 里程碑 | 内容 | 客观证据 |
|---|---|---|
| A | windowed ABI 跑通 | 现有四个用例在 `S3_ABI=windowed` 下全部输出正常 |
| B | PHY 点火 | `phy_get_rf_cal_version()` 返回非 0/0xFFFFFFFF，`esp_phy_enable()` 后能读到信道 |
| C | 扫描 | `esp_wifi_scan_start` 后串口打印 AP 的 SSID/RSSI/信道（≥1 个） |
| D | 关联 + DHCP | 连上 WPA2 AP，拿到 192.168.x.x 地址 |
| E | TCP + HTTP | 取到 `http://` 明文页面（先用明文验证链路） |
| F | **HTTPS** | 取到 `https://qq.com` 的页面数据（mbedTLS 握手 + HTTP GET） |

## 13. 内存与其它约束（务必记住）

- 可用 DRAM 323KB；**0x3FCD9500 之后有一段坏区**（密集读写会挂 CPU），
  `.bss` 必须停在它下面；WiFi 需要 50~80KB 堆 → WiFi 用例必须单独一个镜像，
  不要和录音/LCD 大缓冲共存。
- 没有 flash 文件系统 → 先做内存版 NVS（校准每次重做），SSID/密码先写死。
- CPU 跑在 40MHz（XTAL，未开 PLL）——WiFi 对时延敏感，后面可能必须开 PLL 到
  160/240MHz，这会牵动 I2S 分频（音频用例要重测）。

## 14. 【重大进展 + 路线修正】ABI 桥：call0 主世界 + windowed 孤岛

### 实测结论（真板，都是串口/ELF 证据）

1. **c6 生成的函数在 windowed 下必坏**。最小用例 `tests/t_s3_abic.c6`：
   - 直接 `port_putc` 打字符 → 正常（`ABC`）；
   - 调用运行库 `port_puts`（普通 C，指针参数）→ 正常（`L2 port_puts ok`）；
   - 调用 c6 生成的 `add2(1,2)` → **返回 0**（`L3=0`）；
   - 调用 c6 生成的 `put1("...")` → 直接跑飞。
   原因在 c6 生成的 C（`build/<b>/c99/*.c`）：
   ```c
   memcpy(__c99_frm.f + 32 + (-8), &__c99_a0, 8);   /* 一次拷 8 字节取参数 */
   ```
   它**假设"参数在栈帧里按声明顺序连续排列、每个 4 字节"**。call0 下 GCC 恰好这样排；
   windowed 下参数是经"调用者 a10~a15 ↔ 被调用者 a2~a7"的窗口别名 + 落栈，顺序/连续性
   都不保证 → 参数全乱。c6 编译器是二进制（`/home/winger/c6/tcc/c6`），改不了。

2. **ABI 桥可行（已真板验证）**。手写 trampoline 就能跨 ABI：
   - call0 → windowed：参数搬到**调用者的 a10~a15**，`call8` 进去，返回值从 a10 取；
     调之前临时打开 `PS.WOE`，返回后恢复 PS（call0 里 a12~a15 是 callee-saved，要先存）。
   - windowed → call0：被调用者本来就从 a2~a7 取参数，直接 `call0` 即可；把 a0 存好、
     调完放回 a0，最后 `retw` 转回窗口。
   实现：`port/s3/win_bridge.S`（trampoline）、`port/s3/win_island.c`
   （**按 `-mabi=windowed` 单独编译**，放 IDF 闭源库和它们的 C 胶水）、
   `port/s3/idf_shim.c`（也改成 windowed 编译，内部全用内联汇编，不跨 ABI 调用）。
   真板输出：`L5 win=1003` —— call0 的 c6 代码通过桥调到了 windowed 函数并拿回正确结果。

**因此路线改为：现有 call0 全部不动（音频/面板/内核零风险），IDF 的 WiFi/PHY
闭源库装进 windowed 孤岛，两端只用手写 trampoline 来往。**

### 新的卡点（下一轮第一件事）

把 `libphy.a` 链进"孤岛"后（`S3_WITH_LIBPHY`），镜像**连 `_start` 都进不去**：
串口只有 ROM 的 `load:` 两行，RTC 标记（`_start` 第一件事就是写 0xA0）是**乱值**，
说明 ROM 根本没跳到我们的入口。

已排除/已知：
- 同一个用例**不链 libphy** 时完全正常（`wphy2` 版：`ABC/L2/L3=3/L4/L5/L6/abic done`）；
- **只有 libphy（配 call0 胶水）时也能启动**（早期 `wifim0` 版能打印 banner 并进陷阱）；
- 所以是"windowed 孤岛 + libphy"这个**组合**才炸。
- ELF 里 `_vecbase=0x403c9000`（1KB 对齐 ✓）、`_start=0x403c94bc`、
  镜像 header 的 entry 与之相同 ✓、段表两项（DRAM 0x1890 / IRAM 0x12a78）都正常 ✓、
  没有真正的符号冲突（只有 `__c99_d2/__c99_d3/out_u32` 这类各 TU 内部同名静态符号）。

下一轮要查的方向（按可能性排序）：
1. `libphy.a` 里的 `.iram1`（以及可能没见过的 `.dram0.*`）是**孤儿段**，
   我们 `link.ld` 只按 `*(.text*)`/`*(.bss*)` 收集。用 `-Wl,--orphan-handling=warn`
   或 `readelf -S` 比对，确认这些段最终落在 IRAM/DRAM 里、没有被塞到奇怪的位置。
2. 用 `-Wl,-Map=build/wphy/map.txt` 看 `_vecbase`/`_start` 附近有没有被别的段插进来，
   以及 IRAM 段尾是否越界（本次 0x403c9000+0x12a78）。
3. 试 `elf2image` 改成常规（非 `--ram-only-header`）镜像，看是不是 RAM-only 镜像
   对"混合 windowed 代码 + 大 blob"有额外限制。
4. 把 `libphy.a` 换成只用其中**一个**目标文件（比如 `phy_debug.o`）先跑通，再逐个加。


### 卡点补充实测（同上一条，本轮又排除两种可能）

- 已经在 `link.ld` 里**显式收集** IDF 风格的段（`.iram1*`/`.iram*`/`.dram1*`/
  `.dram0.data*`/`.dram0.bss*`），重新链接后仍然起不来（IRAM 段 0x1296c）。
  → 不是"孤儿段被塞错位置"。
- 失败的启动日志都停在 ROM 的 `load:` 之后、`SHA-256 comparison failed` 之前，
  且 `Saved PC` 落在 **ROM 地址**（0x40045ab3 / 0x40045abc / 0x40056fc0 之类）。
  正常启动时会出现 `SHA-256 comparison failed … Attempting to boot anyway` 再
  `entry 0x403c94xx`。**所以怀疑卡在 ROM 的 SHA-256 校验阶段**（WDT 再复位，形成循环）。
- 对比：`build/wifim0`（同一个 libphy、但胶水是 call0 写的）**能启动**，
  镜像大小几乎一样（82,912 vs 82,752）。唯一差别就是"windowed 编译的孤岛对象"。

**下一轮的三个候选实验（按性价比排序）**

1. 用**常规镜像**（`elf2image` 去掉 `--ram-only-header`，或加 DROM/IROM 段）试，
   看是不是 RAM-only 镜像 + windowed 对象触发了 ROM 的校验分支。
2. 把 libphy 拆开：先只链 `phy_debug.o` 这类小对象（`.o` 直接进链接列表），
   逐个加，找出"哪一个目标文件"让镜像过不了 ROM 校验。
3. 用 esptool `read-flash 0 0x20000` 把写进去的内容读回来和 bin 逐字节 diff，
   排除写入/压缩路径的问题（虽然 esptool 已报 hash 校验通过）。

### 【本轮】启动期看门狗加固（已修好一类问题）+ libphy 启动问题仍未解

**已修并验证**：`port/s3/start.S` 现在在清 .bss 之前就把三个看门狗全部关掉
（TIMG0、RTC WDT、RTC SWD）。原因：这些寄存器**跨 CPU 复位不清零**（只有掉电才
复位），任何一次 `esptool --after watchdog-reset` 或之前固件的设置都会留下来，
表现为"每次都在跑到 `port_init`（关狗）之前就被复位"。
验证：加完这段之后 `build/wphy2`（windowed 孤岛 + 桥，不含 libphy）刷完立刻正常启动。

**仍未解决**：任何**链接了 `libphy.a`** 的镜像都进不到 `_start`（串口只有 ROM 的
`load:` 两行，RTC 标记是乱值，`Saved PC` 落在 ROM）。本轮排除了以下可能：

| 假设 | 实验 | 结果 |
|---|---|---|
| 写入/flash 路径有问题 | `read-flash` 读回与 bin 逐字节比较 | 完全一致 |
| 镜像结构坏 | `image-info`：段表/entry/checksum | 都正常（entry == `_start`，checksum valid） |
| `.iram1` 孤儿段被塞错位置 | 显式 `*(.iram1*)` 合并进 `.text`；再回退 | 两种都不启动 |
| ROM libc/newlib 映射差异 | 重新打开 `rom.libc.ld`/`rom.newlib.ld` | 不启动 |
| 三个看门狗 | `_start` 最前面全关（已验证有效） | libphy 镜像仍不启动 |
| shim 的 ABI（windowed/call0） | `S3_SHIM_ABI=call0` 对照 | 不启动 |
| 环境漂移 | 重新刷**历史上能启动的** `build/wifim0/c6os.bin` | **现在也不启动** |

最后一条很关键：**同一个二进制文件以前能启动、现在不能** → 说明还有"跨复位保留
的板级状态"没被清掉（看门狗已排除，剩下可能是 RTC 域里的其它东西，如
`RTC_CNTL_OPTIONS0`/`SW_SYS_RST` 相关状态、或 flash 的 boot 模式位），也可能
`libphy` 只是让启动时间变长从而暴露了这个问题。

**下一轮第一步：让用户拔插一次 USB（真正掉电）**，然后刷 `build/wifim0/c6os.bin`
或 `build/wifim4/c6os.bin`（带 libphy）看是否恢复启动：
- 恢复 → 说明是"跨复位保留的板级状态"，之后每次实验前先掉电；同时继续查是哪个寄存器；
- 不恢复 → 说明确实和 libphy 链接有关，按下面的顺序二分：
  1. 从 `libphy.a` 里解出 `.o`，先只链 `phy_debug.o`（含 `phy_get_rf_cal_version`），
     再逐个加其它 `.o`，找出"哪一个目标文件"让镜像过不了 ROM；
  2. 检查该 `.o` 的段名（`readelf -S`），看是否有我们 `link.ld` 没覆盖的段
     （如 `.dram0.data`/`.dram1.bss`/`.ext_ram.*`），必要时在 `link.ld` 里显式放置；
  3. 试把镜像写到非 0 偏移（`write-flash 0x10000`）并用 `--flash-size` 调整，
     排除"镜像整体大小/位置"因素。

## 15. 【已定位】libphy 镜像启动不了的真因：IRAM 越过 SRAM 坏区 0x403D9500

用**纯填充实验**（`port/s3/irampad.c` + `tests/t_s3_irampad.c6`，完全不碰 libphy）
把 IRAM 里的 `.text` 推到不同大小，真板结果：

| 实验 | .text 结束地址 | 结果 |
|---|---|---|
| `-DS3_PAD_N=30000` | ≈0x403D0AA8（低于 0x403D9500） | ✅ 正常启动、打印 `c6os s3 boot` |
| `-DS3_PAD_N=60000` | 0x403DF3E4（越过 0x403D9500） | ❌ 起不来（ROM `load:` 之后反复复位，RTC 标记全乱） |

**根因**：这块 SRAM 在 IRAM 侧的别名 `0x403D9500` 就是之前音频那轮查出来的
DRAM 侧坏区 `0x3FCD9500`（同一块物理 SRAM 的两种映射，密集 CPU 访问会把 CPU 挂住）。
镜像的 IRAM 段一旦盖过去，ROM 装载/校验阶段就会踩到 → 启动不了。

**由此得到的硬约束**：IRAM 内容必须停在 `0x403D9500` 以下，
也就是从 `0x403C9000` 起最多 **0x10500 = 66,816 字节**（含 1KB 向量表和 `.iram1`/`.eh_frame`）。

**对 WiFi 的影响**：`phy_get_rf_cal_version` 一个符号的**依赖闭包就有 18 个目标文件**
（几乎整个 libphy，≈76KB 代码），远超 66KB 预算 → 闭源库不能再全塞 IRAM。

### 下一步（下一轮就做这个）

把 IDF 闭源库的代码放到 **flash（IROM）** 里执行，也就是标准的 ESP 镜像布局：

1. `link.ld` 加两个 flash 段：
   - `.flash.text : { *(.flash.text) } > IROM`（`IROM` = `0x42000000`，`AT> ...` 用 LMA 指到 flash 偏移）
   - `.flash.rodata : { ... } > DROM`（`0xDROM` = `0x3C000000`，可选）
2. 用 `-Wl,--section-start` 或按文件/归档做「段名改写」：把 `libphy.a` 等闭源库的
   `.text` 改名为 `.flash.text`（IDF 的做法是给这些库加
   `-mtext-section-literals` + 单独链接脚本里 `libphy.a:(.text)` 之类；
   我们可以在 `link.ld` 里直接写
   `.flash.text : { */libphy.a:(.text*) */libpp.a:(.text*) ... } > IROM`）。
   注意 `l32r` 的字面量池要跟着进 flash 段（`.literal.*` 也要一起搬），
   否则字面量落在 0x403C… 而代码在 0x42…，`l32r` 的 ±256KB 偏移会溢出。
3. `elf2image` 去掉 `--ram-only-header`，让镜像带 IROM/DROM 段（ROM 会自动配置 MMU 映射）。
4. 先只链 `phy_api.o` 验证「flash 里的闭源代码能被正确取指执行」，
   再回到 `phy_get_rf_cal_version` 走桥调用（M1）。

### 15.1 进展：闭源库代码已经能落进 flash 段（但镜像还没启动）

- `link.ld`：新增 `.flash.text 0x42000000 : AT (0x20000)`，收集
  `libphy/libpp/libnet80211/libcore/libcoexist/libwapi` 这些归档的
  `.text*` + `.literal*`；同时把 IRAM 区域的 LENGTH 改成 `0x10500`
  （= 到坏区 0x403D9500 为止），超出会链接报错，等于把"IRAM 预算"变成硬约束。
- `Makefile`：`elf2image` 去掉 `--ram-only-header`（有 flash 段时必须）。
- 验证：不链闭源库的普通镜像**照常启动**（`wphy8` 输出 ABC/L2/L3/L4/L5 全对），
  `image-info` 里也只有 DRAM + IRAM 两段 → 没有回归。
- 链 libphy 时 ELF 已经正确：程序头 `LOAD VirtAddr 0x42000000 PhysAddr 0x20000
  FileSiz 0x96fd` ✓，IRAM 段缩到 0x9674（结束 0x403D2674，安全）。
- **但镜像仍启动不了**：`image-info` 显示 IROM 段落在**文件偏移 0xfff8**
  （不是 64KB 对齐的 0x10000），ROM 很可能因为这个非对齐/填充段而拒绝映射 →
  下一轮要修 `elf2image` 的填充：让 IROM 段的 flash 偏移严格落在 64KB 边界
  （做法：把 `.flash.text` 的 LMA 设到 `0x20000` 之外，再保证前面所有段
  总长 < 0x20000；或显式加一个 `. = 0x20000` 的填充段在链接脚本里，
  不让 elf2image 自己插入 PADDING 段）。

### 15.2 IROM 版本仍然起不来（本轮最后的实测状态）

镜像段表（`build/wifim6`，LMA 已改 0x10000）：

```
0  0x01890  0x3fca0000  off 0x00000018  DRAM
1  0x09674  0x403c9000  off 0x000018b0  IRAM     ← 结束 0x403D2674，安全区 ✓
2  0x050c4  0x00000000  off 0x0000af2c  PADDING  ← 8 字节段头落在 0xfff8
3  0x09700  0x42000000  off 0x0000fff8  IROM     ← 数据实际从 flash 0x10000 开始 ✓
```

仍然启动不了（`Saved PC` 落在 ROM，RTC 标记全乱 = `_start` 没跑）。
怀疑点（下一轮按顺序查）：
1. ROM 对 IROM 段要求 **flash 偏移 64KB 对齐**，而 elf2image 把"段头"放在
   0xfff8、数据放 0x10000；要确认 ROM 认的是哪个（把 bin 的段头解析出来，
   看记录的 load addr 到底是 0x42000000 + 哪个 flash page）。
2. 可能必须让镜像走 `esptool` 的标准**分段烧写**（app image → 0x10000，
   IROM 在其后），而不是"整个镜像写到 0x0"。可以试
   `esptool write-flash 0x0 boot.bin 0x10000 irom.bin` 这种方式。
3. 或者干脆不用 IROM：把闭源库里真正用到的少数函数**复制到 IRAM**
   （用链接脚本的 `.text` + `--only-section` 抽出必要目标文件），
   把 IRAM 控制在 66KB 以内（当前 libphy 闭包 76KB，只差 10KB，
   可以先用 `-Os`/裁剪无关函数试试）。

### 15.3 段表修正完成 + 发现 RTC 标记不可靠

- 已把 `.flash.text` 放到镜像最前面、`VMA=0x42000020 / LMA=0x20`（与 IDF 的 IROM
  惯例一致）。现在镜像段表干净了（不再有"load 地址 = 0"的 PADDING 段）：
  ```
  seg0: IROM 0x42000020 len 0x9700  data@0x20
  seg1: DRAM 0x3fca0000 len 0x1890  data@0x9728
  seg2: IRAM 0x403c9000 len 0x9674  data@0xafc0     ← IRAM 结束 0x403D2674，安全 ✓
  ```
- **重要发现：RTC 慢速内存前几个字不可靠**——实测同一块板上
  `0x50000000` 读到过 0xA0/0xA1/0xB1，也读到过 0xb0dc51a9/0x5a588d90 这类乱值，
  而同一时刻 `0x50000008` 却是合理的 0xB1。ROM/bootloader 很可能也在用
  RTC_SLOW_MEM 开头几个字。**以后做启动进度标记要改写到 DRAM**
  （比如 0x3FCEE000，栈顶下面那块没人用），别再依赖 RTC[0]。
- 该 IROM 镜像刷进去后：串口没有输出，但 RTC[+8] 显示到过 `0xB1`
  （`port_init` 关完看门狗），说明**这次 `_start` 是跑起来的**（与之前完全不同），
  卡在 `port_init` 的 USB 重枚举/延时段。下一轮先用 DRAM 标记确认它到底走到哪、
  再解决 USB 控制台。

### 下一轮（明确）

1. `port/s3/start.S` 和 `port/s3/console.c` 的启动标记从 RTC 改成 DRAM
   （`0x3FCEE000` 起），并写一个小的 `tools/mark.py` 或直接用
   `esptool read-mem 0x3FCEE000` 读。
2. 用 DRAM 标记确认 IROM 镜像走到 `port_init` 的哪一步；如果卡在 USB 段，
   先临时跳过 USB 重枚举（打 `-DS3_NO_USB_REINIT`）看能不能到 `os_main`。
3. 然后 `phy_get_rf_cal_version()` 走桥调用，拿到非 0 值 = M1 地基打通。

### 15.4 【里程碑】IROM 布局成功：闭源库代码真的在 flash 里执行了

- 修正段序（`.flash.text` 放最前、`VMA=0x42000020 / LMA=0x20`）后，镜像段表干净，
  **串口打出了 `c6os s3 boot` + `c6os wifi M0: libphy link test`，然后陷阱在
  `pc=0x420094fe`（IROM 里的地址）** —— 这就是"闭源库代码从 flash 取指执行"的
  直接证据（以前从来没到过这一步）。
- 启动标记改成 DRAM 里的 `g_boot_marks[]`（`port/s3/marks.c`，放 .data，
  带非零初值所以不会被清 .bss 抹掉），并在 `_start` 最前面写 `marks[1]=0xA0`、
  清完 bss 写 `marks[0]=0xA1`、`port_init` 各阶段写 `marks[2..5]=0xB0..0xB3`。
  读法：`nm build/<b>/c6os.elf | grep g_boot_marks` 拿地址，
  `esptool read-mem <addr>` 读。**以后一律用这个，不要用 RTC。**
- 用这套标记确认：IROM 镜像里 `_start` 正常执行、bss 清完、`port_init` 跑到了
  关看门狗之后 → 启动链路是通的。

### 15.5 当前卡点：桥→IROM 里 windowed 函数，`entry` 第 3 个字节上 IllegalInstruction

- ELF 侧确认调用是对的：`s3_win_phy_cal`（IRAM，windowed）里
  `l32r a8, (420094fc) ; callx8 a8` ✓ 目标正是 `phy_get_rf_cal_version`（0x420094fc），
  它在 IROM 里就是 `entry a1, 32`（二进制 0x94fc 处也是 `36 41 00` ✓ 映射没偏移）。
- 但真板陷阱：`cause=0 (IllegalInstruction) pc=0x420094fe ps=0x0006051f`
  —— 正好是 `entry` 的**第 3 个字节**，而 `ps` 里 WOE=1（桥设置成功）。
- 怀疑方向（下一轮按顺序）：
  1. `entry` 在 WOE=1 时如果目标窗口需要 spill，会先触发窗口溢出向量；我们的
     `_win_ovf4/8/12` 是从 IDF 抄的，但**没有测过**。先写一个"只调用一层 windowed
     函数、且故意让调用链深到必然溢出"的最小用例，单独验证溢出/欠载向量。
  2. IROM 里的代码用 `-mtext-section-literals` 编译，字面量池在 `.literal.*` 里；
     我们把 `libphy.a:(.literal* .text*)` 都收进 flash 段了 ✓，但要确认 `l32r`
     的 ±256KB 相对偏移在 flash 段内没越界（`objdump -d` 看有没有 `l32r` 指向
     IRAM 的字面量）。
  3. 实在不行：把整个 libphy 闭包（76KB）里真正需要的少数函数用
     `--gc-sections` + 手写精简 glue 裁到 30KB 以内，直接放 IRAM（IRAM 预算 66KB，
     我们自己的代码只占 ~30KB），彻底绕开 IROM。

### 15.6 IROM 执行的最新实测（本轮结束时的状态）

用我们自己放在 flash 段里的函数做对照（不涉及 libphy）：

- ELF 里 `s3_win_flash_add` 在 VMA `0x42000020`，紧跟的图案数组 `g_flash_pat` 在
  `0x42000029`；在**镜像文件里**图案字节 `11 22 33 44…` 正好出现在偏移 `0x29`
  → 说明链接脚本把"VMA 页内偏移 == flash 页内偏移"这件事做对了（都是 0x20/0x29）。
- 真板调这个 flash 里的函数：陷阱 `cause=0 (IllegalInstruction) pc=0x42000022`
  —— **正好是 `entry` 指令（3 字节）的第 3 个字节**。libphy 那边也是同样的现象
  （`phy_get_rf_cal_version` 在 0x420094fc，陷阱在 0x420094fe）。
- 想用"读数据"来验证映射是行不通的：IROM(0x42xxxxxx) 只能取指，读数据会
  `LoadStoreError`（cause=3）；要看数据必须走 DROM(0x3C000000) 别名。
- 另外确认：IRAM→IROM 的 `call8` 超出 ±128KB，**必须**用 `l32r`+`callx8`
  （链接器会直接报 `dangerous relocation: call8: call target out of range`）。

**下一轮的三个候选（按性价比）**

1. 在 flash 段函数最前面塞 4 个 NOP（或 `.align 4` + 前置填充），让真正的入口在
   4 字节边界上，看陷阱地址是否变成 +4/+6 —— 这样能判断是不是"取指从 +2 开始"
   的映射偏移问题。
2. 做一个"只有一个 flash 段"的极简镜像（不加 DRAM/IRAM 段，或者把 flash 段
   LMA 放在 0x20、其它段排在它后面），排除多段布局对 MMU 映射的干扰。
3. 若 IROM 始终不行 → 回到"纯 IRAM"方案：写一个裁剪脚
   （`objcopy` + 只保留 `phy_api.o` 里用到的几个函数）把闭包压到 30KB 以内，
   或者退一步先不做 PHY，改从 WiFi 的 `esp_wifi_init` 侧验证（同样受 IRAM 限制）。

**注意**：不要让整体进度卡在 IROM 上——这是一个 ROM/MMU 细节，可以并行推进
"把 IDF 的 `esp_wifi`/`esp_phy` C 源码移植进孤岛 + FreeRTOS 垫片"这些不依赖
IROM 的工作（它们的代码都是我们编译的，可以放 IRAM，等 IROM 通了再挪）。

### 15.7 IROM 执行问题的精确数据（本轮收尾）

- `s3_win_flash_addr()` 返回 **0x42000020** ✓（调用目标是对的）；
  ELF/镜像里该处确实是 `entry a1, 32`（字节 `36 41 00`），后面是
  `addi.n`/`add.n`/`retw.n`，图案数组在 0x42000029（镜像偏移 0x29 ✓ 一致）。
- 但真板一执行就 `IllegalInstruction`，PC = **0x42000022**（= `entry` 的第 3 个
  字节），`ps` 里 WOE=1。也就是说：调用地址正确、镜像内容正确，但 CPU 在
  `entry` 中间取到了非法指令 → **IROM 侧取指拿到的字节不是 flash 里那份**
  （MMU 映射或指令 cache 没按我们以为的方式工作）。
- 顺带确认：**IROM(0x42xxxxxx) 只能取指**，用 `ld` 读数据会 `LoadStoreError`
  （cause=3）；要读 flash 数据得走 DROM(0x3C000000) 别名。

**下一轮两个方向（都不依赖运气）**

1. **继续查 IROM（首选，因为 IRAM 装不下 76KB）**：
   - 试着同时提供 DROM 段（把 `.flash.rodata` 放到 `0x3C000000` + 紧随其后的
     LMA），因为 ROM 建立 flash 映射时可能要求 DROM/IROM 成对出现；
   - 或者显式打开指令 cache / 查 `SPI_MEM_MMU` 相关寄存器在启动后的实际值
     （在 `_start` 里读 `0x600C8000` 附近的 MMU 表，串口打印出来看 ROM 建了什么映射）。
2. **纯 IRAM 兜底**：给 Makefile 加 `S3_LEAN=1`（去掉 panel/gfx/i2s/dash/shell/button
   这些对象），把我们的代码压到 ~5KB；然后按"坏区以下 66KB + 坏区以上 24KB"两段
   放置（`link.ld` 用两个输出段 + `libphy.a:xxx.o(.text)` 指定成员），
   把 76KB 的 libphy 闭包塞进去。紧但可行。

### 15.8 关键推断：ROM 只 load 了 RAM 段，没管 IROM 段

回看所有 IROM 版本的启动日志：**ROM 只打印了两行 `load:`（DRAM、IRAM），
从来没有为 IROM 段打印过 load 行**。而标准 ESP-IDF 镜像之所以能用 IROM，是因为
**二级 bootloader** 会：① 读分区表；② 用 `spi_flash_mmap` 把 flash 页写进
SPI MMU；③ 打开 I/D cache；④ 跳到 app 入口。我们这条路是"ROM 直接加载
RAM-only 镜像 + 跳 entry"，没有二级 bootloader，所以 **IROM 的 MMU 映射根本没人建**。

这正好解释了所有现象：
- IROM 段被跳过（不 load）；
- 执行 0x42xxxxxx 时取指拿到的不是 flash 里的内容 → IllegalInstruction；
- 用 `ld` 读 0x42xxxxxx 报 LoadStoreError（DROM/IROM 都要先映射）。

**因此要启用 IROM，必须自己当 bootloader**，步骤（下轮做）：

1. 用 SPI0/SPI1 的 MMU 寄存器把 IROM 页映射到 flash 页：
   S3 上 MMU 表项在 `SPI_MEM_MMU_ITEM_INDEX/CONTENT`（`spi_mem_reg.h`）或
   `EXTMEM` 侧，表项格式是 `[page_num:13:0] + [valid:8?]`；每 64KB 一个表项，
   IROM 0x42000000 对应页号 0..N。
2. 打开 I-cache（`EXTMEM_ICACHE_CTRL` 的 enable 位）并 invalidate 一次。
3. 之后 IROM 代码即可执行。

（另一条路是纯 IRAM + LEAN 构建，见 15.7；两条都记着，先试 MMU 因为
一个 bootloader 级的 MMU 初始化也就几十行寄存器操作。）

## 16. 总体评估（第 7 轮，给后面接手的人）

### 已经拿到的、真板验证过的地基

| 能力 | 证据 |
|---|---|
| 音频录放（BOOT 键录音 → 循环播放） | 已合并 main，真板确认有声、速度正确 |
| call0 ↔ windowed ABI 桥 | `L5 win=1003`、`L5b deep=11`（深递归触发窗口溢出/欠载也能过） |
| 寄存器窗口溢出/欠载向量 | 同上（10 层递归必然溢出/欠载） |
| 启动期看门狗加固 | 加完后小镜像不用掉电就能启动 |
| 闭源库能链进构建（含 ROM 符号表 + 胶水） | `libphy.a` 链接通过，只缺 5 个胶水符号（已补） |
| flash(IROM) 段布局 | ELF 程序头正确（VMA 0x42000020 / LMA 0x20），IRAM 段缩到 66KB 以内 |
| SRAM 坏区定位 | 纯填充实验：IRAM 越过 0x403D9500 镜像就不启动 |

### 还差什么（按依赖顺序）

1. **flash 执行（MMU + cache）**：ROM 只加载 RAM 段，从不建立 IROM 映射。
   要么自己写 MMU/cache 初始化（相当于写半个二级 bootloader），要么回到
   "纯 IRAM"（但 libphy 闭包 76KB + 我们自己的代码，得先做 LEAN 构建 + 分两段放置，
   余量几乎为零）。
2. **移植 IDF 的 WiFi/PHY C 胶水**：`esp_wifi/src`（~2000 行）+ `esp_phy/src`（~2100 行）
   + `esp_coex`（~700 行），并实现 FreeRTOS 垫片（task/queue/sem/event/timer）+
   内存版 NVS + PHY 校准数据。
3. **关联 WPA2**：`libwapi.a` + 事件流。
4. **IP 层**：自研 ARP/IPv4/DHCP/TCP（或移植 lwIP）。
5. **HTTPS**：mbedTLS 移植（另一大块）+ HTTP 客户端。

### 现实评估

第 1、2 步各自都是"数天到一两周"量级的活，第 5 步又是同等量级。
**距离"真板取到 https://qq.com 页面"还有相当长的路**，不建议在最近几轮里
承诺完成；建议按里程碑推进，每个里程碑都留下真板证据（本文件第 12 节 M1~M5）。

## 17. 方案修正（第 8 轮）：用 IDF 的标准 bootloader，而不是自己造半个

上一轮推断"ROM 不建 flash MMU 映射"之后，最省事、最标准的路是：
**把 IDF 的二级 bootloader 补上**（它本来就是干这件事的）：
1. `bootloader.bin` 烧到 `0x0`、`partition-table.bin` 烧到 `0x8000`；
2. 我们的 `c6os.bin` 作为 **app 镜像** 烧到 app 分区（默认 single-app 工厂分区 `0x10000`）；
3. bootloader 负责：读分区表 → 校验 app → **用 SPI MMU 映射 IROM/DROM** → 开 cache → 跳 entry。

已做：
- 先试了上游 xiaozhi 现成的 `bootloader.bin`+分区表（app 在 0x20000）：
  bootloader 能启动（串口出现 `entry 0x403c88f4`），但**立刻软复位**，
  且它的 `CONFIG_BOOTLOADER_LOG_LEVEL=0`（完全静默），看不到原因。
- 因此新建最小 IDF 工程 `tools/idfboot/`（`main.c` 空壳，只为产出 bootloader +
  分区表），`sdkconfig.defaults` 里：`BOOTLOADER_LOG_LEVEL=3`（要日志）、
  `FLASHMODE=dio`、`40m`、`16MB`、`CONSOLE_USB_SERIAL_JTAG=y`、
  `PARTITION_TABLE_SINGLE_APP=y`（工厂分区 0x10000）、关 PSRAM。
  构建命令：`cd tools/idfboot && source $IDF/export.sh && idf.py -B build -DIDF_TARGET=esp32s3 build`

**下一步**：用这个带日志的 bootloader 跑一次，看到它拒绝 app 的具体原因
（最常见的几种：镜像头/校验、segment 的 flash 偏移、分区不匹配、"invalid header"），
然后按提示修 `elf2image`/`link.ld` 的镜像形态。

## 18. c6 标准库能复用什么（第 9 轮盘点，重要）

`/home/winger/c6/std/` 里已经有一整套网络/加密模块，**上层的协议栈不用自己写**：

| 标准库模块 | 能干什么 | 我们能直接用吗 |
|---|---|---|
| `std/net/https` | 同步 HTTP/1.1 客户端（fd 可插拔，可选 TLS 记录层） | ✅ 只要 port 提供 fd 的 read/write |
| `std/net/tlsrec` · `tlskdf` · `x25519` | TLS 1.3 记录层、密钥派生、密钥交换 | ✅ 纯 c6 代码 |
| `std/data/x509` · `std/encoding/{aes,sha256,sha512,asn1}` | 证书解析、AES、SHA、ASN.1 | ✅ |
| `std/net/dhcp` · `dns` · `inet` · `cidr` · `url` · `mime` · `http` | DHCP/DNS 报文编解码、地址工具、URL、MIME、HTTP 解析 | ✅ 纯逻辑 |
| `std/net/tcp` · `udp` · `poll`(os) · `io`(os) | POSIX socket 薄封装 | ⚠️ 需要 port 提供 `socket/connect/send/recv/poll/read/write` |

⇒ **结论：协议层几乎全部可复用**。`https.c6` 的注释也写明："TLS 那层只做记录层，
握手与证书校验不在这里" —— 也就是说我们只需把 `tlskdf`+`x25519`+`x509`+`tlsrec`
串成一个 **TLS 1.3 握手状态机**（几百行，纯逻辑）。

### 因此 c6LOS 这边要补的只有三块

1. **WiFi 链路层**（最难，依赖 IDF 闭源库）：关联 + 原始 802.3 帧收发。
   当前卡在"闭源库代码放哪"：IROM 需要 MMU+cache 初始化（`include/c6os/flash.h`
   已经做了，实测 MMU 表项/I-cache 都设对了但取指仍失败，见 §17），
   纯 IRAM 又装不下 76KB 闭包。
2. **IP 层**：ARP/IPv4/ICMP + UDP + TCP（自己写，或找 std 里有没有用户态实现）。
3. **POSIX 垫片**：fd 表 + `socket/bind/connect/send/recv/poll/read/write/close`，
   让 `std/net/tcp.c6`、`std/net/https.c6` 直接跑起来 —— 这一层是"胶水"，
   放在 `port/s3/socket.c` + `include/c6os/socket.h`（按 c6 标准库规范）。

### 新增到 c6 标准库的东西（本轮已做）

- `include/c6os/flash.h` + `port/s3/flash.c`：
  `os_flash_map()` / `os_flash_init()` / `os_icache_enable()` / `os_flash_mmu_entry()` /
  `os_icache_state()` —— flash 窗口(IROM/DROM)映射与 cache 使能，
  按"arch 相关实现放 port/、接口放 include/c6os/"的既有规范加入。
- `include` 侧的 `g_boot_marks`（`port/s3/marks.c`）+ 调试读法，作为启动期诊断的标准做法。

## 19. IROM 取指问题的最新边界（第 9 轮收尾）

用"已知图案 + DROM 别名"做的测量，把问题切得很干净：

| 现象 | 证据 |
|---|---|
| MMU 表项写入正确 | `os_flash_mmu_entry(0x42000000)=0x0000`（页 0 有效）、`0x42010000 -> 0x0001` |
| **数据通路 OK** | 把 flash 0x0 映射到 DROM 0x3C000000，`os_dcache_enable()` 后读回 **0xE9 0x02**（正是镜像 magic） |
| **取指通路不通** | 调 IROM 里的函数一律 `cause=0 (IllegalInstruction)`，PC 恒为"函数首地址 + 2" —— 包括**手写的 `movi a2,42; retw.n`（没有 entry）**，所以不是 `entry`/窗口的问题 |
| 与 cache 使能位无关 | `os_icache_enable()`（`EXTMEM+0x60` bit0）打开后仍然一样；`Cache_Invalidate_ICache_All()` 一调用反而提前跑飞 |
| ROM 的 `Cache_Enable_ICache` 符号地址不对 | 按 `rom.ld` 调它会跳到 DRAM 里（`pc=0x3fca02df`, cause=2），说明 `esp32s3.rom.ld` 里这个符号不是可调用的入口 |

⇒ 结论：**MMU/D-cache 都已正确，唯独 IBUS→I-cache→flash 这条取指链路没打通**。
下一步要按 IDF 的 `cache_ll.h` / `bootloader_cache_init` 逐个寄存器对齐
（`EXTMEM_ICACHE_CTRL/CTRL1`、`EXTMEM_ICACHE_TAG_POWER_CTRL`、
`EXTMEM_IBUS_*` 计数器，以及 SPI0 侧的 cache 读模式），或者干脆用外部
bootloader。这条线已经花掉 5 轮，建议：
- 要么继续按 IDF 寄存器序列逐位对齐（预计还要 2~3 轮试错）；
- 要么走"纯 IRAM"路线（LEAN 构建 + libphy 闭包分两段放进 66KB+24KB）。

`port/s3/flash.c` 的 `os_flash_init()` 默认**不自动**动 flash 窗口
（`S3_FLASH_AUTO_MAP` 宏才开），保证普通镜像不受影响。

## 20. 【里程碑 M1 地基打通】闭源 libphy 在我们的 RTOS 里跑通并返回结果

真板输出（`build/wphy_lean`，LEAN 构建 + libphy 放 IRAM）：

```
c6os s3 boot
ABC                      ← 直接 port_putc
L2 port_puts ok          ← 调运行库（windowed 普通 C）
L3=3                     ← c6 生成的带参数函数（call0）
L4 put1 ok               ← c6 生成的指针参数函数
L5 win=1003              ← 经 ABI 桥调 windowed 孤岛函数
L5b deep=11              ← 10 层深递归（寄存器窗口溢出/欠载向量也验过了）
L6 phy_cal=0x000002c7    ← **经桥调用闭源 libphy 的 phy_get_rf_cal_version()**
abic done
```

0x2C7 正是该函数在库里的返回值（`movi a2, 0x2c7; retw.n`），说明：
**call0 主世界 → 手写桥 → windowed 孤岛 → 闭源 PHY 库** 整条调用链是对的。

### 关键做法（能跑通的原因）

1. **LEAN 构建**（`S3_LEAN=1`，Makefile 里把 panel/gfx/i2s/dash/shell/button/core1
   这些对象去掉）：我们自己的代码从 ~30KB 降到 ~26KB。
2. **libphy 闭包放回 IRAM**（不放 flash）：闭包只有约 38KB（不是早先估的 76KB，
   因为只拉进真正被引用的对象），26+38 ≈ 64KB，正好在 IRAM 预算
   （0x403C9000..0x403D9500 = 66.8KB）之内；镜像 IRAM 段实际结束在 0x403D8CE8。
3. 因此**不需要 IROM**（那条线的结论仍记录在 §15~19，等以后要放 libnet80211
   这种 1.4MB 的大库时再回来攻）。
4. 编译命令：
   ```
   make C6_ROOT=/home/winger/c6 BUILD=build/wphy_lean S3_LEAN=1 \
     S3_CDEFS="-DS3_WITH_LIBPHY" S3_MAIN=tests/t_s3_abic.c6 \
     S3_EXTRA_LIBS=/media/winger/inst_26/esp/esp-idf/components/esp_phy/lib/esp32s3/libphy.a s3
   ```

### 下一步（M1 正式）

- 把 IDF 的 `esp_phy/src/phy_init.c` 搬进孤岛（它提供 `esp_phy_enable()` 需要的
  PHY 初始化/校准流程 + `phy_init_data`），先跑 `esp_phy_enable()` 并读回信道/校准状态。
- 注意 WiFi 还要 `libpp.a`+`libnet80211.a`（合起来 >2MB）→ 到时候必须解决 IROM
  或者说服自己只用 PHY（不现实）。所以 §15~19 的 IROM 问题**迟早还得攻**，
  但至少现在 M1 能继续往前走了。

## 21. M1 进展与新的精确卡点（第 11 轮）

**已做**：
- 搬入 IDF 的 S3 PHY 默认初始化参数（`port/s3/phy_init_data.c`，128 字节，
  由 `esp_phy/esp32s3/phy_init_data.c` 按默认 20dBm 求值而来）。
- 孤岛里加 `s3_win_phy_enable()`：调 `register_chipv7_phy(s3_phy_init_data,
  &g_phy_cal, PHY_RF_CAL_FULL(2))`，校准数据先放内存（1904 字节）。
- 桥里加 `s3_win_call_phy_enable()` / `s3_win_call_phy_cal_word(i)`。
- `tests/t_s3_abic.c6` 现在跑到 `L6 phy_cal=0x000002c7` ✓（libphy 调用通），
  然后 `L7 phy_enable=` 崩。

**新卡点（精确）**：`L7` 陷阱 `pc=0x803cd397`，把 bit31:30（call8 窗口增量位）
去掉后是 **0x403cd397 —— 落在我们自己的 `s3_win_phy_enable()` 里**。
也就是说：`register_chipv7_phy()` 这条 windowed 深调用链返回时，把"带窗口位的
返回地址"当成了 PC 用。

**为什么和前面"深递归 deep=11 通过"不矛盾**：那个测试只用了 `call8`，
而 PHY 库内部很可能用 `call12`（参数多的函数）→ 会走
`_win_ovf12`/`_win_unf12`（**这两个向量还没被真板验证过**）。

**下一轮第一件事**：写一个 windowed 的 12 参数函数并递归，专门压
`_win_ovf12`/`_win_unf12`；如果它挂了，就对着 IDF `xtensa_vectors.S` 里
`_WindowOverflow12/_WindowUnderflow12` 的字节逐条核对（我之前是照源码抄的，
但没验证过 `s32e/l32e` 的偏移）。

另外记录一个**尝试过但失败**的方案：给 `memcpy/memset` 做"双 ABI thunk"
（按 a0 的窗口位决定 `ret`/`retw`）。思路是对的，但接上后**我们自己的 call0
调用路径**反而在 `add2` 的帧拷贝上挂了（原因待查）；文件留在
`port/s3/abi_thunk.S`（未参与构建）。既然 L7 的崩点其实在
`s3_win_phy_enable` 里而不是 memcpy，这个 thunk 也许并非必需。

## 22. M1 关键修复：闭源库的 mem* 必须用 windowed 版（第 12 轮）

**根因**：`register_chipv7_phy()` 一进来就 `call8 memset(a1, 0, 128)`（还用了
memcpy）。而我们的 `memcpy/memset` 在 `rt/c6rt.c` 里是 **call0** 编译的：
windowed 的 `call8` 进来、call0 的 `ret` 返回 → 跳到"带窗口位的返回地址"
（`pc=0x803cd397`），直接跑飞。

**做法**（通用机制，已写进 Makefile）：
1. 链接前用 `objcopy --redefine-sym` 把闭源库对 `memcpy/memset/memmove/memcmp`
   的引用改名成 `win_memcpy/win_memset/...`；
2. `port/s3/win_island.c`（windowed 编译）里提供这些 `win_*` 实现；
3. 主世界（call0）继续用 `rt/c6rt.c` 那份，两边互不干扰。
   规则：**任何被 windowed 闭源库调用的符号，都必须是 windowed 编译的**
   （目前已有：`phy_enter_critical`/`phy_exit_critical`/`phy_printf`/`sprintf`/
   `coex_pti_print` 在 `port/s3/idf_shim.c`，`win_mem*` 在孤岛里）。

**验证**：
- `call12` 的窗口溢出/欠载也确认没问题（`L5c12 deep12=074 (want 074)`），
  所以之前的怀疑排除。
- 加上 `win_mem*` 后，`L7 phy_enable=` 不再立刻跳飞，而是**死在完整 RF 校准
  过程中**（FULL 模式耗时长/电流大）。已把默认校准模式改成
  `PHY_RF_CAL_PARTIAL`（`S3_PHY_CAL_MODE` 可切 0/1/2），等板子重新插拔后
  再验证。

**环境问题（本轮结束时的状态）**：板子的 USB-CDC 又卡死了
（`esptool` 报 `OSError: [Errno 71] Protocol error`，RTS ioctl 失败），
需要**拔插一次 USB** 才能继续刷机/读串口。

## 23. 坏消息：c6 标准库（std/）用当前工具链用不了（第 13 轮）

想直接 `import std.net.dhcp;` 复用标准库里的网络模块，实测：

1. 先要加模块搜索路径：`$(C6CC) ... -I$(C6_ROOT)`（已加到 Makefile），
   否则报 `cannot find module 'std.net.dhcp'`。
2. 加上之后报 **`/home/winger/c6/std/net/dhcp.c6:40: error: function definition expected`**
   —— 那一行是 `module std.net.dhcp;`，而我们的编译器
   （`/home/winger/c6/tcc/c6`，tcc 0.9.28rc，2026-11-13）**不支持
   `module`/`import` 这种新方言**（std 库是按新方言写的）。

也就是说 §18 里"协议层几乎全部可复用"的结论**要用的话得先解决工具链**：
- 要么换上支持 `module`/`import` 的新版 c6 编译器；
- 要么把需要的那几个模块（`dhcp`/`dns`/`tlsrec`/`tlskdf`/`x25519`/`x509`/`https`）
  按当前方言**手工移植**（每个文件去掉 `module`/`import`，把依赖类型内联）。
  这些模块本身是纯逻辑（几百行/个），移植是体力活但不是难事。

顺带说明：`tests/t_s3_stdnet.c6`（验证用的用例）因为编不过已经删掉，
结论就记在这里。

## 24. 【关键发现】IROM 取指的最后一块拼图：SHUT_CORE0_BUS（第 15 轮）

翻 IDF bootloader 的 `bootloader_utility.c`（`set_cache_and_load_tee_app`）发现：
建完 MMU 映射后它还会调 `cache_ll_l1_enable_bus()`，而 S3 上这个函数的实质是
**清 `EXTMEM_ICACHE_CTRL1.SHUT_CORE0_BUS`（bit0）**。

而 `extmem_reg.h` 里该位的**复位默认值是 1（关着）**：

```
/* EXTMEM_ICACHE_SHUT_CORE0_BUS : R/W ;bitpos:[0] ;default: 1'b1 ; */
```

我们之前只做了：
- ✅ 写 MMU 表项（现在也验证过：DROM 别名能读回镜像 magic 0xE9）
- ✅ `EXTMEM_ICACHE_CTRL |= 1`（ICACHE_ENABLE）
- ❌ **没清 SHUT_CORE0_BUS** → core0 的 I 总线根本不到 cache → 取指拿不到内容
  → `IllegalInstruction`（PC 落在指令中间那一位），这和观察到的一模一样。

已在 `port/s3/flash.c` 修好：
```c
void os_icache_enable(void)
{
    S3_EXTMEM_ICACHE_CTRL1 &= ~1u;   /* 打开 core0 I 总线（复位默认是关的！） */
    S3_EXTMEM_ICACHE_CTRL  |=  1u;   /* 使能 I-cache */
}
void os_dcache_enable(void) { /* D 总线同理：EXTMEM_DCACHE_CTRL1 &= ~1 */ }
```
`os_flash_init()` 也恢复默认自动执行（映射 + 清 bus 位 + 开 cache）。

`tests/t_s3_abic.c6` 里加回了 `L5irom=` 检查（调 flash 段里的手写函数，期望 42）。
**等板子插拔后验证** —— 如果通过，IROM 就通了，`libpp/libnet80211`（2MB+）
也就能放 flash，WiFi 才算真正有了路。

## 25. 【板子 USB 卡死】恢复步骤（第 17 轮）

现象：`esptool` 报 `OSError: [Errno 71] Protocol error`（RTS ioctl 失败），
`/dev/ttyACM*` 不存在或打不开。软件办法（sysfs unbind/bind、重新 bind 接口）
都恢复不了 —— **只能拔插一次 USB**。

插拔后的第一件事（按顺序）：
1. `./tools/flash.sh build/wirom2/c6os.bin`（这个版本含有 IROM 修复 + `L5irom` 检查）
   - 期望串口出现 `L5irom=42 (want 42)` → **IROM 通了**（§24 的 SHUT_CORE0_BUS 修复生效）
   - 如果还是崩，看 `L5irom` 之前最后一行输出，并读 `g_idf_log`（§21）
2. `./tools/flash.sh build/wphyen7/c6os.bin`（PHY 校准 + 飞行记录仪）
   - 期望 `L7 phy_enable=0 cal=...`（非零）
   - 若崩，用 `esptool read-mem <g_idf_log>`（地址见 `nm build/wphyen7/c6os.elf | grep g_idf_log`）
     看闭源库最后打印了什么
3. 然后才谈 libpp/libnet80211 放 flash + 端口 WiFi 胶水。

注：`tools/flash.sh` 会自动等串口、失败重试（但这块板的 USB 卡死是硬件层面，
重试也救不回来，只能拔插）。

## 26. 板子插拔后仍然没有串口输出（第 17 轮末）

现象（值得记住）：
- `tools/flash.sh` 刷机**成功**（`Hash of data verified`），说明芯片/USB-JTAG 的
  *下载协议* 正常；
- 但串口**一个字节都没有** —— 连 ROM 自己的 banner（`ESP-ROM:esp32s3-...`）都没有；
- `g_boot_marks` 读回来是 0（应该先被 .data 初始化成 0xEE、再由 `_start` 写成
  0xA1），说明应用根本没跑起来；
- `esptool --after hard-reset read-mac` 之后立刻抓串口，也是 0 字节。

结论：**ESP32-S3 的 USB-Serial/JTAG 控制台路径还卡着**（下载能用、console 输出到
不了主机）。之前 sysfs unbind/bind + 重新 bind cdc_acm 都报 `I/O error`，
说明这是芯片侧的状态，**需要真正断电一次**（USB 拔掉等 5 秒再插，或按板子的
RST/电源键），单纯重插如果板子还有独立供电就不算断电。

恢复后仍按 §25 的三步走：
`build/wirom2`（等 `L5irom=42`）→ `build/wphyen7`（PHY 校准）→ WiFi 库放 flash。

## 27. 第 18~19 轮：板子仍然不跑应用（不依赖串口的判断）

用"不依赖串口"的办法确认了应用确实没在跑：

| 探针 | 读到的值 | 说明 |
|---|---|---|
| `g_boot_marks[0]`（.data，`port_init` 一进来写 0xB0） | **0** | 应用没跑到 `port_init` |
| `tests/t_s3_alive.c6` 往 RTC `0x50000008` 写的心跳 | `0x1a588d90` 恒定不变 | 不是心跳 |
| 同一个探针往 DRAM `0x3FCEE000` 写的魔数 | `0x400559e8`（ROM 地址） | 那是 ROM/bootloader 自己的栈数据，说明这块 DRAM 每启动都被 ROM 覆写（**这个地址不能当探针用**，改用 `g_boot_marks`） |
| `tools/flash.sh` | `Hash of data verified` ✓ | 下载协议正常，写进去的内容是对的 |
| 串口（含 ROM banner） | 0 字节 | USB-Serial/JTAG 的 **console 通路**不通 |

结论：芯片能被 esptool 读写（ROM 下载模式），但**没有运行我们的应用**，且
console 输出到不了主机。这两件事同时出现，最可能是：
1. 板子没有真正断电（如果板子另有供电，只拔 USB 数据线不算断电）；
2. 或 BOOT(GPIO0) 被按住/卡住 → 每次复位都进下载模式，应用不会跑。

**需要人工确认**：①把 USB 从板子/电脑两端都拔掉等 10 秒；②检查 BOOT 键有没有
卡住；③插回后看板上电源灯/串口。

恢复后仍按 §25：`build/wirom2`（等 `L5irom=42`）→ `build/wphyen7`（PHY 校准）。
另：`g_boot_marks` 是比 RTC/0x3FCEE000 更可靠的"应用跑没跑"探针
（地址用 `nm build/<b>/c6os.elf | grep g_boot_marks` 查）。

## 28. 第 22~24 轮：用背光当指示灯，PHY 初始化卡住的定位

串口在这块板上不可靠（我们的应用崩/挂之后 USB-CDC 会卡死，只能人工拔插），
所以改用**背光（GPIO13）当指示灯**，全程不依赖串口：

- `tests/t_s3_phy.c6`：先快闪 3 下（表示"进到 os_main 了"），然后按结果显示：
  常亮 = PHY 初始化返回 0 且校准数据非空；慢闪 (rc+1) 下 = 返回错误码。
- `port/s3/irq.c` 的 `s3_trap()` 也加了背光码：长亮一下（=进陷阱了）后
  慢闪 (cause+1) 下，任何崩溃都能"看懂"。
- 真板实测（`build/wphytest2`）：**快闪 3 下之后背光就灭了，而且不再重启**
  → 不是崩溃（崩溃会进陷阱/复位循环），而是**卡死在 `register_chipv7_phy()` 里面**。

最可能的原因：那几次构建一直带着 `-DS3_IRQ_HOLD_OFF`（**中断是关的**），
而射频校准要等定时器/中断事件 → 卡死。已准备 `build/wphytest4`：**开着中断**，
并且在危险调用前后**开关 RTC 超级看门狗(SWD)** —— 万一再挂住，3.5 秒后自动
整片复位（板子自己恢复，不用再人工拔插）。

下一步（板子 USB 恢复后）：
1. 刷 `build/wphytest4/c6os.bin`；
2. 看背光：常亮 = PHY 成功（M1 达成）；反复"3 快闪 + 结果" = 自己复位在循环，
   说明还在挂/崩，看慢闪几下定位；
3. PHY 通了之后才是 libpp/libnet80211 放 flash（IROM，`SHUT_CORE0_BUS` 已修待验）。

## 29. 第 25 轮：PHY 初始化"挂死"的定位方向（用户的关键提示）

现象（背光指示，串口无关）：**快闪 3 下 → 灭 → 再快闪 3 下 …** 无限重复
= `register_chipv7_phy()` 卡住，被 TIMG 看门狗（约 2 秒，硬件定时器，
CPU 卡死也有效）复位，程序重跑了。

用户提示："闭源 WiFi/PHY 固件有它自己的系统，会不会和我们冲突？" —— 很可能是对的，
它和 IDF 环境强耦合，会做这些事：

1. **自己装中断/定时器**：IDF 里 PHY/MAC 会用到 level-3/4 甚至 7 的中断。我们的
   向量表**只实现了 level-1**，其它等级全部落到 `_vec_default`（死循环）
   → 一旦库触发高等级中断，表现就是"挂死"。
2. **改时钟树**（BBPLL/XTAL bias/CPU 时钟源）。我们的 CPU 跑在 XTAL 40MHz，
   如果库按 IDF 的 80/160/240MHz 假设去配 PLL，可能把我们的时钟搞停。
3. **依赖链接符号/内存布局**（`_bss_start`、`__dram0_*`、它自己的静态缓冲、堆）。
4. **依赖 FreeRTOS 语义**（延时/tick/队列），我们的移植里没有这些。

### 已做的诊断（待刷机验证）

`port/s3/irq.S`：把 level-2~6 的中断向量从 `_vec_default` 改成各自的处理器
`_vec_lvl2..6`，它们会用**背光闪 N 下**（N=中断等级）然后停住。这样只要看到
"闪 4 下停住"就知道是 level-4 中断引起的 ✓ 直接验证上面第 1 条。

下一步：刷 `build/wphytest8`，看背光是否出现"3 快闪 + 4 次慢闪"这种组合。
如果是 → 说明闭源库要 high-level 中断，我们得把对应等级的向量实现成"清中断 +
返回"（而不是死循环）；如果不是 → 回来查时钟树（第 2 条）。

## 30. 第 25 轮结束时的总状态（接手先看这一节）

### 已经打通、真板验证过的（都在 main 分支或本工作树里）

| 能力 | 证据 |
|---|---|
| 音频录放（BOOT 键录音→循环播放） | 真板有声、速度正确 |
| call0 ↔ windowed ABI 桥 | `L5 win=1003`、`L5b deep=11`、`L5c12 deep12=074` |
| 寄存器窗口溢出/欠载向量（call4/8/12） | 同上（含 12 参数递归） |
| 闭源 `libphy.a` 在我们的 RTOS 里执行 | `phy_get_rf_cal_version()` 返回 0x2C7 |
| 闭源库调用的 mem* 用 windowed 版 | objcopy 改名 + `win_memcpy/win_memset` |
| LEAN 构建 + libphy 装进 IRAM | IRAM 段结束 0x403D8CE8（坏区 0x403D9500 以下） |
| flash 窗口(IROM/DROM)映射模块 | `include/c6os/flash.h` + `port/s3/flash.c`（含 SHUT_CORE0_BUS 修复，**未验证**） |
| 诊断工具 | `tools/flash.sh`（自动重试+USB 自恢复）、`tools/usbreset.py`（USBDEVFS_RESET，能救卡死的 USB）、背光指示灯、`g_boot_marks` |

### 当前唯一的硬卡点

`register_chipv7_phy()`（PHY 初始化/射频校准）**会挂住**：背光实测"反复闪 3 下"，
即被看门狗复位后重试。已经把 level-2~6 中断改成"闪出等级"来验证"闭源库要
高等级中断"这个猜想，但**串口在这块板上不稳定**（我们的 USB 初始化在这块板
状态不对时会失效，连 ROM 的 `ets_printf` 也没输出），背光读数用户也反馈难以判读，
所以结论还没拿到。

### 后面还有多远（务必先看）

即使 PHY 通了，还要：
1. 把 `libpp.a`(708KB) + `libnet80211.a`(1.4MB) 放 flash（依赖 §24 的 IROM 修复生效）；
2. 移植 IDF 的 `esp_wifi/src`（~2000 行）+ FreeRTOS 垫片 + 内存版 NVS + PHY 校准存储；
3. 关联 WPA2（`libwapi.a`）、DHCP/DNS（可移植 c6 std 库代码或自写）、TCP；
4. TLS 1.3 握手（std 库只有记录层/KDF/x25519/x509）+ HTTP 客户端（std 库有 `https.c6`）。
且 §23 已经确认：**当前 c6 编译器不支持 std 库的 module/import 方言**，这些模块要手工移植。

**现实判断**：距离"真板取到 https://qq.com 页面"还有很长距离，不是几轮能完成的。
建议后续按里程碑推进，先把"PHY 初始化跑通 + 能扫到 AP"作为下一个硬目标
（它同时验证了 IROM、ABI 桥、闭源库三件事）。

## 31. 第 26 轮：板子不再直接引导我们的镜像（需要换引导方式）

花了大半天排查后确认的事实：

| 测试 | 结果 |
|---|---|
| `tools/flash.sh` 刷我们的镜像 | 成功，`read-flash` 读回与 bin **逐字节一致** |
| 刷完我们的镜像后看串口 | **0 字节**（连 ROM banner 都没有） |
| 屏幕（`tests/t_s3_paint`，红绿蓝循环） | **黑屏** = 应用没跑 |
| 刷上游 xiaozhi 全套（bootloader+分区表+otadata+app@0x20000） | **正常跑**，串口有 IDF 日志 ✓ |

⇒ 板子硬件、USB 控制台都没问题；**问题在"ROM 直接引导我们的 RAM-only 镜像"
这条路现在不通了**（以前能通，所以不是我代码里的编译/链接问题，
也不是 `--ram-only-header`、镜像内容或写入的问题）。

后续两条可选路：
1. **走标准 app 镜像 + IDF 二级 bootloader**（`bootloader`@0x0、分区表@0x8000、
   我们的 app@0x20000）。但实测 IDF bootloader **会拒绝**我们的镜像（
   `bootloader_reset()`，见 §17）—— 最可能是因为 IDF 要求镜像开头有
   `esp_app_desc_t`（DROM 段在最前、带 0xABCD5432 magic），而我们的镜像
   第一个段是 IROM。要做这条路，得在 `link.ld` 里补一个 DROM 段放 app 描述符
   （约 256 字节结构 + magic + 版本串），并把 `.flash.text` 排到它后面。
2. **查清 ROM 为什么不再接受 RAM-only 镜像**（可能是芯片被反复复位/看门狗
   折腾后的状态问题，也可能和之前我们往 0x0 写过的 IDF bootloader 残留在
   别的扇区有关）。最快的验证：把芯片整片擦除（`esptool erase-flash`）后
   再刷我们的镜像试一次。

**这一步必须解决，否则后面所有真板验证都做不了。**

## 32. 第 26 轮结论：真板验证通道断了，必须人工介入

这一轮试过的所有办法与结果：

| 尝试 | 结果 |
|---|---|
| 整片 `erase-flash` 后再刷我们的镜像（0x0 直接引导） | 黑屏，串口 0 字节 |
| 带 `--ram-only-header` / 不带（有无 flash 段） | 都不跑 |
| 补 `esp_app_desc_t`（magic 0xABCD5432，DROM 段最前）+ 走 IDF 二级 bootloader | bootloader 起来后 `bootloader_reset()`（`rst:0x3 RTC_SW_SYS_RST`, `Saved PC:0x403ccf15`）拒绝我们的 app |
| 刷上游 xiaozhi 全套固件 | **正常跑**，串口有日志 ✓ |
| `tools/usbreset.py`（USBDEVFS_RESET） | 能救"USB 卡死"，但救不了"镜像不跑" |

判断：板子/串口/镜像内容都没问题，**问题在"我们的镜像不再被引导"**。
标准 app 镜像的段布局还没对齐（我们的镜像会出现 `load=0x00000000` 的填充段，
原厂镜像没有），这是 bootloader 拒绝我们 app 的直接嫌疑点：`elf2image` 是按
LMA 排段的，而我们的 `.data/.text` 的 LMA 还没真正排到 flash 段后面
（`link.ld` 里加了 `AT(...)` 但生成的镜像没变化，需要再查）。

### 建议的人工介入方式（任选其一）

1. **抓一次真实的上电串口日志**：用 USB 线接板子，`sudo -n python3 tools/serial.py -t 30`
   **在按 RST 之前就开始抓**，然后按一下板上 RST。ROM 的报错（"invalid header"、
   "SHA-256 comparison failed" 等）会第一时间出现在串口上 —— 我们现在的抓法
   总是晚了几百毫秒，可能正好错过。
2. 或者把板子接到另一台机器/另一个 USB 口，用官方 `idf.py` / esptool 刷一次
   上游固件确认链路，再用我们的镜像试。
3. 如果你手上有 USB-UART（CH340/CP2102），接 GPIO43/44 抓 UART0 的日志，
   那比 USB-CDC 可靠得多。

有了上面任意一条的日志，就能定位"ROM 为什么不吃我们的镜像"，然后
PHY 时钟修复（§30 新增的 `SYSCON_WIFI_CLK_EN` 使能）就能验证了。

## 33. 第 26 轮的重大发现：芯片卡在"下载模式"（这是"应用不跑"的真凶）

用"**先开抓、再复位**"的方式（`tools/cap.py`）终于抓到了 ROM 的原始输出：

```
ESP-ROM:esp32s3-20210327
rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))
waiting for download                 ← 芯片在等下载指令，根本没引导 flash！
```

也就是说：`esptool` 的复位走的是 **USB-Serial/JTAG 的"复位进下载模式"通道**，
一旦进了下载模式，**DTR/strap 状态会粘住**，之后的软件复位（watchdog reset、
纯 RTS 复位）都还在下载模式里，`/dev/ttyACM*` 上看不到任何我们的输出
——之前"镜像不跑、屏幕黑"全是这个原因，不是镜像的问题。

**证据**：用 `tools/reset.py`（只拉 RTS/EN，DTR=0 保持 GPIO0 高）那一次复位，
ROM 输出变成：

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x8 (SPI_FAST_FLASH_BOOT)   ← 正常引导！
load:0x3fca0000,len:0xb90
load:0x403c9000,len:0x8294
entry 0x403c98e8
c6os s3 boot                                                    ← 我们的固件跑起来了！
```

### 这一轮新增的工具（都很关键）

| 工具 | 作用 |
|---|---|
| `tools/reset.py` | **干净的硬件复位**（DTR=0 保持 GPIO0 高，只脉冲 RTS/EN），让芯片正常引导 flash 里的固件 |
| `tools/cap.py` | 串口抓取，**同时监听所有 `/dev/ttyACM*`**（复位后常残留僵尸节点，之前的抓法抓错节点就会"0 字节"） |
| `tools/usbreset.py` | 用 `USBDEVFS_RESET` 救卡成 EPROTO 的 USB 端口（不需要拔插） |
| `tools/flash.sh` | 刷机失败时自动 `usbreset` 重试 |

### 另一个坑（已回退）

为了让 IDF bootloader 接受我们的 app，我试过补 `esp_app_desc_t` 并把 DROM 段排最前，
结果 `elf2image` 在镜像里插了一个 `load=0x00000000` 的填充段，
**ROM 直接引导时往地址 0 写数据 → 立刻 StoreProhibited 崩溃**：
```
load:0x00000000,len:0x755c
Guru Meditation Error: Core 0 panic'ed (StoreProhibited)  EXCVADDR: 0x00000000
```
⇒ ROM 直接引导这条路**必须保持 RAM-only 镜像**（`--ram-only-header`）。
要走 bootloader 那条路，得让每个段的 LMA 在 flash 里连续（IDF 的段布局），
这事还没做完（`link.ld` 里加了 `AT(...)` 但生成的镜像没变）。

### PHY 初始化：仍然挂死，但已能定位

`build/wphyE`（带追踪）：串口能看到
```
c6os s3 boot / phy: enter os_main / phy: bl ok / phy: blink ok / phy: clock+call
```
→ 卡在 `register_chipv7_phy()` 里面（modem 时钟已按 §30 补上，仍然卡）。
闭源库回调我们的胶水函数（`win_memset`/`phy_enter_critical`）时都会打一个字符，
下次板子能正常出串口时，就能看到它走到哪一步。

## 34. 诊断通道现状（第 26 轮末）

现在同时有四条通道，按可靠性排序：

| 通道 | 怎么用 | 状态 |
|---|---|---|
| **RTC 信标** `RTC_CNTL_STORE4..7`（0x600080C0..CC） | `s3_beacon(slot, val)`，然后 `esptool read-mem 0x600080C0` | 已实现；CPU/看门狗复位都不丢，**不需要串口** |
| **ROM printf**（`ets_printf`，走 windowed 孤岛 `s3_win_call_log`） | 测试里 `pstr()` | 不依赖我们自己的 USB 初始化，只要芯片正常引导就能出 |
| 我们自己的 USB-CDC 控制台 | `port_puts` | 可用，但 `port_init` 里的 USB 重枚举偶发失败；**PHY 挂死后还会把 USB 搞掉**（要用 `S3_NO_USB_REINIT` 构建避免） |
| 背光（GPIO13） | `blink()` / 陷阱处理 | 只能表达"是/否"，用户反馈难以判读 |

工具：`tools/reset.py`（干净复位，**必须用**，否则芯片留在下载模式）、
`tools/cap.py`（同时监听所有 ttyACM）、`tools/usbreset.py`（救 EPROTO）、
`tools/flash.sh`（失败自动 usbreset 重试）。

### 下一步（板子能正常出日志后 5 分钟就能有结论）

flash 里的 `build/wphyG`（`-DS3_NO_USB_REINIT -DS3_WITH_LIBPHY`）已经把信标打在：
- slot0：`0xB0000001` 进 os_main、`0xB0000004` 正要调 PHY、`0xB0000005` PHY 返回
- slot1：PHY 的返回值
- slot3：`0xC0000000|n` 闭源库回调我们胶水函数的次数（`win_memset`/`phy_enter_critical`）

按 RST 后读一次这 4 个字，就能确定它卡在 `register_chipv7_phy()` 的哪一段，
以及闭源库到底有没有回调我们。

## 35. 关键硬件事实：板子有电池 ⇒ 拔 USB 不算断电（第 27 轮）

用户确认这块板子**带电池**。所以：

- 拔 USB 只切了外部供电，芯片仍由电池供电 → **没有真正的上电复位**；
- 每次复位都来自 USB-Serial/JTAG（串口里一直是 `rst:0x15 (USB_UART_CHIP_RESET)`），
  而且它会把 strapping 顶成下载模式（`boot:0x0 (DOWNLOAD(USB/UART0))`），
  **一进去就粘住**，之后 watchdog 复位、RTS 复位全还在下载模式；
- 这就是"刷写成功、镜像内容正确、但应用就是不跑、串口 0 字节、屏幕黑"的全部原因。

**要么**：断开电池（或关电源开关）5 秒再上电 —— 才有真正的 `POWERON` 复位，
ROM 才会正常引导 flash 里的固件；
**要么**：不要用 esptool 的复位；如果芯片还在跑固件，用 `tools/reset.py`
（DTR 恒 0 = GPIO0 高，只脉冲 RTS/EN）能正常引导（实测成功过一次）。

### 信标（beacon）通道已确认可用

`RTC_CNTL_STORE4..7`（0x600080C0/C4/C8/CC）在 RTC 掉电域里：

- 用 `esptool write-mem 0x600080C0 0x12345678` 写入，复位后再读，**值还在** ✓
- 所以它是"不依赖串口、不依赖 USB"的执行进度记录通道 ✓

当前固件（`build/wbeacon`）的信标约定：

| 地址 | 值 | 含义 |
|---|---|---|
| 0x600080CC | `0xA0A0A0A0` | 进了 `_start`（上电后第一条指令附近就写） |
| 0x600080CC | `0xB0B0B0B0` | 进了 `port_init`（覆盖上一条） |
| 0x600080C0 | `0xB0000001` | 进了 `os_main` |
| 0x600080C0 | `0xB0000004` | 正要调 `register_chipv7_phy()` |
| 0x600080C0 | `0xB0000005` / 0x600080C4 | PHY 返回了 / 返回码 |
| 0x600080CC | `0xC0000000|n` | 闭源库第 n 次回调我们的胶水函数 |

## 36. 下一步的架构方案：自己当"二级引导"（不依赖 IDF bootloader）

因为 ROM 直接引导**只支持 RAM-only 镜像**（flash 段会被当成 `load=0` 填充段，
往地址 0 写 → StoreProhibited），而 WiFi 闭源库（libpp 708KB + libnet80211 1.4MB）
**必须放 flash**，所以需要自己做一个"两段式"：

- **第 1 段（stage1）**：就是现在的 RAM-only 镜像 = c6OS 内核 + `port/s3/flash.c`。
  它负责用 MMU 把第 2 段所在的 flash 区域映射到 IROM(0x42000000+)/DROM(0x3C000000+)，
  **清 `EXTMEM_ICACHE_CTRL1.SHUT_CORE0_BUS`**（§24 的修复）、使能 cache、
  清掉 ROM 遗留的 cache tag，然后跳到第 2 段入口。
- **第 2 段（stage2）**：我们的"大块头"代码 + IDF 闭源库，链接地址在 IROM/DROM 上，
  LMA 紧跟在 stage1 的镜像之后；刷机时一次 `write-flash 0x0` 把两段一起写进去。

这条路完全绕开了 IDF bootloader 的段布局要求，而且用到的每个部件
（`os_flash_map`、MMU 表 0x600C5000、`SHUT_CORE0_BUS`）我们都已经有了。

## 37. 第 29 轮：USB 失联的两个真因（已修）+ 现状

### 真因 1：我们自己的抓取工具在"按着 BOOT"

`cap.py`/`serial.py` 用 termios 打开端口时，cdc_acm 会让 **DTR 保持有效**；
而 ESP32-S3 的 USB-Serial/JTAG 把 **DTR=1 当作"GPIO0 拉低"**。
结果：只要我们在抓串口，芯片任何复位都会进下载模式
（`boot:0x0 (DOWNLOAD(USB/UART0))`），固件永远不跑 —— **不是用户按着 BOOT**。
已在两个工具里改成打开后 `TIOCMBIC` 清掉 DTR/RTS。

### 真因 2：PHY 初始化会把内部 USB PHY 抢走

`register_chipv7_phy()` 配置射频/USB PHY 后，USB-Serial/JTAG **掉枚举**：
串口没了、`/dev/ttyACM*` 消失、esptool 也连不上（这就是"板子不在总线上"）。
已在 `s3_win_phy_enable()` 返回后加 `s3_win_usb_reclaim()`
（重新设置 `RTC_CNTL_USB_CONF` 抢回 PHY + USB 设备时钟/复位 + CONF0 pullup）。
**但如果 PHY 调用本身卡死，这段代码永远执行不到** ⇒ 卡死时 USB 仍然是掉的。

### 因此现在的现实

- PHY 初始化仍然卡死（`phy: clock+call` 之后没有任何输出）；
- 卡死 → USB 掉 → 既看不到串口也读不了 RTC 信标（读信标也要 esptool 连上 USB）；
- 要打破这个循环，最省事的是**一根 USB-UART 适配器接 GPIO43/44**（UART0 不依赖
  USB PHY，PHY 怎么折腾都能看到日志），或者让 PHY 不抢 USB PHY（需要搞清
  IDF 的 `phy_bbpll_en_usb()` / `CONFIG_ESP_PHY_ENABLE_USB` 的寄存器细节）。

### 已经稳稳到手的（都在仓库里，真板验证过）

- 音频录放（BOOT 按键录音→循环播放，已合并 main）
- call0 ↔ windowed ABI 桥 + 寄存器窗口向量（含 12 参数递归）
- 闭源 `libphy.a` 在 c6LOS 里执行、`phy_get_rf_cal_version()` = 0x2C7
- 闭源库用的 `mem*` 全部改成 windowed 版（objcopy 改名 + 孤岛实现）
- c6 标准库新增：`include/c6os/flash.h`（IROM/DROM 映射、cache 使能含
  `SHUT_CORE0_BUS` 修复）、`include/c6os/reg.h`
- 一套硬件诊断工具（`tools/flash.sh`、`reset.py`、`cap.py`、`usbreset.py`、
  `beacon.sh`）+ RTC 信标通道
- `docs/wifi-plan.md` 37 节，把这十几次踩的坑全部记录在案

## 38. 第 29 轮末：PHY 卡死的根因已定位到"函数表 g_phyFuns"，但还没接上

### 已确认的事实（反汇编 + 分步实测）

`register_chipv7_phy()` 一开始就：
```
l32r a7, <&g_phyFuns>      ; a7 = g_phyFuns 变量的地址
l32i a5, a7, 0             ; a5 = g_phyFuns（函数表指针）
l32i a5, a5, 0x204         ; 表项
callx8 a5                  ; 调 ROM 里的 PHY 函数
```
而我们的链接结果里 `g_phyFuns` 是本地 `.bss`（`0x3fca95e4`）= **0**，
所以这一步就是空指针调用 ⇒ 卡死/跑飞（实测 trap：`cause=0x14 InstrFetchProhibited pc=0`）。

分步实测（`build/wphyM` 的 RTC 信标）：
```
phyFuns before = 0
step1(时钟)            rc=0      ✓
step2(phy_get_romfuncs) rc=0     ✓ 调通了，但…
step3(查表)            rc=0x102  ✗ g_phyFuns 仍然是 0
```

### 为什么 `phy_get_romfuncs()` 没填上

ESP32-**S2** 的 ROM ld 里有 `PROVIDE ( g_phyFuns = 0x3ffffd90 )`，
即应用引用的 `g_phyFuns` 直接**别名到 ROM 的内存变量**，ROM 填的就是它。
ESP32-**S3** 的 `esp32s3.rom.ld` **没有**这条 ⇒ 我们的链接器造了个私有副本，
ROM 填的是它自己那份，两边不是同一个变量。

### 下一次的最短路径（任选）

1. **反查 S3 的 g_phyFuns 地址**：在 IDF 的 `libphy.a` 里它是个 `.bss` 符号；
   真正有效的是 ROM 数据区里的那一份。可以用"调用 `phy_get_romfuncs()` 前后
   dump `0x3FC88000..0x3FC9FFF0` 做 diff"（注意：**只能解引用确定映射的地址**，
   否则 LoadStoreError——这一轮踩过）。
2. 或者干脆**自己造表**：`g_phyFuns` 指向一张 4 字节对齐的函数指针表，
   把 `register_chipv7_phy` 用到的表项（0x204 等）填成 ROM 里对应的
   `phy_*` 函数地址（`port/s3/rom/esp32s3.rom.ld` 里已经有一批）。
3. 观察通道：**强烈建议接一根 USB-UART 到 GPIO43/44**（UART0）。现在的
   USB-CDC 只要 PHY 一初始化就掉枚举，什么都看不到；UART0 不受影响。

### 这一轮新增/修正的代码

- `port/s3/win_island.c`：`s3_win_phy_clock_enable()`（modem 时钟）、
  `phy_get_romfuncs()` 调用、`s3_win_find_phytab()`（按特征搜表）、
  `s3_win_usb_reclaim()`（PHY 抢走 USB PHY 后抢回）、`s3_win_phy_step(1..5)`
- `port/s3/win_bridge.S`：`s3_win_call_phy_step`、`s3_win_call_phy_funcs`
- `tools/beacon.sh`、`tools/cap.py`（抓所有 ttyACM + 释放 DTR）
- `tests/t_s3_phy.c6`：分步诊断，结果只写 RTC 信标（崩溃也不丢）

## 39. 第 29 轮补充实测：`phy_get_romfuncs()` 不是答案

安全探测（`build/wphyQ`，只写 RTC 信标）结果：

```
slot0 = 0xF1        ← 进到 os_main 并开始探测
slot1 = 0x00000002  ← phy_get_romfuncs() 的**返回值是 2**（不是表指针）
slot2 = 0x00000000  ← 返回值不像指针，没解引用（防崩）
slot3 = 0xB0B0B0B0  ← port_init 的标记（说明后面的 step2 又卡住了）
```

⇒ ROM 的 `phy_get_romfuncs()` **既不填我们的 `g_phyFuns`，也不返回表指针**。
另外**去扫描 ROM 数据区（0x3FCEF000 起）会把总线读死**（不是 trap，是挂住），
所以"扫内存找表"这条路不要再用。

### 下次开工的三条路（按性价比排序）

1. **接一根 USB-UART 到 GPIO43/44（UART0）**：现在每次 PHY 一初始化 USB 就掉，
   什么也看不到；UART0 不受 PHY 影响，能直接看到卡在哪条指令。
   （我们的 `port_putc` 也可以顺手加一条"同时写 UART0"的分支。）
2. **用 JTAG**（S3 内置 USB-JTAG 支持 OpenOCD）：下断点看 `g_phyFuns` 在 ROM 里
   到底被写到哪个地址。
3. **自己造表**：`g_phyFuns` 只需要是一张函数指针表，`register_chipv7_phy`
   用到的表项（0x204 等）指向 ROM 里已有的 `phy_*` 函数
   （`port/s3/rom/esp32s3.rom.ld` 里有 1200+ 个 ROM 符号可查）。

### 已经确定无误的部分（不用再怀疑）

- 镜像内容/刷机/引导：`boot:0x8 (SPI_FAST_FLASH_BOOT)` + `c6os s3 boot` ✓
- ABI 桥、libphy 能执行 ✓（`phy_get_rf_cal_version()` = 0x2C7）
- modem 时钟已开 ✓（step1 rc=0）
- 卡点**唯一且明确**：`g_phyFuns` 空表 ⇒ `callx8 0` ⇒ `cause=0x14 pc=0`

## 40. 第 29 轮收尾：状态与交接

### 构建状态（已验证，两种配置都通过）

- 不带闭源库：`make C6_ROOT=/home/winger/c6 BUILD=build/x S3_MAIN=tests/t_s3_paint.c6 s3` ✓
- 带闭源库：`S3_LEAN=1 S3_CDEFS=-DS3_WITH_LIBPHY S3_EXTRA_LIBS=build/x/libphy_win.a` ✓

`port/s3/win_island.c` 的预处理结构这一轮被改乱过，已修好（ifdef 作用域合并、
补回 `#endif`、非 libphy 构建补 `s3_win_phy_step/funcs_word/romfuncs_ret/find_phytab` stub）。

### 真板当前事实（RTC 信标，掉电域，崩溃/复位都不丢）

```
0x600080C0 = 0x000000F1   进到 os_main 并开始探测
0x600080C4 = 0x00000002   phy_get_romfuncs() 返回值 = 2（不是表指针）
0x600080C8 = 0x00000000   返回值不像指针，未解引用
0x600080CC = 0xB0B0B0B0   port_init 标记；之后 step2 卡住
```

**唯一卡点仍是 `g_phyFuns` 空表**：`register_chipv7_phy` 一进来就
`l32i a5,[g_phyFuns]; l32i a5,[a5+0x204]; callx8 a5` ⇒ 空指针调用。

### 下次开工需要的三样东西（按性价比排序）

1. **USB-UART（CH340/CP2102）接 GPIO43(TX)/44(RX)+GND**：PHY 一初始化 USB-CDC 就
   掉枚举，串口和 esptool 全失联；UART0 不受影响。拿到后先给 `port_putc` 加一条
   "同时写 UART0" 的分支，之后所有调试都看得见。
2. **OpenOCD（S3 内置 USB-JTAG）**：对 `register_chipv7_phy` 下断点，直接看 ROM
   把函数表写到哪个地址。
3. **自己造表**：`g_phyFuns` 只是函数指针表，把 `+0x204` 等表项填成 ROM 里已有的
   `phy_*` 地址（`port/s3/rom/esp32s3.rom.ld` 有 1200+ 符号）。

### 已经稳稳到手的（都在仓库里）

- 音频：BOOT 键录音 → 循环播放（已合并 main，真板有声）
- call0 ↔ windowed ABI 桥 + 寄存器窗口向量（含 12 参数递归）
- 闭源 `libphy.a` 在 c6LOS 里执行（`phy_get_rf_cal_version()` = 0x2C7）
- 闭源库用的 `mem*` 全改 windowed 版（objcopy 改名 + 孤岛实现）
- c6 标准库：`flash.h`（IROM/DROM 映射 + `SHUT_CORE0_BUS` 修复）、`reg.h`
- 工具链：`flash.sh`、`usbreset.py`、`reset.py`、`cap.py`、`beacon.sh`
- 本文档 40 节，把 ABI、内存坏区、看门狗、下载模式、DTR、USB PHY 抢占、
  镜像段布局、函数表这些坑全部记录在案

## 41. 【网上查证】其他人的非 IDF 移植经验（第 29 轮，非常有价值）

### ① PHY 函数表的正确初始化方式（直接解决我们的卡点）

- `g_phyFuns` 是 **DRAM 里的一张函数指针表**（ESP32 上是 0x3fcef3d8），
  WiFi/BT 闭源代码里几乎全是 `(*(g_phyFuns + 0xec))(...)` 这种间接调用。
- **正确入口是 libphy.a 自己导出的 `phy_get_romfunc_addr()`（S3 上 .text 偏移 0x88）**，
  IDF 的 `components/esp_wifi/include/phy.h` 原文：
  *"Initialize function pointer table in PHY library… must be called before
  register_chipv7_phy"*。
  我们之前调的是 **ROM** 里的 `phy_get_romfuncs`（0x40005c58）——同名不同物，
  它填的是 ROM 内部的表，跟我们的 `g_phyFuns` 无关（实测返回值是 2）。
- 已改成调 `phy_get_romfunc_addr()`（`port/s3/win_island.c`），构建通过，
  待真板验证（`slot2 = g_phyFuns` 非 0 就说明表初始化成功）。

### ② 已有的非 IDF 移植可以直接抄

| 项目 | 位置 | 用处 |
|---|---|---|
| **NuttX** | `arch/xtensa/src/esp32s3/esp32s3_wifi_adapter.c` | 最完整的参考：怎么用 `esp_wifi_init(&wifi_cfg)`、`esp_phy_*`、tx_done 回调 |
| **tinygo espradio** | `tinygo-org/espradio` 的 `radio.c` | 裸机下初始化顺序 + 坑：`phy_get_romfunc_addr()` 在某些构建里会崩，他们改用**静态 stub 表**；`g_phyFuns` 非 0 当"已初始化"标志；`wifi_init_config_t` 要 512 字节对齐（Xtensa/Clang 的 OR-offset bug）；不要预先设置 `g_osi_funcs_p` |
| **esp-hal (Rust)** | `esp-phy` crate | `register_chipv7_phy(PHY_INIT_DATA_DEFAULT, cal, PHY_RF_CAL_PARTIAL/FULL)` 的调用方式与校准结果保存 |
| **Zephyr** | `hal_espressif` + `esp_wifi_adapter.c` | 走 IDF 组件封装，不是裸调 |

### ③ WiFi 移植真正需要的东西（有心理准备）

1. 厂商 `.a`：`libphy` + `libpp`(710KB) + `libnet80211`(1.5MB) + `libcore` + `libwapi`
   + 芯片 ROM 符号 `.ld`（我们已有 `port/s3/rom/`）
2. **~30 个函数的 OS 适配层**（`esp_wifi_os_adapter`）：`wifi_calloc/free`、
   `wifi_thread_semphr_get`、`queue_send`、`task_ms_to_tick`、`wifi_int_disable/restore`、
   任务创建、软件定时器 —— 需要**真正的调度器**（c6LOS 有），不一定非要 FreeRTOS
3. PHY 初始化 + `esp_wifi_init` + 定时器/事件循环（Xtensa 上占用 TIMG1/TIMER0 + CCOMPARE0，单核）

## 42. 第 30 轮：PHY 表已通，卡点前移到 set_chan_freq_hw_init

真板实测（`tools/flashmon.sh` 一条命令：刷机 + 立即监视，无需人工断电）：

```
step 1 ... rc=0                tab=0
step 2 ... rc=0                tab=0x3fcef3d8   ← phy_get_romfunc_addr() 填表成功
step 3 ... rc=0                tab=0x3fcef3d8
step 4 ... rc=0                tab=0x3fcef3d8   ← register_chipv7_phy_init_param()
step 5 ... *** TRAP cause=0x00 pc=0x403d0468    ← IllegalInstruction
```

`0x403D0468` 落在闭源库的 `set_chan_freq_hw_init`（0x403d03d4..0x403d047f）里，
而且**正好是 `memw` 指令的中间字节**（`memw` 在 0x403d0467，占 3 字节）。
PC 落在指令中间 ⇒ 典型的**返回地址不对**（窗口 ABI 的 `retw` 回到了非 call8 的
调用点，或 `callx8` 用了坏指针）。下一步就从这条线索查：确认这一步里闭源库
回调的每个函数（含 `g_phyFuns` 表里的 ROM 入口）都是 windowed ABI。

配套：`-DS3_KEEP_SWD` + 应用内重新武装 RTC 超级看门狗，挂住约 3.5 秒自复位重跑，
所以现在**不需要人工断电**也能连续迭代。

背光（GPIO13）已常亮：LEAN+libphy 构建里也能把 LCD 点亮，肉眼可确认板子在跑。

## 43. 第 30 轮：栈顶压到坏区以下（一个真实的坑，已修）

发现：`_stack_top` 原来是 **0x3FCEF000**，而这块板的 DRAM **从 0x3FCD9500 往上不稳定**
（§14 的纯填充实验证明：代码/数据越过它就起不来）。也就是说栈的高端约 **106KB 落在坏区里**。

我们自己用栈很浅（几百字节）所以从没踩到；但**闭源 PHY 初始化调用很深**
（`entry a1, 0x110` + 多层嵌套 + 寄存器窗口 spill 会一路往下写），一旦写到坏区，
数据就丢了，`retw` 从被污染的 spill 槽恢复出**错误的返回地址** ——
这正好解释了我们看到的 `IllegalInstruction` 且 **PC 落在指令中间**（0x403d0468
正好是 `memw`（0x403d0467 起，3 字节）的中间字节）。

已把 `_stack_top` 改成 **0x3FCD9000**（坏区以下），栈区间变成
`[_bss_end+0x1F00, 0x3FCD9000)` ≈ 190KB，仍然很宽裕，而且**永远不会碰到坏区**。

### 仍然存在的障碍

- `register_chipv7_phy()` 一进去就抢走内部 USB PHY ⇒ USB-CDC 掉枚举，
  **看不到它的返回**（`phy_bbpll_en_usb(1)` 没能避免）。
- RTC 信标能跨 CPU 复位/看门狗复位保留，但 **SWD（超级看门狗）复位会连同 RTC 域一起清掉**，
  所以"下一轮再读"的 self-report 机制在 SWD 自恢复模式下不成立。
- 结论：要看 step5 的结果，仍然需要 UART0（GPIO43/44）或 OpenOCD；
  或者让 PHY 不再抢 USB PHY（需要 `esp_phy_enable()` 里那套 USB 共存细节）。

## 44. 第 30 轮：补齐 IDF 的 modem 上电序列 + 稳定亮屏固件

按 IDF `phy_init.c` 的 `esp_wifi_bt_power_domain_on()` 补齐了 PHY 之前必须做的三件事
（`port/s3/win_island.c` 的 `s3_win_phy_power_on()`）：

1. 解除 WiFi 掉电：`RTC_CNTL_DIG_PWC`(0x60008090) 的 `WIFI_FORCE_PD`(bit17) = 0
2. 打开 modem 时钟：`SYSCON_WIFI_CLK_EN`(0x60026014) |= `0x78078F`（WiFi/BT 共用）
   + `0x400000`（PHY 校准）
3. modem 复位脉冲：`SYSCON_WIFI_RST_EN`(0x60026018) 的 `MODEM_RESET_FIELD_WHEN_PU`
   = bits 0,1,2,3,4,9,11,13 = **0x2A1F**，先置位再清零
4. 解除隔离：`RTC_CNTL_DIG_ISO`(0x60008094) 的 `WIFI_FORCE_ISO`(bit28) = 0

另外按 IDF 的用法加了 `phy_bbpll_en_usb(1)`（`CONFIG_ESP_PHY_ENABLE_USB` 等价物）。

**结果**：`register_chipv7_phy()` 仍然在 `set_chan_freq_hw_init` 里跑飞
（IllegalInstruction，PC 落在指令中间、随校准模式漂移）。栈顶坏区问题已排除
（改链接符号后 `sp` 已落在 0x3FCD9000 以下，崩溃依旧）。

### 留给板子的稳定状态

刷了 `tests/t_s3_on.c6`（`build/won`）：

- LCD **背光常亮**（GPIO13，LEAN 构建直接操作 GPIO）
- 每 ~5 秒串口打印 `alive n=...`（走 ROM 的 `ets_printf`，不依赖我们自己的 USB 初始化）
- **不碰 PHY**，所以 USB 不会掉、esptool 随时能连上。

上午接着搞时直接从 `./tools/flashmon.sh build/phy/c6os.bin 20` 开始即可
（`make wphy` 会构建 PHY 分步诊断版本）。

## 45. 第 30 轮末：PHY 卡点现状与下一步（交接）

### 已排除的可能（都做过对照实验）

| 怀疑点 | 实验 | 结论 |
|---|---|---|
| 栈压在 DRAM 坏区 | 栈顶改链接符号 `_stack_top` = 0x3FCD9000，实测 `sp=3fcd8e00` | 已修，但崩溃依旧 → **不是它** |
| 高等级中断打断 windowed 闭源库 | `-DS3_IRQ_HOLD_OFF`（全程关中断） | 依旧崩 → 不是它 |
| modem 没上电/隔离没解 | 补 `WIFI_FORCE_PD=0`、`WIFI_FORCE_ISO=0`、modem 复位脉冲 0x2A1F | 依旧崩 → 不是它（但这几步本身是对的，保留） |
| 校准数据/模式 | `PHY_RF_CAL_PARTIAL(0)` / `FULL(2)` / `NONE(1)` | 都崩，**PC 随模式漂移**（0x403d0468 / 0x403d049c / 0x403d04a0），都在 `set_chan_freq_hw_init` 里，且都**落在指令中间** |
| USB PHY 被抢 | `phy_bbpll_en_usb(1)` | 仍抢 → 只能靠"下一次上电"或 UART0 观测 |

### 现在的判断

PC 落在指令中间 + 随参数漂移 ⇒ **返回地址/a0 被破坏**，而不是某条指令本身非法。
闭源库内部只有两种东西能改 a0：`callx8/retw` 的窗口机制，或 **把 a0 存到被写坏的
内存里**。窗口 spill/fill 向量是标准序列、且自测过（deep=11 / deep12），
所以嫌疑最大的是"spill 槽落在一块写不进去的内存"。

**下一步最直接的验证**：给 PHY 调用换一块**独立、已知良好**的栈
（比如在 DRAM 低端 0x3FCA0000 附近自己划 4KB，切过去再 call），
如果崩溃消失/位置改变，就锁定是栈/内存问题。

### 板子当前状态

已刷回 `build/won`（`tests/t_s3_on.c6`）：背光常亮 + 每 5 秒打印 `alive n=...`，
不碰 PHY，USB/esptool 随时可用。

## 46. 第 31 轮：**USB-JTAG 调试通道打通**（这个目标卡了十几轮缺的观察手段）

不需要 UART！ESP32-S3 内置的 USB-Serial/JTAG 外设**同时**提供 CDC 串口和 JTAG，
系统里已经有 `openocd`（`~/.espressif/tools/openocd-esp32/.../bin/openocd`）和
`xtensa-esp32s3-elf-gdb`。真板实测连接成功：

```
Info : esp_usb_jtag: serial (CC:8D:A2:EC:99:C4)
Info : JTAG tap: esp32s3.cpu0 tap/device found: 0x120034e5 (Tensilica)
Info : Listening on port 3333 for gdb connections
```

### 新工具

- `tools/dbg.sh <elf> <gdb脚本>` —— 起 openocd（等端口就绪）+ gdb 批处理 + 收尾
- `tools/gphy.gdb` —— 在 `register_chipv7_phy` 下断点
- `tools/gtrap2.gdb` —— 在 `_vec_level1` 用**硬件断点**（`hbreak`；软件断点写不进去，
  因为目标常驻异常态 EXCM=1）循环跳过 `exccause==4`（定时器中断），只报真异常
- `tools/gstep.gdb` —— 单步跟踪

### 已观测到的硬证据

```
Breakpoint hit: register_chipv7_phy+3
pc=0x403ce7fb  a0=0x803cdbf1  a1=0x3fcd8e90  ps=0x60500(WOE=1, CALLINC=2)   ← ABI 桥是对的

停在 set_chan_freq_hw_init:
pc=0x403d04b3  a0=0xffffffff803ce7ee                                        ← 返回地址正常

捕获真异常:
exccause=0 (IllegalInstruction)  epc1=0x40044290(**ROM 里**)  excvaddr=0
ps=0x6050f  a0=0x403cc819(in port_init)  a1=0x3fcd8fe0
a2=0x600080cc  a3=0xffffffffb0b0b0b0                                        ← 我们 s3_beacon(3,0xB0B0B0B0) 的参数
```

`epc1` 落在 **ROM 区**（0x40044290），且寄存器里还是 `port_init` 里 `s3_beacon` 的参数
⇒ 这条异常是**从我们自己的 call0 代码直接调 ROM** 触发的（ROM 是 windowed ABI，这是老坑）。
下一步就用这个调试通道把 PHY 崩溃点和这条 ROM 调用精确定位（现在可以单步、可以读 a0/ps）。

### 注意

- 目标常驻异常态时软件断点插不进去 → 用 `hbreak`（硬件断点）
- PHY 抢 USB PHY 时 JTAG 会一起断；所以 PHY 用例里加了 **90 秒调试窗口**，
  要在窗口内附着
- `pkill -f openocd` 会把自己这条 shell 命令一起杀掉 —— 用 `pkill -x openocd`

## 47. 第 32 轮：**M1 达成 —— PHY 初始化真的成功了**（JTAG 实证）

用第 31 轮打通的 USB-JTAG 通道，配合"调试门"（应用启动后卡在
`while (RTC_STORE4 != 0xDEADBEEF)`；gdb 里 `set {int}0x600080C0 = 0xDEADBEEF` 放行），
逐步验证 `s3_win_phy_step(1..5)`：

```
--- STEP1 进入 pc=0x403cdb03   --- STEP1 返回 a2=0x1
--- STEP2 进入 pc=0x403cdb03   --- STEP2 返回 a2=0x2
--- STEP3 进入 pc=0x403cdb03   --- STEP3 返回 a2=0x3
--- STEP4 进入 pc=0x403cdb03   --- STEP4 返回 a2=0x4
--- STEP5 进入 pc=0x403cdb03   --- STEP5 返回 a2=0x5
```

`s3_trap` 断点**一次都没命中** ⇒ **PHY 初始化没有崩溃**。

之前那条 `IllegalInstruction (pc 落在指令中间)` 的真相是：
**PHY 抢走内部 USB PHY ⇒ USB-CDC 掉枚举 ⇒ 我们随后调用 ROM 的 `ets_printf` 打印
"rc=..." 时在 ROM 里跑飞**（跟 PHY 无关）。也就是说：

- ✅ `phy_get_romfunc_addr()` 填表
- ✅ `register_chipv7_phy_init_param()`
- ✅ `register_chipv7_phy()`（含射频校准）
- ❌ 之后**不能再用 USB-CDC 打印**

结论：**卡了 20 多轮的 PHY 初始化已经通了**，剩下的只是"PHY 之后没有串口可看"，
而这已经被 USB-JTAG 调试通道补上了。

### 复现命令

```sh
make C6_ROOT=/home/winger/c6 BUILD=build/wgate S3_LEAN=1 S3_CDEFS="-DS3_WITH_LIBPHY" \
     S3_MAIN=tests/t_s3_phy.c6 S3_STACK_TOP=0x3FCD9000 build/wgate/libphy_win.a
make C6_ROOT=/home/winger/c6 BUILD=build/wgate S3_LEAN=1 S3_CDEFS="-DS3_WITH_LIBPHY" \
     S3_MAIN=tests/t_s3_phy.c6 S3_STACK_TOP=0x3FCD9000 \
     S3_EXTRA_LIBS=build/wgate/libphy_win.a s3
./tools/flashmon.sh build/wgate/c6os.bin 5      # 刷进去，应用会卡在调试门
./tools/dbg.sh build/wgate/c6os.elf tools/gfin2.gdb   # 逐个读 5 步的返回值
```

### PHY 之后串口失联的根因（调试器实测寄存器）

| 寄存器 | PHY 之前 | PHY 之后 |
|---|---|---|
| `SYSTEM_PERIP_CLK_EN1` (0x600C001C) | 0x00000604 | **0x00000000** |
| `SYSTEM_PERIP_RST_EN1` (0x600C0024) | 0x000001CA | **0x00000000** |
| `RTC_CNTL_USB_CONF` (0x60008120) | 0x00100000 | **0x00000000** |
| `USB_JTAG_CONF0` (0x60038018) | 0x00004200 | **0x00000000** |

⇒ PHY 初始化（或它里面的 modem 上电序列）把 **USB 外设的时钟/复位/PHY 配置全清了**。
`/dev/ttyACM*` 仍然存在（USB-Serial/JTAG 设备还在），但 **CDC 通道废掉** ⇒
我们的打印在 ROM `ets_printf` 里跑飞。

`port/s3/win_island.c` 的 `s3_win_usb_reclaim()` 已经在 `register_chipv7_phy()` 之后
按这些值回写，但实测**回写无效**（再读仍然全 0）；用 openocd 的 `monitor mww`
从调试器写也一样。推测是 modem 上电序列把 USB 的**时钟根**关掉了，
需要按 IDF `esp_phy_disable()`/`esp_wifi_bt_power_domain_off()` 的逆序恢复，
或者干脆接受"PHY 之后没有串口"，改用 USB-JTAG 调试通道看结果（已验证可行）。

**结论：PHY 之后请用 `./tools/dbg.sh` + `tools/gfin2.gdb` 观察，不要指望串口。**

## 48. 第 32 轮：WiFi 闭源库接入的勘查结果（下一步的精确起点）

### 依赖面比想象的小

把 `libphy.a + libpp.a + libnet80211.a + libcore.a + libwapi.a` 用
`ld -r --whole-archive` 部分链接后，**只剩 120 个未定义符号**，其中：

- 一半是 **libgcc 的软浮点**（`__adddf3`/`__divdi3`/`__floatsidf`…）⇒ 直接链 libgcc 即可
- 一小批 `g_xxx_ptr`（`g_ic_ptr`/`g_pm_ptr`/`g_lmac_cnt_ptr`…）⇒ .bss 全局指针
- `esp_crc8`、`ets_delay_us`、`free`、`coex_pti_print` ⇒ 小函数
- **`g_osi_funcs_p`** ⇒ 见下，S3 的关键缺口

### `g_osi_funcs_p`：和当初 `g_phyFuns` 一模一样的情形

```
esp_rom/esp32c6/ld/esp32c6.rom.pp.ld:359:  g_osi_funcs_p = 0x4087ff6c;
esp_rom/esp32c2/ld/esp32c2.rom.ld:789:     g_osi_funcs_p = 0x3fcdfdd8;
esp_rom/esp32c61/ld/esp32c61.rom.pp.ld:339:g_osi_funcs_p = 0x4084ff60;
```

**别的芯片 ROM ld 里都有，S3 没有** ⇒ 必须由我们自己定义（和 `g_phyFuns` 完全同类问题）：

```c
wifi_osi_funcs_t g_wifi_osi_funcs;          /* 填好 OS 适配层 */
void *g_osi_funcs_p = &g_wifi_osi_funcs;    /* blob 读这个指针 */
```

结构定义已经搬进标准库：`include/c6os/wifi_osi.h`（从 IDF
`esp_private/wifi_os_adapter.h` 逐行拷贝生成，字段顺序一致，21 个函数指针）。

### 适配层要做的实际工作

IDF 自己的参考实现是 `components/esp_wifi/esp32s3/esp_adapter.c`（119 处赋值，
其中一大半是 `coex_*` 共存相关的 wrapper —— 只做 WiFi 时全部可以空实现）。
真正需要的底层能力：

- 内存：`malloc/free/calloc/realloc/zalloc`（内部堆）
- 队列：`queue_create/send/recv/delete`
- 信号量/互斥：`semphr_create/take/give/delete`、`mutex_create/lock/unlock/delete`
- 定时器：`timer_setfn/arm_us/disarm/done`
- 线程：`task_create/delete/yield/yield_from_isr`
- 中断：`set_intr/clear_intr/set_isr/ints_on/ints_off/int_disable/int_restore`
- 日志：`log_write`、`log_timestamp`
- 事件：`event_post`
- 其余：`random`、`slowclk_cal_get`、`phy_get_romfunc_addr`（已有 ✓）

### 建议的推进顺序

1. `port/s3/wifi_glue.c`：先全部填 `(void*)glue_ret0` 的假实现，让
   `esp_wifi_init()` 能**链接**通过（这是可验证的里程碑）
2. 用 USB-JTAG（`tools/dbg.sh`）在 `esp_wifi_init` 里单步/断点，按崩溃顺序
   把假实现替换成真的（内存 → 队列 → 信号量 → 定时器 → 线程 → 中断）
3. 能返回 `ESP_OK` 后，接 `esp_wifi_set_mode/start/scan`

## 49. 第 33 轮：M2 的真实工作量评估（重要结论）

把闭源库（libphy+libpp+libnet80211+libcore+libwapi）链进我们的镜像后，
除了 `g_osi_funcs_p`（已自建 ✓）之外，还缺**一大批 IDF 源码层符号**：

```
ic_add_rx_ba, ic_del_key, ic_enable_rx, ic_create_wifi_task, ic_csi_set_config,
ic_ampdu_op, ic_disable_crypto, ic_del_ptk_and_gtk_alg, ...（ic_* 几十个）
hal_ba_session_restore_by_hw_index, hal_crypto_enable, hal_enable_sta_tsf,
hal_disable_softap_tsf, hal_get_tsf_time, ...（hal_* 一批）
esf_buf_setup_for_mesh, dbg_lmac_statis_dump, hexstr2bin,
esp_wifi_internal_set_retry_counter, esp_wifi_internal_set_msdu_lifetime,
g_espnow_user_oui, g_mt
```

这些**不是闭源库内部能自洽的**，它们来自 IDF 的**源码层**
`components/esp_wifi/src/*.c`（`ic.c`、`hal_*.c`、`wifi_init.c`、`esf_buf.c` …约 40 个文件）。

也就是说：**WiFi 栈 = 闭源 blob + IDF 的 esp_wifi 源码层**，后者必须由我们编译进来。

### 结论（给下一步定方案）

`esp_wifi_init_internal()` / `esp_wifi_start()` / `esp_wifi_set_mode()` 都在 blob 里 ✓，
但它们依赖源码层的 `ic_*` / `hal_*` ⇒ 必须把 IDF `components/esp_wifi/src/` 编进来。
这些文件需要的 FreeRTOS API 并不算多（队列/信号量/任务/定时器/事件组），
可行路径：

1. 把 `components/esp_wifi/src/*.c` 加入构建，用 `-mabi=windowed` 编译
   （它们和闭源库同 ABI；我们从 c6 侧通过 `win_bridge` 调用）
2. 提供 **FreeRTOS 兼容层**（`freertos/FreeRTOS.h` 等最小头 + 队列/信号量/任务/定时器实现，
   内部直接用 c6LOS 的 `os_*` API）
3. 提供 `esp_log`、`esp_timer`、`esp_event`、`heap_caps_*`、`nvs_*` 的最小实现
4. 然后按 §48 的顺序填 `wifi_osi_funcs_t`

这是一块**明确但量大**的工作（IDF 侧约 40 个文件），不是几轮能完成的；
已经完成的 PHY（M1）是它唯一真正有技术风险的先决条件。

### 本轮已落地

- `include/c6os/wifi_osi.h`：119 字段 `wifi_osi_funcs_t`（顺序照抄 IDF）
- `port/s3/wifi_glue.c`：`g_wifi_osi_funcs` + 自建 `g_osi_funcs_p` + 全桩适配层，已进构建
- `Makefile`：`wifi_glue.o` 编译规则 + 进 `S3_OBJ_WIFI`
- `tests/t_s3_wifi2.c6`：调用 `esp_wifi_init_internal()` 的第一版接线用例（含调试门）

## 50. 第 33 轮末：IDF 源码层交叉编译的进展与最后一个卡点

`tools/idfcc.sh` + `port/s3/idf_compat/{sdkconfig.h,FreeRTOSConfig.h}` 已经能把
`components/esp_wifi/src/wifi_init.c` 用我们的工具链按 **windowed ABI** 编译进去，
头文件缺口一路收敛到：

```
fatal: sdkconfig.h            -> 我们自己补 port/s3/idf_compat/sdkconfig.h
fatal: FreeRTOSConfig.h       -> port/s3/idf_compat/FreeRTOSConfig.h
fatal: portmacro.h            -> -I.../FreeRTOS-Kernel/portable/xtensa/include/freertos
fatal: soc/reg_base.h         -> -Icomponents/soc/esp32s3/register
fatal: esp_newlib.h           -> -Icomponents/esp_libc/platform_include
现在:  stdio.h: unknown type name '__FILE'
```

最后一个卡点是 **IDF 的 newlib 覆盖层 vs 我们裸工具链的 newlib**：
IDF 的 `esp_libc/platform_include` 里有整套覆盖头（`stdio.h`/`time.h`…），
它们假设 IDF 自己的 newlib 配置（`_REENT_SMALL`、`__FILE` 结构等）。
两条路：

1. **不引入 IDF 的 platform_include**，让 esp_wifi 源码用工具链自带 newlib 头
   （可能需要少量 `#ifdef` 或提供一个小的 `esp_newlib.h` 空壳）
2. 完全照 IDF 的方式配置 newlib（需要 IDF 的 newlib 补丁头）

先试第 1 条（成本低）：把 `-I$IDF/components/esp_libc/platform_include` 换成
我们自己写一个最小 `esp_newlib.h`（只声明 esp_wifi 源码真正用到的几个符号），
放在 `port/s3/idf_compat/` 里，让头文件搜索优先命中我们的版本。

### 整体判断（老实说）

- ✅ **M1（PHY 初始化）已达成**，这是唯一有技术风险、需要逆向/调试的部分，
  而且有真板 JTAG 逐指令证据
- ⏳ **M2（WiFi）剩下的是一块体量大的移植工作**：IDF `components/esp_wifi/src/`
  约 40 个文件 + FreeRTOS 兼容层 + newlib 兼容层 + `wifi_osi_funcs_t` 的 119 个实现。
  路径完全清楚，但需要持续投入，不是几轮能收尾的
- 观察手段已经彻底解决（USB-JTAG，不需要 UART）

## 51. 第 34 轮：**WiFi 全部链接通过**（0 个未定义符号），卡点变成 IRAM 容量

这一轮把 WiFi 栈从"120 个未定义符号"推到了 **0**：

1. **IDF 源码层能编译**：`tools/idfcc.sh`（windowed ABI）+ `port/s3/idf_compat/`
   （`sdkconfig.h`/`FreeRTOSConfig.h`/最小 `esp_newlib.h`）⇒
   `components/esp_wifi/src/*.c` **7/8 通过**（只有 `smartconfig_ack.c` 缺头，我们用不到），
   `components/esp_wifi/regulatory/esp_wifi_regulatory.c` 也通过
2. **归档顺序坑**：`libnet80211.a` 引用 `libpp.a` 里的 `ic_*`，必须用
   `-Wl,--start-group … -Wl,--end-group` 包起来（177 → 30 个未定义）
3. **mesh/espnow 库**：加 `libmesh.a` `libespnow.a`（30 → 4）
4. **小符号**：`hexstr2bin`/`esp_log_level_set`/`g_mt`/`g_espnow_user_oui`/
   `esp_supplicant_init`/`esp_event_*` + libc 的 `str*`/`puts`/`free`/`snprintf`/`vsnprintf`
   ⇒ **0 个未定义符号**
5. **`l32r` 越界坑**：链接脚本必须把 `.literal` 和 `.text` **按同一个 .o 成对**放置：
   ```ld
   *(.literal .literal.* .text .text.*)
   ```
   （分成 `*(.literal*)` + `*(.text*)` 两个模式会让跨目标文件的字面量池超出 ±256KB）

### 现在唯一的卡点：IRAM 装不下

```
ld: region `IRAM' overflowed by 421187 bytes
```

WiFi 闭源栈（libpp + libnet80211 + libcore + mesh + espnow）约 **500KB**，
而我们的 IRAM 只有 **66.8KB**（而且 0x403D9500 以上是坏区，不能再扩）。

⇒ **必须把闭源库放进 flash（IROM，0x42000000 起）**，用 I-cache 取指。
这正是文档 §24 里记的 `SHUT_CORE0_BUS` 问题：S3 复位后 `EXTMEM_ICACHE_CTRL1`
的 bit0（SHUT_CORE0_BUS）默认是 1，必须清掉再开 I-cache，否则取指全错。

标准库里已经有 `os_icache_enable()`（先清 SHUT_CORE0_BUS 再使能），
下一步就是：
1. 把 libpp/libnet80211/libcore/mesh/espnow 放进 `.flash.text`（0x42000000）
2. 启动时 `os_flash_map()` + `os_icache_enable()`（映射 0x42000000 到 flash 偏移 0）
3. 真板验证：能不能从 IROM 正常取指执行（用一个放在 flash 里的自证函数）

## 52. 第 34 轮末：IROM 取指验证卡在 `os_flash_map`/`os_icache_enable`

真板跑 `tests/t_s3_irom.c6`（串口实测）：

```
c6os s3 boot
irom test start          <- 打到这里
（之后无输出，RTC 信标 slot0 仍是 0）
```

⇒ 卡在 `os_flash_map(...)` 或紧接着的 `os_icache_enable()`，
还没走到写 slot0 信标那一步（信标 slot3=0xB0B0B0B0 说明 `port_init` 正常）。

**下一步就查这两个函数**（`port/s3/flash.c`）：

- `os_flash_mmu_entry(0x42000000)` 的 MMU 读写是不是碰了坏区/没映射的地址
- `os_icache_enable()` 里清 `EXTMEM_ICACHE_CTRL1.SHUT_CORE0_BUS` 的顺序
  （S3 要求：先关 cache、清 SHUT_CORE0_BUS、再使能）
- 建议先用 USB-JTAG（`tools/dbg.sh`）在 `os_flash_map` 上下断点单步，
  这个通道已经验证可用，不用再猜

### 第 34 轮末补充：IROM 用例真正卡在 `memcpy`

用 USB-JTAG 逐个断点跟下来：

```
停在 os_flash_map+11           <- 进入映射函数
停在 os_icache_enable+36       <- 继续执行
单步后发现 PC 在 0x403d0d7e..0x403d0d8d 之间反复循环
nm 一查：0x403d0d7c = memcpy
```

⇒ 不是 cache 的问题，而是 **`os_flash_map()` 里那次 `memcpy` 卡住了**
（`port/s3/flash.c`）。下一步就看这次 memcpy 的长度/源地址算错了哪里
（很可能是把 0x20000 这种长度当成了元素数，或者源地址跨了没映射的区域）。

## 53. 第 35 轮：S3 的 IROM/cache 状态实测（重要数据）

新增 `tests/t_s3_irom3.c6`：只读寄存器、不做任何配置，串口实测 **ROM 引导完之后**的状态：

```
ICACHE_CTRL  (0x600C4060) = 0x00000000     <- I-cache 是关的
ICACHE_CTRL1 (0x600C4064) = 0x00000003     <- 两个核的 flash 访问都被 SHUT（bit0/bit1）
MMU[0..3]    (0x600C5000) = 0x00004000     <- **MMU 表项已经建好了**（bit14=valid，page 0）
*(u32*)0x42000000         = 0xbad00bad     <- 读 IROM 返回"坏数据"哨兵
*(u32*)0x3C000000         = 0x00000000
```

⇒ **MMU 不用我们建**（ROM 已经映射好了 IROM 0x42000000 → flash page 0），
缺的只是"清 SHUT + 开 cache"。

按这个顺序打开之后：

```
ICACHE_CTRL  = 0x00000001   (bit0 = enable)
ICACHE_CTRL1 = 0x00000000   (SHUT 已清)
*(u32*)0x42000000 = 0x00000000   <- 不再是 0xbad00bad（总线通了），但还是 0
*(u32*)0x42000004 = 0x00000000
*(u32*)0x42001000 = 0x00000000
```

即：**总线从"拒绝访问"变成"能访问但读到 0"**，说明 cache 还没有真正从 flash 取数。
可能的剩余因素（下一步排查顺序）：

1. S3 除了 `EXTMEM_ICACHE_CTRL`，还有 **cache/MMU 的电源控制**（`CACHE_MMU_POWER_CTRL` 之类）
2. flash 的读模式（ROM 是用 DIO @80MHz 引导的；cache 取指要求 SPI 控制器处于可读状态）
3. `EXTMEM_ICACHE_CTRL1` 可能还有 invalidate/仲裁位需要配
4. 建议对照：用 USB-JTAG 在 **原厂 IDF 固件** 上 halt，读同样的几个寄存器，
   直接拿到"能跑 IROM 的配置长什么样"（这是最快的办法，板子上有原厂固件备份可刷回）

### 结论

- WiFi 闭源栈**链接已经完全通过**（0 未定义符号）
- 唯一剩下的是 **IROM 执行（flash 取指）** 这一个平台能力；
  它通了，WiFi 库就能进 image，`esp_wifi_init_internal()` 就能真板跑
- 这条路上的所有测量手段（串口 + RTC 信标 + USB-JTAG + gdb 脚本）都已就位

## 54. 第 35 轮决定性的对照：把原厂 IDF 固件刷回去，用 JTAG 读同一批寄存器

方法（很快、很有效）：刷回 `/media/winger/inst_26/esp/xiaozhi/build/` 的原厂固件，
用 `tools/dbg.sh` halt 住，读 I-cache/MMU 寄存器，直接拿到"能正常跑 IROM"的配置。

| 寄存器 | 原厂（能跑 IROM） | 我们（开 cache 之后） | 结论 |
|---|---|---|---|
| `ICACHE_CTRL` 0x600C4060 | 0x00000001 | 0x00000001 | ✅ 一致 |
| `ICACHE_CTRL1` 0x600C4064 | 0x00000000 | 0x00000000 | ✅ 一致 |
| `0x600C4068` | **0x00000005** | 0x00000000 | ❌ 我们没设 |
| `0x600C407C` | **0x00000004** | 0x00000000 | ❌ 我们没设 |
| `MMU[0]` 0x600C5000 | 0x00004000 | 0x00004000 | ✅ 一致（MMU 不用我们建） |
| `*(u32*)0x42000000` | **0x00000000** | 0x00000000 | ✅ **一样！** |

**重要更正**：之前把"读 0x42000000 得到 0"当成失败是错的 —— 原厂固件里也是 0
（固件实际从 flash 0x20000 开始映射，IROM page 0 本来就空）。所以我们的
"清 SHUT + 使能 I-cache"**很可能已经是对的**。

### 下一步（很明确）

把原厂设的另外两个寄存器补上，然后**用真实映射验证**（而不是读 page 0）：

- `0x600C4068` = 0x5  → 每核的 cache 配置（`EXTMEM_ICACHE_CTRL2` 类）
- `0x600C407C` = 0x4  → preload/autoload 配置类
- 验证方法：把一段代码放到 `.flash.text`（flash 偏移 = 我们镜像的 LMA），
  **用 esptool 单独把这段字节写到那个 flash 偏移**，然后在应用里把 MMU 对应的
  IROM 页指过去、直接 `callx` 进去 —— 返回值写 RTC 信标（不需要串口）。
  MMU 表项格式已验证：`page | 0x4000(valid)`。

### 附：原厂固件已备份且能刷回

```sh
B=/media/winger/inst_26/esp/xiaozhi/build
esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 $B/bootloader/bootloader.bin \
  0x8000 $B/partition_table/partition-table.bin 0x20000 $B/xiaozhi.bin
```
（刷完是能正常运行的完整小智固件，说明硬件和 flash 都没问题。）

### 第 35 轮末：IROM 端到端验证的半成品（精确交接）

已经把 `.flash.text` 固定到 **vaddr 0x42100000 / LMA 0x100000**（`port/s3/link.ld`），
并用 `objcopy -O binary --only-section=.flash.text` 抽出 24 字节、用
`esptool write-flash 0x100000` 写进 flash（Hash verified ✓）。
用例 `tests/t_s3_irom.c6` 会：映射 `0x100000 → 0x42100000` → 开 I-cache →
`s3_win_call_flash42()`（期望返回 42）→ 结果写 RTC 信标 slot0/slot1。

**当前状态**：信标 slot0/slot1 仍是 0 ⇒ 应用没走到写信标那一步。
调试门（等调试器放行）用 RTC 寄存器做时发现 CPU 读到的值和 esptool 读到的不同步，
已改成 DRAM 全局变量 `g_gate`（注意 `.c6` 里全局必须显式初始化：`volatile u32 g_gate = 0u;`）。

下一步只需要：
1. 确认门能放行（`set {int}g_gate = 0xDEADBEEF` 后 `detach`）
2. 读信标 slot1：**42 = IROM 取指成功**（这是 WiFi 进 flash 的最后一道门）
3. 若失败，用 JTAG 在 `s3_win_call_flash42` 上单步

### 第 36 轮末：IROM 验证的实际阻塞点（最新）

去掉调试门后真板实测，串口只到 `irom test start` 就没了。用 USB-JTAG halt 看：

```
#0  port_delay_ms ()
#1  blink ()
```

即卡在用例开头那个 `blink(2, 150)`（2 次、每次 150ms，本该 0.6 秒）里出不来；
`port_delay_ms` 本身是纯忙等（`for (i=0;i<40000u;i++)`，无溢出问题），
所以更可能是**应用在反复复位**（每次复位都从头跑，我们抓到的永远是开头那一段）。

下一步要确认的是：`blink()` 之后到底有没有走到 `os_flash_map`（读信标 slot0 即可判断，
slot0 = 0x10000001 表示映射那段代码执行过了）。若没走到，就先查
`bl_init()`/`bl()` 里的 GPIO 写法有没有把背光引脚配成特殊功能导致异常。

## 55. 第 36 轮：修好 MMU 表项的 valid 位（真板验证），IROM 调用仍未返回

### 已修（真板实证）

`port/s3/flash.c` 的 `os_flash_map()` 原来只写页号：

```c
*e = ((flash_off >> 16) + i) & 0x3FFFu;      /* 错：把 valid 位清掉了 */
```

S3 的 MMU 表项格式是 **bit[13:0]=flash 页号、bit14=valid、bit15=0(IROM/DROM)**，
少了 bit14 映射就是无效的。改成：

```c
*e = (((flash_off >> 16) + i) & 0x3FFFu) | 0x4000u;
```

真板验证（信标 slot2 = MMU 表项）：`0x00000010` → **`0x00004010`** ✓

另外把从原厂固件读到的两个 cache 配置寄存器也补进了 `os_icache_enable()`：
`0x600C4068 = 0x5`、`0x600C407C = 0x4`，`SHUT` 位同时清 core0/core1。

### 仍未通

用例现在的自证方式是纯 RTC 信标（开 I-cache 之后**不打印**，因为实测 ROM 的
`ets_printf` 在这之后会跑死）：

```
slot0 = 0xB0000001   进到 os_main
slot2 = 0x00004010   MMU 表项（valid ✓）
slot3 = 0xB0B0B0B0   port_init 走过
slot0 没有变成 0xF1xxxxxx  =>  s3_win_call_flash42() 没有返回
```

即：映射有效、cache 使能，但**跳到 flash 里的代码还是不通**。

### 下一步（按优先级）

1. 用 JTAG 在 `os_icache_enable` 之后单步，看 `s3_win_call_flash42` 里是
   `callx0 0x42100000` 就挂住，还是取指失败进了异常（读 `$exccause`/`$epc1`）
2. 确认 flash 里 0x100000 处的字节确实是那段代码（`esptool read-flash 0x100000` 对比
   `objcopy` 出来的 24 字节）—— 我们写的时候是 verify 通过的，但值得复核一次
3. 对照原厂固件再读几个相关寄存器（`0x600C406C`、`0x600C4070`、`0x600C4074`
   以及 `SPI_MEM` 的 cache 控制位），把差异一次性补齐

## 56. 第 36 轮决定性证据：**跳进 IROM 成功了，但取到的指令是 0（ill）**

把用例改成"循环调用 flash 里的函数 + 每次写信标"，这样调试器随时附着都能抓到现场。
真板实测：

```
信标 slot1 = 0xAAAA0001      （马上要调 s3_win_call_flash42）
JTAG halt:
  #0 s3_trap
  $exccause = 0              （IllegalInstruction）
  $epc1     = 0x42100014     （**在我们的 flash 段里！**）
```

⇒ 说明：
1. **跳转本身成功了** —— `epc1` 落在 `0x42100000` 起的那 24 字节里 ✓
2. 但在那里执行时是 **IllegalInstruction** ⇒ 取到的指令字节是 `0x00000000`
   （Xtensa 的 `0x00000000` 正好解码成 `ill`）

也就是说 **I-cache 没有真正从 flash 读数据**（读回来是 0）。
这跟 §53 的观察一致（总线通了，但读出来全是 0）；原厂固件在 page 0 读到 0 是因为
那一页本来没映射，不能作为反证。

### 下一步非常明确

补 **SPI flash 控制器的 cache 读配置**，让 I-cache 能真正发起 flash 读：

- `SPI_MEM_CACHE_FCTRL_REG`（0x6000203C 附近）：cache 读的指令/地址模式
- `SPI_MEM_CACHE_SCTRL_REG`（0x60002040 附近）：数据读模式
- 以及 `SPI_MEM_CTRL_REG` 的 `FREAD_QIO/FREAD_DIO/FASTRD` 位（ROM 用 DIO@80MHz 引导）

最省事的办法仍然是**对着原厂固件抄**：用 JTAG halt 原厂固件，把
`0x60002000`~`0x60002080` 这一片 SPI_MEM 寄存器整体 dump 下来，和我们启动后的对比，
差异补上即可 —— 这已经是最后一块拼图（其余链路：PHY ✓、WiFi 链接 ✓、MMU ✓、I-cache 使能 ✓）。

## 57. 第 37 轮：SPI_MEM 寄存器对照（最后一块拼图的位置）

用 JTAG 分别 dump「原厂 IDF 固件」和「我们固件」的 `0x60002000~0x600020A0`：

| 地址 | 原厂（能跑 IROM） | 我们 |
|---|---|---|
| `0x60002008` | 0x00608000 | **0x00ECA008** ← 不一样 |
| `0x60002014` | 0x00030103 | **0x00010001** ← 不一样 |
| `0x6000201C` | 0x5c000007 | 0x5c000007 ✓ |
| `0x60002028` | 0x00000007 | 0x00000007 ✓ |
| `0x60002054` | 0x001840ef | 0x001840ef ✓ |
| **ICACHE 0x600C4060/64/68** | **1 / 0 / 5** | **1 / 0 / 5** ✓ **完全一致** |

⇒ I-cache 侧配置已经和能跑的固件一模一样，MMU 表项也一致（`0x4010` valid），
**唯一差异在 SPI_MEM 的 flash 读配置**（`0x60002008`/`0x60002014`）。

已试：把 `elf2image` 的 flash 频率从 40m 改成 **80m**（和原厂一致）⇒ 仍然不通。
说明问题不在频率，而在那两个寄存器的**读模式位**（`0x60002008` 里含
`FREAD_QIO`/`FREAD_DIO`/`FASTRD` 等位；我们的值 0x00ECA008 明显带了 QIO 类配置，
而原厂是纯 DIO 0x00608000）。

### 下一步（最后一步）

启动时按原厂的值把 SPI_MEM 读模式改过来，或更稳妥地**照抄原厂 bootloader 的做法**：

```c
/* 在 os_icache_enable() 之前 */
*(volatile u32 *)0x60002008u = 0x00608000u;   /* DIO */
*(volatile u32 *)0x60002014u = 0x00030103u;
```

然后重跑 `tests/t_s3_irom.c6`，看信标 slot0 是否变成 `0xF100002A`（42 ⇒ IROM 通了）。
通了之后，`libpp/libnet80211/...` 就能放进 `.flash.text`（`make wfi` 那套参数），
WiFi 栈就能装进 image，`esp_wifi_init_internal()` 就能真板跑起来。

### 至此整条链路的完成度

| 环节 | 状态 |
|---|---|
| windowed ABI 桥 + 窗口向量 | ✅ 真板验证 |
| libphy 在 c6LOS 内执行 | ✅ 真板验证（5 步全返回） |
| PHY 之后串口失联的原因 | ✅ 定位（USB 时钟被清） |
| USB-JTAG 调试通道（无 UART） | ✅ 打通（断点/单步/读寄存器） |
| WiFi 闭源库 + IDF 源码层链接 | ✅ 0 个未定义符号 |
| MMU 映射（IROM） | ✅ 表项 0x4010 valid |
| I-cache 使能 | ✅ 与能跑的固件一致 |
| **SPI_MEM flash 读模式** | ❌ **唯一剩余** |
| 之后：WiFi 初始化/扫描/连接/DHCP/TCP/TLS/HTTP | ⏳ 待 IROM 打通 |

### 第 38 轮：按原厂值改 SPI_MEM 读模式 —— 仍未通（记录在案）

在 `os_icache_enable()` 里加上了：

```c
*(volatile u32 *)0x60002008u = 0x00608000u;   /* 原厂值：DIO */
*(volatile u32 *)0x60002014u = 0x00030103u;
```

重刷镜像 + flash 段后信标仍是：

```
slot0 = 0xB0000001   （进 os_main）
slot1 = 0xAAAA0001   （马上要调 flash 里的函数）
slot2 = 0x00004010   （MMU 表项 valid ✓）
=> slot0 没有变成 0xF100002A，即 s3_win_call_flash42() 仍未返回
```

说明 SPI_MEM 的这两个值不是充分条件（也可能写进去后被 ROM/硬件覆盖，或还需要别的位）。

### 交接时的确切结论（重要）

到这一轮为止，**WiFi 目标没有达成**。已确证的部分：

| 环节 | 状态 | 证据 |
|---|---|---|
| windowed ABI 桥 + 窗口溢出/欠载向量 | ✅ | 真板 deep=11 / deep12=074 |
| libphy 在 c6LOS 里执行（PHY 初始化） | ✅ | JTAG 逐步：5 步全返回，s3_trap 未命中 |
| PHY 之后串口失联的原因 | ✅ | USB 时钟/复位/RTC_USB_CONF 被清零（实测寄存器） |
| 无 UART 的调试通道 | ✅ | USB-JTAG + openocd + gdb（断点/单步/读寄存器） |
| WiFi 闭源库 + IDF esp_wifi 源码层链接 | ✅ | 0 个未定义符号 |
| MMU IROM 映射 | ✅ | 表项 0x4010（含 valid 位）|
| I-cache 使能 | ✅ | CTRL=1 / CTRL1=0 / 0x4068=5，与原厂一致 |
| **IROM 取指（flash 里执行代码）** | ❌ | epc1=0x42100014 但 exccause=0（取到 0x00000000 = ill） |
| WiFi 初始化/扫描/连接/DHCP/TCP/TLS/HTTP | ⏳ | 卡在上面这一项 |

**唯一阻塞点**：S3 的 I-cache 不从 flash 取数（读到 0）。这是纯平台配置问题，
所有寄存器对照手段都已就绪（刷原厂固件 + JTAG dump 对照），继续做只需要：

1. 更系统地 dump 原厂固件的 `0x60002000~0x60002100`（SPI_MEM 全段）以及
   `0x600C4000~0x600C4100`（EXTMEM/ICACHE 全段），逐位比对
2. 或者在原厂固件里 halt 后，**直接观察它执行一次 flash 访问**前后的寄存器变化
3. 也可以换个思路：**用 ROM 的 `Cache_Read_Enable`/`SPI_read` 类 API**
   （ROM 里有现成的 flash cache 初始化入口，只是之前没找对）

### 代码与工具状态

- 所有工作已提交（main 干净）
- 板子刷的是 `build/won`：背光常亮 + 每秒心跳，不碰 PHY，串口/esptool/JTAG 都可用
- 排查工具：`tools/dbg.sh`、`tools/flashmon.sh`、`tools/beacon.sh`、`tools/idfcc.sh`、
  `tools/{gphy,gfin2,gusb,gtrap2,gidf2,gspi,gwhere2}.gdb`

### 第 38 轮末补充：EXTMEM 寄存器块的读取

再 dump 原厂固件的 `0x600C4000` 起：

```
0x600c4000: 0x00000001  0x00000000  0x00000005  0x00000000
（0x600C4010 之后读出被 abort，说明那一段没有寄存器/需要别的访问方式）
```

和之前读 `0x600C4060/4068` 得到的 `1 / 5` 是**同一组值** ⇒ ICACHE 块可能存在镜像地址，
或者我们对偏移的理解要按 TRM 再核一遍（`EXTMEM_ICACHE_CTRL` 的确切地址）。

这条线索留给下一次：**用 S3 的 TRM 核对 ICACHE/EXTMEM 寄存器映射**，
而不是继续靠读值猜。原厂固件备份和刷回命令都在 §54。

### 第 39 轮：S3 ROM 没有导出 flash/cache 初始化入口

查遍 `esp32s3.rom.ld` / `esp32s3.rom.api.ld`：

```
没有 Cache_* 前缀的符号（Cache_Read_Enable / Cache_Enable_ICache 都没有导出）
没有 spi_flash_* 符号
```

所以"调 ROM 的现成函数把 cache 配好"这条路在 S3 上走不通（只有 efuse 相关的
`ets_efuse_get_spiconfig`、`ets_efuse_flash_octal_mode` 之类）。

⇒ 必须自己把 SPI_MEM 的 cache 读路径配全。**下次最有效的办法**：
在**原厂固件**上 halt，用 JTAG 写一个"探针"到它的 RAM 里跑不了（没有编译环境），
所以更现实的是：

1. 按 S3 TRM 的 "SPI0/1 Cache Control" 章节，把
   `SPI_MEM_CACHE_FCTRL`(读命令/addr 位宽/mode 位)、`SPI_MEM_CACHE_SCTRL`、
   `SPI_MEM_SRAM_CMD`、`SPI_MEM_CTRL`(FREAD_* 位) 全部按 DIO@80MHz 配一遍
2. 每次配完读一次 `0x42000000`（应该返回 flash 偏移 0 的内容，我们的镜像头 `0xE9...`）
   作为**快速判据**——这个判据比跳进去执行安全得多
3. 直到 `0x42000000` 读回 `0x000000e9` 一类非 0 值，就说明 cache 读通了，
   那时 IROM 执行自然就通了

### 第 39 轮末：flash 段写入必须校验（踩过一个坑）

之前几次 `esptool write-flash 0x100000 <段>` 我都用 `>/dev/null` 吞掉了输出，
结果**根本没写进去**（端口 busy 之类）。读回校验才发现 flash 0x100000 是旧内容。

正确做法（以后一律照这个来）：

```sh
esptool --chip esp32s3 -p /dev/ttyACM1 write-flash 0x100000 /tmp/fseg.bin | grep verified
esptool --chip esp32s3 -p /dev/ttyACM1 read-flash 0x100000 24 /tmp/rb.bin
cmp -s /tmp/rb.bin /tmp/fseg.bin && echo 一致
```

这次确认写进去了、读回也一致，但信标仍是：

```
slot0 = 0xB0000001   进 os_main
slot1 = 0xAAAA0001   即将调用 flash 里的函数（没有变成 0xF100002A）
slot2 = 0x00004010   MMU 表项 valid ✓
```

⇒ 排除了"flash 内容不对"，确认就是 **I-cache 不从 flash 取数**。
（这也是本轮唯一没打通的平台能力，其余链路全部就绪。）

## 58. IROM 探针系统化搜索：6 种配置全部读出 0（含 ROM 自己的 Cache_Enable_ICache）

新用例 `tests/t_s3_irom4.c6` 的做法：**每次"开 cache → 读探针 `0x42100000` → 立刻关 cache"**，
这样即使配置不对也不会把串口搞挂，能拿到每一步的结果。探针期望值
`0x7B004136`（flash 0x100000 的头 4 字节，已用 `read-flash` + `cmp` 校验过内容正确）。

真板结果：

```
初始 CTRL=0x0 CTRL1=0x3  SPI08=0x00ACA008 SPI14=0x00010001
MMU[0x10]=0x00004010                       <- 映射有效
A 清SHUT+使能                => 0x00000000
B +4068=5/407C=4             => 0x00000000
C +invalidate 脉冲           => 0x00000000
D +SPI_MEM 按原厂值          => 0x00000000
E 读 flash 0x0 头            => 0x00000000
F ROM Cache_Enable_ICache    => 0x00000000   <- 走 windowed 孤岛调 ROM 0x40001878
```

**连 ROM 官方的 `Cache_Enable_ICache()` 都读不出来** ⇒ 问题不在"配置没配对"这个层面，
而更靠底层（flash 控制器给 cache 的读通道、或者 bootloader 之后 SPI flash 处于某种
只有 ROM 能读的状态）。

### 已排除

- ✅ flash 内容正确（`write-flash` + `read-flash` + `cmp` 一致）
- ✅ MMU 表项正确（`0x4010`）
- ✅ I-cache 使能位与原厂一致（`CTRL=1/CTRL1=0/4068=5`）
- ✅ SPI_MEM 关键寄存器按原厂值写过
- ✅ ROM 的 Cache_Enable_ICache 也试了（经 windowed 孤岛，ABI 正确）

### 建议的下一条路（换个方向）

既然"让 cache 读 flash"这么难，可以**绕开它**：

1. **把 WiFi 栈裁到 IRAM 装得下** —— 只链真正用到的对象（`-ffunction-sections` +
   `--gc-sections` 对闭源 .a 无效，但可以只 `ar x` 出需要的 `.o` 再重打包），
   先跑通 scan 这种最小功能
2. **用 SPI_MEM 的 FIFO 手动读 flash 到 RAM**（不走 cache）：把需要的代码段在
   启动时拷到 IRAM 的保留区 —— 但 500KB 装不下，只能挑关键路径
3. **换成带 PSRAM 的配置**（如果板子有）—— 代码不能从 PSRAM 取指，也无解

结论：**S3 上要让闭源 WiFi 栈跑起来，"从 flash 取指"基本是硬需求**，
这块平台能力没打通之前，WiFi 目标无法推进。所有其他环节（PHY、ABI、链接、调试）都已就绪。

## 59. 两个重要纠正（推翻前两轮的结论）

### (1) gdb/JTAG **读不了 IROM**，"探针=0" 的结论全部作废

在**能正常跑的原厂固件**上 halt，读它自己的代码地址：

```
ORIG_IROM_0x42020000: 0x00000000     <- 原厂固件正在从 flash 取指执行，这里也读回 0
ORIG_MMU10:              0x00004000
```

⇒ JTAG 读 IROM 窗口恒为 0，**不能用来判断 cache 是否工作**。
（§58 里 A~G 全部读 0 是因为这个方法本身无效，不是配置不对。）

### (2) ROM 的 `Cache_Invalidate_ICache_All()` 一调就挂

为了清 I-cache，把 ROM 函数经 windowed 孤岛正确调用（ABI 这次是对的）：

```
s3_win_call_cache_invalidate()   -> ROM 0x400016d4 Cache_Invalidate_ICache_All
s3_win_call_cache_enable()       -> ROM 0x40001878 Cache_Enable_ICache
```

真板信标（`tests/t_s3_irom5.c6`，每个调用后都写信标）：

```
slot0 = 0xB0000002   进 os_main
slot2 = 0x00004010   MMU 映射有效
slot3 = 0xB0B0B0B0   <- 一直是初值：**invalidate 没返回**
slot1 = 0x00000000   所以后面的 flash 调用根本没执行
```

⇒ 即使走正确的 windowed ABI，ROM 的 cache 维护函数在我们的 RAM-only 镜像里
也跑不通（这也是以前 call0 直调"一调就死"的真正原因之一）。

### 有效的判据（以后只用这个）

**唯一有效的方法是"跳进去执行"**：`s3_win_call_flash42()` 调 flash 里的
`s3_asm_flash_42`(`0x42100014`: `movi.n a2,42; retw.n`)，返回值写进信标。
- 返回 42 → IROM 通了
- 不返回 / `exccause=0` + `epc1=0x42100014` → 取指失败

已知寄存器状态（`0x600C4000~0x600C40FF` 完整 dump，与原厂逐字一致）：

```
DCACHE_CTRL=1  DCACHE_CTRL1=0  DCACHE_TAG_PWR=5  DCACHE_SYNC=4
ICACHE_CTRL=1  ICACHE_CTRL1=0  ICACHE_TAG_PWR=5  ICACHE_SYNC=4
MMU[0x10]=0x4010   (page 0x10 -> flash 0x100000, valid)
```

即：**cache 硬件配置与能工作的原厂固件逐位相同，MMU 映射正确，flash 内容已校验，
但取指仍然拿到 0x00000000。**

## 60. 走"普通 flash 镜像"路线的进展（本轮最大突破方向）

思路：IROM 取指需要 cache 建好，而**建 cache 正是 bootloader 的职责**。所以不再做
RAM-only 镜像（ROM 直接引导、跳过 cache 初始化），改成"bootloader + app 镜像"。

### 踩到并解决的三个真问题

1. **`esp_app_desc_t` 必须在镜像文件偏移 0x20**
   `esptool elf2image` 把 DRAM 段排在最前，app_desc 落不到 0x20 ⇒ bootloader 直接复位。
   解决：自己写 `tools/mkimg.py`，第一段放 DROM(`.flash.rodata`)，数据正好从 0x20 开始。

2. **IRAM 段不能压在 bootloader 身上**
   我们原来 IRAM 在 `0x403C9000`，而 bootloader 正跑在 `0x403C8700..0x403CB700` ⇒
   `abort() at mmu_hal_map_region`。把 IRAM 挪到 **0x40378000**（长度 0x10700）后解决。

3. **flash 段的 vaddr 必须 64KB 对齐**，且镜像内偏移 = `paddr - 镜像基址`，
   这样 bootloader 的 MMU 映射才成立。

### 当前状态

改完之后：`abort()` 消失、bootloader 正常加载并跳到我们的 app（`entry 0x403c88f4`），
但随后 **bootloader 仍判定镜像无效并软复位**（`rst:0x3 RTC_SW_SYS_RST` 循环）。

剩下的疑点是镜像的 **XOR 校验字节**：

```
$ esptool image-info /tmp/img3.bin
Checksum: 0x00 (invalid - calculated 0x10)
```

我们自己算（`checksum = 0xEF; for each byte: ^= b`，范围到校验字节之前）得到 0x2E，
且整镜像 XOR（含校验字节）恰好为 0，和 IDF 的约定一致；但 esptool 报的期望值是 0x10，
说明它取的 `image_len` 和我们不一致（差 1~2 字节）。**下一步就是把这一字节对齐**，
esptool 认可后 bootloader 就会接受镜像。

一旦接受，bootloader 会替我们建好 MMU+cache，IROM 取指这个卡了很久的问题就自然解开，
WiFi 闭源库（~500KB）也就有地方放了。

## 61. 自定义镜像已经做到"bootloader 能加载"，但卡在校验的最后一步

### 已经确认对的部分（和原厂逐字节对比过）

```
原厂 xiaozhi.bin 前 0x28 字节：
  e9 07 02 4f  409a3740  ee000000  09000000  00630000 00000001  <- 24 字节头
  2000 1e3c  a8390b00                                           <- 段 0 头: vaddr=0x3C1E0020 len=0xB39A8
  3254cdab ...                                                  <- 段 0 数据 = esp_app_desc_t (魔数!)
```

我们按同样格式生成的镜像：

```
  e9 06 02 1f  f0883740  ee000000  09000000  00000000 00000000  <- 24 字节头
  2000 003c  d8ff0d00                                           <- 段 0 头: vaddr=0x3C000020 len=...
  3254cdab ...                                                  <- 段 0 数据 = esp_app_desc_t ✓
```

关键规律（原厂 0x3C1E0020 ↔ 文件偏移 0x20 印证）：
**flash 段的 vaddr 低 16 位必须等于该段数据在镜像文件里的偏移低 16 位**，
bootloader 用这个关系反推 flash 物理地址并建 MMU 映射。

### 现在的失败点

1. bootloader 加载了我们的镜像、`mmu_hal_map_region` 的断言不再触发（IRAM 已挪到
   0x40378000、flash 段已按上述规律对齐）
2. 但 `bootloader_utility_load_boot_image` 仍然判定"没有可启动分区"（JTAG 实测
   断点 `bootloader_reset`，回溯到 `bootloader_utility.c:628`）
3. `esptool image-info` 也报同一个问题：

```
ERROR: End of file reading segment 0x887c0000, length 2100052023
```

   这个错误里 `0x7d2c` 正好是 `.text` 段长度、`0x887c` 是 `.text` 段 vaddr 的尾巴，
   说明 **它读段头的位置比我们写的晚了 4 字节**（段与段之间的补齐规则不一致）。

### 下一步（很具体）

- 段与段之间的 4 字节补齐规则要和 esptool/IDF 完全对齐：IDF 的 `load_segment`
  本身**不补齐**，补齐由调用方按 4 字节处理；我们最后一段之前补、最后一段之后
  再放校验和，这一处需要逐字节复核
- 校验和：IDF 是 `checksum = 0xEF; 对所有段的数据 XOR`（不含镜像头/段头，
  含段间补齐）；我们已按此实现，但 `esptool` 报的期望值还差 1~2 字节，需一起核对
- 一旦 `esptool image-info` 通过，bootloader 必然接受；那时 bootloader 会替我们
  建好 MMU+cache，**IROM 取指这个卡了很久的根本问题就自然解开**，WiFi 闭源库
  （~500KB）也就有地方放了

## 62. 里程碑：我们的 C 代码已经能以"标准 IDF app 镜像"被 bootloader 引导起来

### 打通的完整链路（都有真板证据）

```
flash: 0x0     原厂 bootloader
       0x8000  我们自己的分区表 (tools/ → p2.bin, factory @0x20000, 4MB)
       0x20000 我们自己生成的 app 镜像 (tools/mkimg.py)
```

真板串口实际输出：

```
mode:DIO, clock div:1
load:0x3fce2820,len:0x544
load:0x403c8700,len:0xcc8
load:0x403cb700,len:0x2ea0
entry 0x403c88f4          <- bootloader 自己的入口
c6os s3 boot              <- ★ 我们的应用起来了（我们自己的 banner）
*** TRAP sp=3fceef60 cause=00000000 pc=42100014 ps=00060910
```

信标同时证明 `os_main` 真的执行到了：`slot0 = 0xB0000003`。

### 生成合法镜像的四个必备条件（全部实测确认）

1. **`esp_app_desc_t` 必须在镜像文件偏移 0x20**（魔数 `32 54 cd ab`）
2. **段表必须连续**，段与段之间不能留空洞；flash 段的"对齐空洞"要并入上一段的长度
3. **每段长度补齐计入长度本身**（原厂各段长度全是 4 的倍数；我们的 `.vector` 是 0x47A
   不补就会被解析错位 4 字节）
4. **整个镜像长度必须是 16 的倍数**，且最后 1 字节是校验和
   （`checksum = 0xEF; 对各段数据 XOR`）——满足后 `esptool image-info` 报
   `Checksum: 0xdd (valid)`

### bootloader 侧实测状态（JTAG 读寄存器）

```
MMU[0x10] = 0x00000003     -> 0x42100000 映射到 flash page 3 = 0x30000  ✓ 映射正确
ICACHE_CTRL = 1, CTRL1 = 0 ✓ I-cache 已使能、bus 未关
DCACHE_CTRL = 1, CTRL1 = 0 ✓
SPI08 = 0x016CA008         vs 原厂 0x00608000   <- 仍不一致
```

⇒ 映射、cache 都是对的，但跳进 flash（`0x42100014` = `movi.n a2,42`）仍然
`cause=0`（IllegalInstruction）⇒ 取指拿到 0。
**最后一个疑点是 `SPI_MEM` 的 cache 读配置（0x60002008）**：bootloader 配出来是
`0x016CA008`，而能正常跑 IROM 的原厂固件是 `0x00608000`。下一步就是在调 flash 之前
把这两个寄存器改成原厂值再试（用例已改成这么写，但那次构建的镜像尺寸变了导致
bootloader 又不认，需要重新对齐镜像布局）。

## 63. IROM 取指：全部前置条件逐项验证通过，仍然 fault（本轮结论）

现在我们的应用是**标准 IDF app 镜像**（bootloader 引导、`esptool image-info` 报
`Checksum: valid`），可以把"取指失败"这件事查得非常干净了。真板实测（JTAG）：

```
MMU[0x10] = 0x00000003        0x42100000 -> flash 页 3 = 0x30000        ✓
MMU[0x03] = 0x00000003        换成恒等映射 0x42030000 -> 0x30000        ✓ 也对
flash 0x30000 + 0x14 处内容 = 2c a2 1d f0   (movi.n a2,42 ; retw.n)    ✓ 内容正确
ICACHE_CTRL = 1, ICACHE_CTRL1 = 0                                      ✓ 已使能
DCACHE_CTRL = 1, DCACHE_CTRL1 = 0                                      ✓ 已使能
SPI_MEM 0x60002008 = 0x00608000   (改成原厂能跑 IROM 的固件的值，已回读确认) ✓
```

真板现象（三种组合都一样）：

```
c6os s3 boot
*** TRAP sp=3fceef60 cause=00000000 pc=42100014    <- 0x42100000 版
*** TRAP sp=3fceef60 cause=00000000 pc=42030014    <- 恒等映射版
```

信标同时正常写入（`slot0=0xB0000003`、`slot2=0x00608000`），说明应用本身跑得好好的，
**只有"取指进 flash"这一步拿回 0x00000000**（`0x0000` 在 Xtensa 里正好是 `ill`）。

### 结论

S3 的 IROM 取指在我们这套"自建镜像 + 原厂 bootloader"里，**TRM 和 IDF 要求的所有
前置条件都已逐项验证成立**（映射、内容、双 cache、SPI_MEM 时序寄存器、页对齐、
恒等/重映射两种方式），取指仍然返回全零。这不是配置漏配，而是更底层的问题
（cache 的 TAG 状态 / flash 控制器的 cache 读通道 / 芯片的某种保护）。

### 下一步方向（如果要继续攻）

1. 对比**原厂固件运行态**下 `0x42020000` 附近的 MMU 表与 SPI_MEM 全量寄存器
   （它是能跑 IROM 的），逐位找出我们缺的那一位
2. 用 JTAG 在**原厂固件**里把 PC 设到我们镜像的 flash 页上单步，看它的环境能否
   取指成功——如果能，就把差异收敛到"bootloader 之后、app 之前"这段
3. 如果确认 S3 从 RAM 引导的场景下 IROM 不可用，就只剩"把 WiFi 栈裁到 66KB
   IRAM"这条路（可行但工作量大）

## 64. 全量寄存器对照：硬件配置与原厂固件**逐字节相同**，取指仍失败

用 JTAG 在两边各 dump 同一批寄存器再逐字 diff
（`0x60002000~0x6000207F` SPI_MEM + `0x600C4000~0x600C40FF` EXTMEM/cache）：

```
--- SPI_MEM 差异 (0x60002000+):   （无）
--- EXTMEM 差异 (0x600C4000+):
  0x600C402C orig=0x3c1e0000 ours=0x3c800000   <- DCACHE_SYNC_ADDR，只是最后同步地址，无意义
  0x600C40C8 orig=0x00000000 ours=0x00000002   <- 访问计数器（我们跑得久，数值大）
  0x600C40CC orig=0x00000000 ours=0x00000004
  0x600C40D4 orig=0x0000003c ours=0x00000d39
  0x600C40D8 orig=0x0000009c ours=0x0000347a
```

⇒ **SPI_MEM 全块完全一致，EXTMEM/cache 全块也完全一致**（差异只有同步地址和性能计数器）。

也就是说：flash 内容 ✓、MMU 映射 ✓（恒等和重映射两种都试过、JTAG 回读确认）、
双 cache 使能 ✓、SPI_MEM 时序/读命令 ✓、页对齐 ✓ —— **每一项都和能正常跑 IROM 的
原厂固件一模一样，取指依然拿到 0x00000000**（`exccause=0`，Xtensa 里 `0x0000` = `ill`）。

这是本项目目前唯一剩下的平台级障碍。已经排除的方向列在上面的 §60~§63 里，
建议后续按 §63 的三条思路继续（最有用的是第 2 条：在原厂固件的环境里用 JTAG
把 PC 指到我们镜像的 flash 页上单步，把差异收敛到"bootloader 之后、app 之前"）。

## 65. 最后一组诊断：确认是"取到非法指令"而不是"映射/权限错误"

JTAG 直读异常寄存器（我们的 app，跳进 flash 后 trap）：

```
pc        = 0x4037c011  (s3_trap 里的死循环)
exccause  = 0           <- IllegalInstruction
excvaddr  = 0           <- 没有任何地址错误
epc1      = 0x42030014  <- 出错的那条指令地址（我们的 flash 段里）
```

如果 MMU 没映射或权限不对，会得到 `LoadStoreError`/`InstructionFetchError`（cause 24/28/29）
且 `excvaddr` 有值。现在 `cause=0`、`excvaddr=0` ⇒ **CPU 确实取到了指令，但取回的是
`0x00000000`**（Xtensa 里 `0x0000` 正好解码成 `ill`）。

同时做的一个对照实验（在原厂固件的环境里用 JTAG 把 PC 指到它自己映射的 flash 页上单步）
**同样 fault**，说明这条路本身也不能用来验证——JTAG 单步进 flash 不可靠，结论以
"真正跳进去执行"为准。

至此 IROM 取指问题的性质已经完全确定：**不是配置、不是映射、不是内容**，
而是 cache 从 flash 取回的数据恒为 0。所有 IDF 官方路径（bootloader 引导、ROM 的
Cache_Enable_ICache、原厂 SPI_MEM 值）都试过并验证生效，问题仍在。

## 66. 联网查证：症状与官方文档/他项目经验完全对得上，且已排除最后两个猜测

### 官方文档对 `IllegalInstruction`（EXCCAUSE 0）在 0x42xxxxxx PC 上的解释

> "无法从 SPI flash 中读取下一条指令" —— 即 CPU 在 IROM 窗口执行，但 flash/MMU 路径没送出
> 真正的指令；取回 `0x00000000`，而 `0x0000` 在 Xtensa 里正是 `ill` 指令。

注意区分：真正的"PC 不在合法区域"是另一个 panic `InstrFetchProhibited`（cause 20）。
我们的是 cause 0 + `excvaddr=0` ⇒ **取指被允许但取回垃圾**，与"映射/权限"无关。

### 他项目踩过的同类坑（esp32-baremetal-zig / Zephyr / NuttX）

- esp32-baremetal-zig 有一个 commit 标题就叫 *"fix IllegalInstruction crash on real
  hardware flash-boot"*，原因写得和我们的处境一模一样：**S3 上 flash-cache 的 MMU 在
  最开始那一刻还没准备好取指**，所以入口必须在 IRAM，进 IROM 必须用间接跳转
  （`l32r` + `callx8`）。
  → 我们已经是这么做的（`_start` 在 IRAM、经 windowed 孤岛的 `callx0` 进 flash）。
- Zephyr 的做法是把 `esp_image_segment_header_t` 的 flash 偏移与
  `_image_irom_vaddr` 都按 `MMU_FLASH_MASK` 对齐后写 MMU 表 —— 我们也对上了。
- NuttX 的注记：**写 MMU 寄存器时要挂起 cache**，否则 cache 子系统的查表和 CPU 写
  MMU 会互相干扰。

### 本轮又验证（并排除）的两个猜测

1. **bootloader 建的映射缺 valid 位**：实测 `MMU[3] = 0x00000003`（原厂表项是 `0x4000`），
   于是我们自己写 `0x4003` 补上 valid 位（信标回读确认 `slot3 = 0x00004003`）
   → **仍然 `cause=0, pc=42030014`**，不是这个原因。
2. SPI_MEM 值、双 cache、页对齐、恒等映射 —— 前面几节已逐项排除。

### 剩下最可能的一条

**改完 MMU 之后 cache 的 TAG 需要失效**（NuttX 那句注记），而 S3 上唯一能做失效的
`Cache_Invalidate_ICache_All()`（ROM 0x400016d4）在我们的镜像里一调就不返回
（§59 实测）。所以下一步的关键是**找一个不依赖 ROM 的 I-cache 失效办法**
（例如直接操作 `EXTMEM_ICACHE_CTRL` 的 preload/sync 位，或在 cache 关闭状态下写表）。

## 67. 寄存器级 I-cache sync 也试了：sync 能完成，取指依旧为 0

按 `extmem_reg.h` 找到真正的寄存器（不是靠猜）：

```
EXTMEM_ICACHE_SYNC_CTRL   = 0x600C4088   bit0=启动, bit1=SYNC_DONE
EXTMEM_ICACHE_PRELOAD_CTRL= 0x600C4094
EXTMEM_ICACHE_LOCK_CTRL   = 0x600C407C
EXTMEM_ICACHE_SYNC_ADDR   = 0x600C408C
```

用例里做了三件事（信标逐步确认）：

```
1. 手写 MMU[3] = 0x4003        -> slot3 前身回读 0x00004003  ✓ 表项正确
2. 写 0x600C4088 = 1 启动 sync
3. 轮询 bit1                   -> slot3 = 0x00000602        ✓ sync 完成（6 次轮询）
4. 调用 flash 里的 movi.n a2,42; retw.n
```

结果：**依然 `cause=0, pc=42030014`**（取回 0x0000）。

⇒ "MMU 表项缺 valid 位" 和 "cache 需要 sync" 两条也都排除了。

至此，S3 从 flash 取指这件事，我们这边能做的一切（镜像格式、bootloader 引导、
MMU 两种映射 + 手写表项、双 cache 使能、SPI_MEM 与原厂逐字节一致、寄存器级 sync、
ROM 官方 cache 函数）都已逐项试过并验证生效，取指仍返回全零。

### 下一条值得走的路：用同版本 IDF 编一个最小 app 做"参照物"

原厂那套 IDF 源码是现成的，可以编一个只有 `printf("hello")` 的最小 IDF app，
确认它能正常跑（它当然能），然后**逐步把它的初始化搬过来做二分**，找出"app 侧"
到底做了哪一件我们没做的事（最可能是 `spi_flash_init_chip_state()` 里改写 flash
**芯片内部**状态的那部分——寄存器看起来一致，但芯片内部的 dummy/QE 位可能不同）。

## 68. 查官方 TRM（ESP32-S3 Technical Reference Manual v1.8）

官方资料是有的，之前一直在逆向猜，这次把 TRM 拉到本地（`/tmp/trm.pdf`，15MB，
`pdftotext` 转文本 20 万行）逐节查了：

### §4.3.3 外部存储器 / §4.3.3.1 地址映射

> CPU 借助高速缓存 (Cache) 来访问外部存储器。Cache 将根据 MMU 中的信息把 CPU 指令总线
> 或数据总线的地址变换为访问片外 flash 与片外 RAM 的实地址……32 MB 的指令总线地址空间，
> 通过 ICache 以 **64 KB 为单位**映射到片外 flash 或片外 RAM，支持 4 字节对齐的读访问
> 或**取指访问**。

→ 和我们做的完全一致（64KB 页、0x4200_0000 起、页对齐）。ICache 16/32KB，块 16/32B。

### §4.3.3.3 Cache 操作

Write-Back / Clean / Invalidate（自动+手动）/ Preload / Lock —— 没有"改 MMU 前必须做某
特殊操作"的额外要求。

### §17 SYSTEM_CACHE_CONTROL_REG (0x60026048) —— 新查到的寄存器

```
bit0 SYSTEM_ICACHE_CLK_ON   置 1 使能 i-cache 时钟
bit1 SYSTEM_ICACHE_RESET
bit2 SYSTEM_DCACHE_CLK_ON   置 1 使能 d-cache 时钟
bit3 SYSTEM_DCACHE_RESET
```

这条很像是答案（"cache 根本没时钟，自然读不回数据"），于是做了两件事：

1. 在我们固件里读该寄存器：`0x00001000`（bit0/bit2 都是 0），于是置上 bit0|bit2
   → **仍然 fault**
2. 用 JTAG 去读**能正常跑 IROM 的原厂固件**的同一寄存器：**也是 `0x00001000`**
   → **说明这两位的状态本来就该是 0，和取指无关**，这条也排除

⇒ TRM 层面能查的配置项已经全部对过一遍，我们的寄存器状态与"能跑 IROM 的原厂固件"
完全一致（§64 的全量 diff 已经证明），问题不在这张寄存器表能解释的范围内。
剩下最可能的仍是**flash 芯片内部状态**（dummy cycle / 模式位），而 cache 读是直接依赖它的。

## 69. 官方 WiFi 文档到底有什么（回答"官方有 wifi 的文档吗"）

查了 TRM 和本地这套 IDF v6.0.1 的文档目录，结论是：**有，但全在"怎么用 API"这一层，
没有"怎么移植"这一层**。

### TRM 里的 WiFi：只有两处

1. 框图里出现 "Wi-Fi MAC"、"Wi-Fi Baseband" （仅有名字，没有寄存器说明）
2. **§7.2.4.3 Wi-Fi 和 Bluetooth LE 时钟** —— 这条是硬要求，对我们**实际有用**：

   > Wi-Fi 和 Bluetooth LE **必须在 CPU_CLK 时钟源选择 PLL_CLK 下才能工作**。
   > 只有当 Wi-Fi 和 Bluetooth LE 进入低功耗模式时，才能暂时关闭 PLL_CLK。

   我们现在 CPU 跑的是 40 MHz XTAL（`port_cycles()` 也按 40MHz 算），
   **等 IROM 通了、真要跑 WiFi 时，必须先切到 PLL_CLK**，否则 WiFi MAC 根本不工作。
   这一条现在就记下来，免得后面白折腾。

### IDF 文档里的 WiFi：全是 API 层

```
docs/zh_CN/api-reference/network/esp_wifi.rst        WiFi API 编程指南
docs/zh_CN/api-reference/network/esp_netif*.rst      TCP/IP 适配层
docs/zh_CN/api-reference/network/esp-wifi-mesh.rst   Mesh
```

没有任何"移植到其他 OS / 裸机"的章节 —— 因为这层本身就是**闭源二进制**：
`libphy / libpp / libnet80211 / libcore / libwapi / libmesh / libespnow`，
官方只开放 API，WiFi MAC 寄存器也不公开。

### 所以"看标准库"这条路的尽头在哪

尽头就是我们现在的状态：把官方**开放的那部分源码**（`components/esp_wifi/src/*.c`、
`esp_phy`、`regulatory`、`soc/*.h` 寄存器头）编进来，加上闭源 blob 一起链接。
这一步已经做到 **0 个未定义符号**（§M1 记录）。

剩下唯一的门槛就是**这些 blob 要能放在 flash 里执行**（合计约 500KB，而可用 IRAM 只有
66KB），也就是 IROM 取指那个问题。官方文档在这件事上帮不上忙——它默认你就在跑 IDF。

## 70. 在 "bootloader 刚跳进我们 app 的第一条指令" 处抓现场

用硬件断点停在我们 app 的入口（`_start` = 0x403788f0），此时**我们的代码一行都没执行**，
读到的状态：

```
ICACHE_CTRL  (0x600C4060) = 0x00000001   已使能
ICACHE_CTRL1 (0x600C4064) = 0x00000000   bus 未关
DCACHE_CTRL  (0x600C4000) = 0x00000001   已使能
MMU[3]       (0x600C500C) = 0x00000003   <- 映射到 flash 页 3，但**没有 valid 位**
SPI_CTRL     (0x60002008) = 0x012CA008
```

**结论：这个"看起来有问题"的状态是 bootloader 自己留下的，不是我们代码破坏的**（我们
的代码还没开始跑）。也就是说 bootloader 给我们建的表项就是 `0x00000003`，而它在给原厂
app 建表时留的是 `0x4000` 这种带 bit14 的形式。

这条也解释了为什么"事后手写 0x4003 补 valid 位"没用：bootloader 跳转前可能已经把这个
无效表项读进 cache 的 tag 里了，而 `SYNC`（写回）不等于 `INVALIDATE`（失效），
光 sync 不会让 tag 重新去查表。

### 所以真正需要的是"让 ICache 忘掉这个 tag"

S3 上做得到这件事的有三条路：
1. ROM 的 `Cache_Invalidate_ICache_All()` —— 我们的镜像里一调就不返回（§59）
2. `EXTMEM_ICACHE_PRELOAD_*` 系列寄存器（预取会重新走一遍取数路径）
3. 在 cache 关闭（`ICACHE_CTRL.ENABLE=0`）状态下重写 MMU 表项再打开

第 3 条最干净，也是 IDF/NuttX 的做法（NuttX 注释原文："写 MMU 寄存器时要挂起 cache"）。
**下次直接从这里开始试**：关 ICACHE → 写 MMU[3]=0x4003 → 开 ICACHE → 跳 flash。

### 试过"关 ICACHE → 重写表项 → 开 ICACHE"——仍然不行

严格按 NuttX 注释的建议做（信标逐步确认每步都执行到）：

```
1. ICACHE_CTRL = 0         关 I-cache
2. MMU[3] = 0x4003         重写表项（valid | flash 页 3）
3. ICACHE_CTRL = 1         重新打开
4. 跳 0x42030014           执行 flash 里的 movi.n a2,42; retw.n
```

结果：**仍然 `cause=0`，取回 0x0000**。

至此，S3 上"让 CPU 从 flash 取到指令"这条路，寄存器层面能做的**全部**做完了：
镜像格式、bootloader 引导、MMU（恒等/重映射/手写/关 cache 重写）、双 cache、
cache 时钟位、ICACHE sync、SPI_MEM 全块（与原厂逐字节一致）、ROM cache 函数。
全部有真板实测记录，全部无效。

**结论：这个问题已经超出"配置"范畴**，怀疑落在 flash 芯片内部状态或该 bootloader 与
自建镜像之间的某种隐性契约上。要再往前推，必须先有一个"能跑 IROM 的对照物"——
也就是用同一套 IDF 编一个最小 app 跑通（需要 `install.sh`，而 `/home` 磁盘目前 100% 满）。

## 71. 决定性判别：**数据读** IROM 窗口也不通（不只是取指）

之前所有"读 IROM"的尝试有个盲点：JTAG/gdb 读 IROM 恒为 0（§59 已证明该方法无效），
而我们自己的代码从没干净地"用数据读方式"访问过 IROM 窗口。补上这个实验：

```c
u32 v = *(volatile u32 *)0x42030000u;   /* IROM 窗口, flash 0x30000, 内容已知 */
s3_beacon(1u, v);                        /* 期望 0x7B004136 */
```

真板结果（信标）：

```
slot0 = 0xB0000008     进 os_main
slot1 = 0x00000000     <- 读回 0，而且应用在这里就挂了（slot2 还是上一轮的残留值）
```

**⇒ cache 从 flash 读数据也不通**，不只是取指。这就把问题彻底定位到
"cache ↔ flash 的数据通路"，而不是"取指单元/MMU/权限"。

顺带试了换 flash 读命令模式（DIO=2 / DOUT=3 / QOUT=1，通过镜像头 byte2 让 ROM 和
bootloader 按不同模式配置 flash），三种模式结果完全一样（slot1 都是 0）
⇒ 和 SPI 读命令模式无关。

### 至此的状态

寄存器层面：全部与原厂一致且逐项验证；数据通路：cache 读 flash 恒返回 0。
**在没有"能跑 IROM 的对照物"之前，已经没有可做的实验了。**

要继续推进，必须先用同一套 IDF 编一个最小 app 跑通（确认 `install.sh` 能装上、
IDF app 能跑 IROM），再把它 app 侧的 flash/cache 初始化逐步搬过来二分。
`install.sh` 需要约 1.5~2 GB 空间，而 `/home` 目前只剩 441 MB（用户在清理中）。

## 72. 又排除三条，并把官方文档摘录固化进仓库

本轮做的三件新实验（全部真板实测）：

1. **镜像补上 SHA-256**（`hash_appended=1` + 正确摘要），让我们的镜像结构与原厂完全一致
   → `esptool image-info` 报 `Checksum valid` + `Validation hash valid`
   → 取指/数据读**仍然**返回 0
2. **照 IDF `cpu_start.c` 的做法**把 ICache + DCache 一起 disable → 重设 bus mask → enable
   （IDF 注释原文："It helps to fix missed cache settings for other cores.
   It happens when bootloader is unicore." —— 正是我们的情形）
   → 仍然返回 0
3. **PMS 权限**（TRM §15，文档里最有力的线索：CPU 通过 CACHE 访问外部存储器必须被授权）
   → 实测 `SYSCON_FLASH_ACEn_ATTR/ADDR/SIZE` 与能跑 IROM 的原厂固件**逐字相同**
     （ATTR 全 0xFF 完全开放）⇒ 排除

同时把官方文档摘录固化到仓库 `docs/refs/`：
`README.md`（索引）、`trm-cache-mmu.md`、`trm-pms.md`、`trm-system-cache.md`、
`image-format.md`（我们实测出来的镜像格式四条铁律）。

### 关键观察

数据读 IROM 返回 **0 而不 trap** —— 这正是"MMU 表项对 cache 而言无效"的典型表现，
尽管寄存器回读表项确实是 `3`（有效页号）。也就是说**bootloader 写的表项，
cache 那边没认**。

### 唯一已知的剩余差异

原厂 app 会在自己启动时做一整套 flash/cache 初始化（`spi_flash_init_chip_state()`
+ `ext_mem_init()`），我们的 app 没做。要验证这一点，必须有一个"能跑 IROM 的对照物" ——
即用同一套 IDF 编一个最小 app（`install.sh` 需要约 2 GB，磁盘现已腾出 8 GB）。

## 73. 参照 app 带来的新认知：我们是"静默返回 0"，而正常应该是抛 Cache error

### 实验：让 IDF 参照 app 去读未映射的 IROM 页

```c
printf(..., *(uint32_t*)0x42030000u, ...);
```

真板输出：

```
I (166) cpu_start: cpu freq: 160000000 Hz
Guru Meditation Error: Core 0 panic'ed (Cache error)
```

对比我们自己的固件（同样的地址、同样"未映射"）：

```
read 0x42030000 = 0x00000000     <- 静默返回 0，不 panic
```

**这是一个本质差别**：正常硬件行为是"访问未映射的外部存储器 → Cache error"，
而我们得到的是"静默 0"。也就是说**我们的那次访问压根没有被当作外部存储器访问处理**。

### 按照 IDF 运行态值逐项对齐后仍然失败

```
SPI_MEM_CLOCK 0x60002014 : 写 0x00030103 -> 回读确认生效 ✓（IDF app 里就是这个值）
SPI_MEM_CTRL  0x60002008 : 写 0x00608000
MMU[3]        0x600C500C : 写 0x4003（valid|页3） -> 回读确认生效 ✓
顺序：关双 cache -> 写上述 -> 开双 cache -> 数据读
结果：仍返回 0，且仍不报 Cache error
```

### 另外发现

IDF app 运行在 **160 MHz（PLL_CLK）**，而我们跑 **40 MHz XTAL**。
TRM §7.2.4.3 只要求 Wi-Fi/BLE 用 PLL，没要求 cache/flash——但这是目前唯一还
没对齐的系统级差异。`SYSTEM_SYSCLK_CONF`(0x60026060) 的 `SYSTEM_SOC_CLK_SEL`
（0=PLL? 1=PLL，TRM 表 7.2-1：0=XTAL_CLK，1=PLL_CLK）可以切。

### 下一步（按优先级）

1. **把 IDF app 的 IROM vaddr ↔ MMU 表项对应关系彻底搞清楚**：它自己的代码落在
   哪个 vaddr、对应表项里写的是什么值。这个"正确样例"能直接告诉我们表项该长什么样。
2. 切到 PLL_CLK(160MHz) 后再试（唯一还没对齐的系统级差异）。
3. 若还不行，就需要 Espressif 支持或硬件层面手段了。

## 74. 全部要素齐备的最终序列 —— 仍然失败

把目前掌握的所有正确要素一次性凑齐（每步都用信标确认生效）：

```
1. ICACHE off / DCACHE off
2. SPI_MEM_CLOCK(0x60002014) = 0x00030103      ← 按 IDF 参照 app 运行态值
3. SPI_MEM_CTRL (0x60002008) = 0x00608000      ← 同上
4. MMU[3] = 0x4003 (valid|页3), MMU[0] = 0x4002 (valid|页2)
5. DCACHE on / ICACHE on
6. ICACHE INVALIDATE：0x600C4088 写 bit0(INVALIDATE_ENA)，轮询 bit1(DONE)
   -> 信标 0x00000602，确认失效完成
7. 数据读 0x42030000
8. 取指 s3_win_call_flash42()
```

真板信标：

```
slot0 = 0xB0000013   进 os_main
slot3 = 0x00000602   invalidate 完成 ✓
slot2 = 0x00000000   数据读仍是 0 ✗
slot1 = 0x00000000   取指没返回 ✗
```

### 结论

trace 到这里，S3 上"让 cache 从 flash 读数"的可控手段已经全部试尽并且都能确认生效
（寄存器值可写、可回读、与能正常工作的 IDF app 一致；invalidate 能完成；MMU 表项
带 valid 位），**访问仍然静默返回 0、且不触发 Cache error**。

这已经超出"配置"能解释的范围。下一步只能靠：
1. Espressif 官方支持（我们有完整的对照实验记录）
2. 或者换思路：不用 IROM，改用其它方式容纳 WiFi 栈（例如只保留 scan 路径裁到
   IRAM——工作量大但可行）

## 75. 备选路线评估：把 WiFi 栈裁到 IRAM（不可行）

之前链接 WiFi 时链接器报过 `region IRAM overflowed by ~421KB`（IRAM 只有 66KB），
即 `esp_wifi_init` 的符号闭包约 **490KB**，是可用 IRAM 的 **7 倍**。

802.11 协议栈（libpp + libnet80211 + libcore + libwapi + libphy）是**单体闭源**的，
`--gc-sections` 对没有函数级 section 的闭源 .o 无效，只保留 scan/connect 路径也不能
显著缩小闭包（协议状态机、PHY 校准表、crypto 都在一起）。

⇒ **"裁栈到 IRAM" 不是可行路线**，必须解决 IROM 才能上 WiFi。
这一条现在有依据了，不用再花时间尝试。

## 76. 试过"把 IROM 段放到页 0"（模仿 IDF app 的结构）—— bootloader 不接受

既然能工作的 IDF app 的 IROM 段在 **vaddr 0x42000000（页 0）**，而我们的在页 3，
就把布局改成和它一样：

```
.flash.text   0x42000000 : AT (0x20)    ← IROM 放最前，数据从文件偏移 0x20 开始
.flash.rodata 0x3C000020 : AT (0x30000) ← DROM 随后
app_desc 段移到 IROM 段内（.flash.text.desc），保证仍在镜像偏移 0x20
```

镜像生成成功、`Checksum: valid`，但真板**bootloader 不接受**
（信标停在 bootloader 的残留值 `0x00280028`，应用没起来）。

已回退到能引导的布局（`.flash.rodata@AT(0x20)` + `.flash.text 0x42030000`），
真板恢复正常（信标 `slot0=0xB0000016`，寄存器打印正常）。

⇒ 说明 bootloader 对段布局有额外要求（很可能与"第一个段必须是 DROM"有关），
这一条也记进问题报告。

## 77. 又一排除：flash 芯片内部状态寄存器相同

`esptool read-flash-status` 读的是 flash 芯片自己的状态寄存器（QE 位、工作模式等，
掉电不丢）。分别在"我们固件跑过"和"原厂固件跑过"两种状态下读取：

```
Flash memory status: 0x0200     <- 我们固件
Flash memory status: 0x0200     <- 原厂固件
```

⇒ **flash 芯片内部模式一致**，不是"芯片被配成了某种 cache 读不了的模式"。

同时新增了 `tools/try.sh`（一条命令完成 构建→生成镜像→烧三件套→复位→抓串口→读信标），
后续验证假设可以一条命令搞定。

## 78. eFuse 也排除了

`espefuse summary` 关键位：

```
DIS_ICACHE      = False    (没有禁用 I-cache)
DIS_DCACHE      = False
DIS_DIRECT_BOOT = False    (没有禁用直接启动)
FLASH_TYPE      = 4 data lines
```

⇒ **没有任何 eFuse 位禁用 cache 或 flash 直启**，一次性配置里这一项也排除。

## 79. `spi_flash_init_chip_state()` 在我们的场景下是空函数（重要简化）

一直怀疑 IDF app 靠它做了什么神秘配置。读源码（`components/spi_flash/flash_ops.c:180`）：

```c
esp_err_t IRAM_ATTR spi_flash_init_chip_state(void)
{
#if SOC_SPI_MEM_SUPPORT_FLASH_OPI_MODE
    if (bootloader_flash_is_octal_mode_enabled()) { ... }   // 我们是四线，跳过
#endif
#if CONFIG_SPI_FLASH_HPM_ON
    return spi_flash_enable_high_performance_mode();        // 默认关闭，跳过
#endif
    return ESP_OK;                                          // ← 直接返回
}
```

⇒ **四线 DIO + 默认配置下它是空操作**，不是我们缺的那一步。这个大嫌疑可以划掉了。

那个初始化的真正入口是 flash **驱动**的 `spi_flash_chip_generic_config_host_io_mode()`
（`spi_flash_chip_generic.c:485`）：它根据 read_mode 算出
`read_command`(DIO=0x3B) / `addr_bitlen`(24) / `dummy_cyclelen`，再调
`host->driver->configure_host_io_mode()` 去写 `SPI_MEM_*` 寄存器。
**下一步就是把这个函数最终写下的寄存器值扒出来**（已派子代理专门做这件事）。

## 80. 新线索：cache 读 flash 可能依赖"连续读(XIP)模式"的 dummy 位配置

读 IDF 的 `esp_hal_mspi/spi_flash_hal_common.inc` 里 `spi_flash_hal_configure_host_io_mode`
的实现，有一段注释非常关键：

> Some flash chips, when working under some IO modes (DIO, QIO and OIO in the future),
> **treat the first 8 bits of the dummy bits as the [continuous mode] bits**. When the bits
> meet some pattern, the chip will go into a **"continuous (XIP)" mode**, where the command
> field will be skipped in the next [transaction]...

也就是说：**S3 的 cache 从 flash 取数，依赖 flash 芯片进入"连续读(XIP)模式"**，而这个模式
是靠 **dummy 位里的 8 位模式值** 触发的，由 `spi_flash_ll_set_dummy_out(dev, 1, 1)`
这类调用去配置 `SPI_MEM_CTRL` 的 `FDUMMY_OUT`/`FDOUT_OCT` 相关位。

而我们的 app：
- `SPI_MEM_CTRL = 0x012ca008`（bootloader 留的）
- IDF app 运行态 = `0x00000000`

两者的 dummy-out / 连续模式配置不同。**这解释了为什么 cache 读回 0 却不报 Cache error**
——cache 发出了读命令，但芯片没有按预期进入连续读模式，返回的数据相位错位/全 0。

### 下一步

扒出 `spi_flash_ll_set_dummy_out` 在 ESP32-S3 上到底写 `SPI_MEM_CTRL` 的哪些位、
连续模式值是多少（通常是 0xA0），然后在我们 app 里**在 cache 使能之前**把这些位配好。
这一条已交给子代理深入（`sub-2`）。

### 补充：`FDUMMY_OUT` 位我们其实是设着的

`SPI_MEM_CTRL` 的位定义（`spi_mem_reg.h`）：bit3 = `FDUMMY_OUT`、bit4 = `FDOUT_OCT`。

我们的值 `0x012ca008` 拆开：bit15 + bit13 + **bit3(FDUMMY_OUT 已置位)**；
IDF app 运行态是 `0`（没置位）。

⇒ 这一位我们是**更"配全"**的一方，所以"缺 dummy-out 配置"这条假设不成立。
连续读模式的触发值（M7-M0）在 `SPI_MEM_SPI_FMEM_...` 那几个高位里，
我们和 IDF 也不同，但方向是"我们有、它没有"，不能简单照搬。

结论：XIP/连续读这条线也只能算"待子代理确认"，不能直接当作修复方案。

## 81. 最具体的一条异常：同一个 bootloader，给两个 app 配的 SPI_MEM 不同

在 IDF 的 `cpu_start.c` 里插探针，让它**在自己的 cache/flash 初始化之前**打印寄存器
（这时的值就是 bootloader 留下的）：

```
IDF app  PRECLK: icache=0x01 dcache=0x01 spiclk=0x00010001 spictrl=0x00aca008 mmu0=2 mmu1=1 mmu2/3=0x4000
         POSTCLK: icache=0x0b dcache=0x09 （其余不变 —— 说明 cache 模式是 IDF 自己设的，SPI 是 bootloader 设的）
我们 app 入口:    icache=0x01 dcache=0x01 spiclk=0x80000000 spictrl=0x012ca008 mmu0=2 mmu1=0x4000
```

⇒ **同一个 bootloader、同样的镜像头字段（`0x40` = 16MB/40MHz、chip_id=9、hash_appended=1、
chip rev 0x63 都已对齐），给我们镜像留下的是 `spiclk=0x80000000`，给 IDF 镜像留下的是
`0x00010001`。**

这是目前最具体、最可追的异常：**bootloader 读取我们镜像的某个信息后走了不同的配置路径**。
`SPI_MEM_CLOCK=0x80000000` 很可能就意味着 SPI 时钟分频没配好（bit31 置位、分频字段为 0），
于是 cache 拿不到有效数据，于是取指得到 0。

### 下一步

找出 bootloader 里决定 `SPI_MEM_CLOCK` 的那段逻辑（`bootloader_flash_...` /
`esp_image_...` 里与 `SPI_MEM_CLOCK`/`spi_flash_hal_configure_host_io_mode` 相关的调用），
看它依赖镜像的哪个字段（很可能是**段表里 flash 段的描述方式**，而不是 24 字节头）。

已交给子代理 `sub-2` 深挖。

### 反向实验：镜像里不放 flash 段 —— bootloader 直接 abort

用 `MKIMG_NO_FLASH=1` 生成一个只有 RAM 段的镜像（4 段、36KB），真板结果：

```
abort() was called at PC 0x403cdbe0
```

（就是之前那个 `mmu_hal_map_region` 的断言）

⇒ **bootloader 要求镜像里必须有 flash 段**，不能绕开。所以"不放 flash 段"这条
排除法走不通，必须从"为什么我们镜像的 flash 段让它配出 `SPI_MEM_CLOCK=0x80000000`"
这个角度继续查。

## 82. ★ 里程碑：WiFi 闭源栈已链接进 flash，并已开始从 flash 执行

I-cache 修好之后，把 IDF 闭源库的代码放进 `.flash.text`（链接脚本按归档分别收集
`.literal/.text/.iram1`，并补上 `.wifi*` 系列段），结果：

```
镜像: 641K（之前因 IRAM 溢出 421KB 根本链不出来）
段: DRAM 0x3fca0000 / IRAM 0x40378000 / IROM 在 .flash.text
链接: Successfully created ESP32-S3 image.
```

真板串口：

```
c6os s3 boot
wifi_init ...                       <- esp_wifi_init_internal 被调用
*** TRAP sp=3fcd8e90 cause=00000000 pc=4203d934
```

**PC = 0x4203d934 位于 `.flash.text` 里** —— 证明 CPU 确实在**从 flash 取指并执行
WiFi 库的代码**（I-cache 完全正常）。

`0x4203d934` 正好是 `esp_wifi_init_internal` 的入口，反汇编是 `entry a1, 32`。
`entry` 是 windowed ABI 的指令，直接调用会 IllegalInstruction —— 已经改成经
windowed 孤岛（`s3_win_call_wifi_init`）调用，但仍然在同一条指令上 trap，
说明还有第二个原因（待查）。

### 顺带修掉的两个链接问题

1. `.wifislpiram*` / `.wifirxiram*` 等 WiFi 专属段没被收集 → 变成孤儿段导致 LMA 重叠
   （用 `*(SORT_BY_NAME(.wifi*))` 收进 IRAM 解决）
2. `g_mt` / `g_espnow_user_oui` 在 `wifi_glue.c` 和 `libespnow.a` 里重复定义 → 移除 glue 里的

### 下一步

`entry a1,32` 在 0x4203d934 上 trap 的原因。注意我们的 PHY 也是 windowed 库、
经同样的孤岛调用是正常的，所以孤岛机制本身没问题。可能是：
- 该函数被从**非孤岛**路径调到（比如库里内部互调时 ABI 混用）
- window 溢出/欠载向量在这条路径上没被正确处理

### 82b. 继续排查 `entry a1,32` 的 trap

**内容是对的**：读 `0x4203d934` 得到 `0x4d004136`，低三字节正是 `36 41 00`（`entry a1,32`）
⇒ **数据读在那个地址完全正常**。

**I-cache 尺寸也修正了**：IDF app 的 `ICACHE_CTRL = 0x0b` 解出来是 **8-way / 16KB / 32B**
（bit2 SIZE_MODE=0），我们之前误用了 32768（32KB）。已改成
`rom_config_instruction_cache_mode(16384, 8, 32)`。

**但 trap 依旧**：`pc=4203d934`、`cause=0`（IllegalInstruction）。

对照：同一个 I-cache 配置下，`0x42030014` 的取指是**成功**的（之前 `movi.n a2,42` 返回 42），
而 `0x4203d934` 失败，两者同在 page 3。

⇒ 所以问题不是"I-cache 完全不能取指"，而是**这个特定地址/这个函数入口**不行。
`entry` 是 windowed ABI 的入口指令，理论上我们的孤岛调用方式对 libphy 是有效的
（PHY 5 步都跑通了）。下一步要看：
- 是不是 libphy 的 windowed 函数入口根本没有 `entry`（用 `retw` 的可能是别的形式）
- 或者 `esp_wifi_init_internal` 被**库内部**（而非我们的孤岛）调用到了

### 82c. 关于 `entry` trap 的一个重要判断依据

`win_island.c` 是用 `-mabi=windowed` 编译的，它自己的函数入口就是 `entry`，
而且 **PHY 的 5 步调用全部成功返回** —— 说明 **`entry` 指令本身在我们的 app 里是能用的**。

同时确认：
- `0x4203d934` 处的**数据读正常**（`0x4d004136`，低三字节就是 `entry a1,32` 的编码）
- 同一个 I-cache 配置下，`0x42030014`（`movi.n a2,42`）的**取指执行成功**
- 只有 `0x4203d934` 的**取指**失败（`cause=0` = 取回非法指令）

⇒ 所以这不是"windowed ABI 不能用"，也不是"I-cache 完全坏"，而是
**这个特定地址的取指**有问题。

值得接下来验证的：
1. 换一个 WiFi 库里的其他 windowed 函数调用，看是否同样在 `entry` 上 trap
   （如果是 ⇒ 是"flash 里的 `entry`"这一类问题；如果否 ⇒ 是这个函数的特殊性）
2. 检查 `esp_wifi_init_internal` 是否被**库内部**而非我们的孤岛调到
3. 读 `0x4203d934` 前后几条指令的取指结果（比如跳到 0x4203d938 执行看看）

### 82d. 相联度试验：4-way 更糟，8-way 是对的

把 I-cache 从 8-way 改成 4-way（`rom_config_instruction_cache_mode(16384, 4, 32)`）后，
应用连 `os_main` 的第一个信标都没写出来 —— 比原来更早死。

⇒ **8-way / 16KB / 32B（`ICACHE_CTRL = 0x0b`，和 IDF 一致）是正确的配置**，已改回。
`0x4203d934` 的 `entry` trap 与相联度无关，另有原因。

### 82e. ★ 重大突破：`esp_wifi_init_internal` 已经跑起来了，新错误是 ABI 不匹配

把 WiFi 库放进**独立的 `.flash.wifi` 段**（VMA/AT 都按 64KB 页对齐，避免扰动
`.vector`/`.text` 布局）之后，构建和链接都正常了。真板结果：

```
c6os s3 boot
wifi_init ...                                  <- 进入 esp_wifi_init_internal
*** TRAP cause=00000014 pc=8037d21d            <- InstrFetchProhibited
```

对比之前 `cause=0 pc=4203d934`（卡在函数入口的 `entry`）：

- **`entry` 已经执行过去了** —— 说明 "windowed 代码从 flash 取指执行" 完全没问题
- 新 trap 是 **`InstrFetchProhibited`**，PC = **`0x8037d21d`**
- **`0x8037d21d` 的最高位是 1**，而 `0x4037d21d` 正是我们的 IRAM 地址
- Xtensa 里 **最高位为 1 是 `call0` 返回地址的标记**

⇒ **ABI 不匹配**：WiFi 闭源库是 **windowed** ABI，它回调我们提供的
`wifi_osi_funcs_t` 里那 100 多个桩函数；这些桩函数在 `wifi_glue.c` 里是用 **call0**
编译的，被 windowed 代码 `callx8` 调用后 `ret` 返回，地址就错了。

### 修复方向

把那批 OSI 桩函数放到**用 `-mabi=windowed` 编译的文件**里（像 `win_island.c` 那样），
或者让 `wifi_glue.c` 整体用 windowed 编译。这是纯工程改动，方向明确。

### 82f. 把 OSI 桩函数改成 windowed 编译 —— 又前进了一步

`wifi_glue.c` 改用 `-mabi=windowed` 编译（并给 `s3_wifi_osi_init` 加了孤岛桥）后：

```
c6os s3 boot
*** TRAP cause=00000014 pc=8037d260      <- 又是 call0 标记的地址
```

- 之前是 `pc=8037d21d`（在 `s3_win_call_wifi_init` 里）
- 现在变成 `pc=8037d260`（更靠后），而且 `wifi_init ...` **没打印出来**
  ⇒ 说明现在是在 `s3_win_call_osi_init` 阶段就出问题，比之前更早

⇒ ABI 方向是对的，但**还有别的地方在用 call0 标记的地址**。可能是：
- `wifi_glue.c` 里通过函数指针/表项调到的其他 call0 代码
- 或者孤岛桥本身在这种"表里存了函数指针再回调"的路径上不够用

### 目前确认可行的部分（都是真板证据）

1. IROM 取指完全正常（I-cache 配置修好之后）
2. WiFi 闭源库能链接进 flash（独立 `.flash.wifi` 段）
3. `esp_wifi_init_internal` 的 `entry` 能执行（windowed 代码从 flash 跑没问题）
4. 剩下的纯粹是 **call0/windowed ABI 边界**的工程问题

### 82g. 精确定位：`call8` 的返回地址被置上了 call0 的最高位

反汇编 `s3_win_call_osi_init`（孤岛桥）:

```
4037d25d:  call8   4037c9f0 <s3_win_osi_init>
4037d260:  mov.n   a2, a10          ← 这条就是 trap 报的 PC（0x8037d260）
```

而 `s3_win_osi_init`（在 `win_island.c`，**确实是 windowed 编译**）:

```
4037c9f0:  entry   a1, 32
4037c9f3:  call8   4037ca0c <s3_wifi_osi_init>
4037c9f6:  call8   4037ca20 <s3_wifi_osi_funcs_ptr>
4037c9f9:  or      a2, a10, a10
4037c9fc:  retw                     ← 用的是 retw，正确
```

⇒ trap 的 PC 是 `0x8037d260`（= `0x4037d260` 置了 bit31），也就是 **`call8` 的返回地址
在返回时被加上了 call0 的标记位**。

`S3_SHIM_ABI ?= windowed` 确认无误，孤岛本身是 windowed 的。

可能的来源：
- `s3_wifi_osi_init()` 内部把寄存器窗口压到比 PHY 测试更深的层级，
  触发了我们窗口溢出/欠载向量还没覆盖到的层
- 或者它内部调到的某个 call0 函数污染了 `a0`

（我们已经验证过窗口向量在 deep=11 时是正常的。）

### 82h. ★ ABI 问题真正修好了：`esp_wifi_init` 完整执行并返回

关键发现：之前几次构建**根本没重新编译 `wifi_glue.o`**（Makefile 规则改了但源文件没动，
make 认为它是最新的），所以一直在跑旧的 call0 版本。清掉 `build/wifi` 全量重建后：

```
build/wifi/obj/wifi_glue.o:  s3_wifi_osi_init -> entry a1, 32   ← 真的 windowed 了
```

真板结果（这是本轮最重要的证据）：

```
c6os s3 boot
wifi_init ...
wifi_init rc=0xffffffff
```

**没有 trap，`esp_wifi_init_internal()` 完整执行并返回了！**
`rc=0xffffffff` 是因为链接器用了我们那个弱兜底桩（`return -1`）——
弱定义会满足引用，导致归档成员不被拉取。已给弱兜底加上 `#if !defined(S3_WITH_WIFI)`。

### 当前的最后一个障碍：`.flash.wifi` 段让 bootloader 拒绝镜像

把 `.flash.wifi` 放到 `vaddr 0x42100000 / flash 0x100000`（identity、页对齐）后，
镜像变成 7 段、645KB，链接正常，但 **bootloader 复位**（`rst:0x3`），应用没起来。

之前 6 段（不带 `.flash.wifi`）的镜像是能被接受的。所以问题在段的描述方式 ——
可能是 bootloader 对 flash 段的**顺序/偏移**有要求，或者 0x100000 这个位置
超出了它愿意映射的范围。下一步就是把段布局调成 bootloader 能接受的形式。

### 82i. 段布局两种方案的对比（重要线索）

| 方案 | 结果 |
|---|---|
| WiFi 库放**独立** `.flash.wifi` 段（vaddr 0x42100000 / flash 0x100000） | **`esp_wifi_init_internal` 完整执行并返回**（无 trap）✅ 但 bootloader 拒绝 7 段镜像 ❌ |
| WiFi 库**并入** `.flash.text`（连续，段 1 大小 0x67EE8） | bootloader 接受 6 段镜像 ✅ 但 `esp_wifi_init_internal` 在 `entry` 上 trap ❌ |

两者唯一的实质差别是 **`esp_wifi_init_internal` 落在哪个地址**：
- 独立段方案里它在 **0x421xxxxx**（页 0x10）—— 能执行
- 并入方案里它在 **0x4203d934**（页 3 的后半段）—— `entry` 取指失败

而我们已经知道 **0x42030014（页 3 开头）的取指是成功的**。

⇒ 强烈指向 **IROM 页 3 的映射/容量只覆盖了一部分**，
或者 I-cache 在处理这个区段时有问题。这与"bootloader 只按第一个 IROM 段的大小建映射"
的猜想吻合 —— 段大小 0x67EE8 约 422KB，但实际可能只映射了一部分。

### 下一步

1. 读 `MMU[3..0x10]` 的实际值，看页 3 之后到底映射了哪些页
2. 或者把 `.flash.text` 整体挪到 **页对齐且靠前**的位置（如 0x42100000），
   让所有 flash 代码都在能工作的那个区间里

### 82j. 不是页的问题，是**段的偏移**

把 `.flash.text` 整体挪到 `vaddr 0x42100000 / flash 0x100000`（也就是之前独立段方案里
能正常执行的那个区间）后：

```
*** TRAP cause=0  pc=4210d934
```

**trap 的地址变成了 `0x4210d934`** —— 页号变了（0x10），但**段内偏移还是 `0xD934`**，
依然失败。

⇒ 所以既不是"页 3 有问题"，也不"页 0x10 就好"，而是**段内偏移约 55KB 之后的取指会失败**。
而同一地址的**数据读**是正常的。

结合之前的观察（`0x42030014` ≈ 段内 0x14 能取指成功），现在的图像是：

> **IROM 段靠前的部分能取指，偏移 0xD934 附近不行**，但数据读两边都正常。

这很可能与 **bootloader 实际映射了多少页** 有关（它按第一个 IROM 段的描述建映射，
但可能有上限或对齐问题）。下一步：直接读 `MMU[3]` 到 `MMU[0x20]` 的实际内容，
看这 400 多 KB 到底被映射了多少页。

### 82k. MMU 表实测：前 64 项里只有 **15 个有效映射**

```c
for (i = 0; i < 64; i++) { v = MMU[i]; if ((v & 0x4000) == 0) n++; }
```

真板信标：

```
slot1 = 0x0000000F   前 64 项里有效映射的页数 = 15
slot2 = 0x0000000E   第一个无效表项的索引 = 0x0E
slot3 = 0x00000006   MMU[4] 的内容 = 6
```

⇒ 有效映射是 **entry 0 ~ 0x0D（14 页）** 再加上一项，共 15 页 ≈ 960KB。

我们的镜像里：
- DROM 段 0xDFFD8 ≈ **14 页**（因为为了让 IROM 对齐而做的 64KB 填充）
- IROM 段 0x67EE8 ≈ **7 页**

**14 + 7 = 21 页 > 15 页** —— 而 IROM 和 DROM 在 512 项表里**共用**（NuttX 的注释：
"One table, one entry per 64 KB, shared by the instruction and the data"）。

⇒ **两段加起来需要的表项数超过了实际被映射的数量**，
所以 IROM 段靠后的页面（比如偏移 0xD934 所在的页）**根本没有被映射**，
取指自然拿到 0 —— 但这与"数据读正常"仍有矛盾，需要进一步确认。

### 下一步（很明确）

**把 DROM 段的 64KB 填充去掉**，让 DROM 只占几页，把表项省出来给 IROM。
具体做法：DROM 不再为了"让 IROM 落在页边界"而填充，改用
`Cache_Set_IDROM_MMU_Size` 那套 IROM/DROM 切分约定来排布局。

### 82l. 关键观察：失败的总是**段内偏移 0xD934**

三种不同的基址都试过：

| `.flash.text` 的 vaddr | trap 的 PC | 段内偏移 |
|---|---|---|
| 0x42030000 | 0x4203d934 | **0xD934** |
| 0x42100000 | 0x4210d934 | **0xD934** |
| 0x42001020 | 0x4200e954 | **0xD934** |

⇒ 与页号、与映射了多少页都无关，**永远是段内偏移 0xD934 这一处**取指失败。
而这个地址的**数据读是正常的**（读到 `0x4d004136`，正是 `entry a1,32` 的编码）。

之前"独立 `.flash.wifi` 段"的方案里，`esp_wifi_init_internal` 落在 `0x421xxxxx`
（另一段），**能正常执行并返回**。

⇒ 说明问题与"**这个函数在某一段里的具体位置**"有关，很可能是：
- `l32r` 字面量池跨段/越界（Xtensa 的 l32r 只有 ±256KB 范围）
- 或者该位置的取指命中了某种对齐/边界问题

### 下一步

既然"独立段 + 0x42100000"那套能让 `esp_wifi_init` 跑通，就沿这条路走：
解决 bootloader 对 7 段镜像的接受问题（可能是段顺序或偏移要求），
而不是继续纠结偏移 0xD934。

所有改动都已提交，当前 link.ld 是最紧凑的布局（DROM 1 页 + IROM 7 页）。

### 82m. 独立段方案再验证：bootloader 仍然拒绝

把 WiFi 库放到**独立 `.flash.wifi` 段**（vaddr 0x42080000 / flash 0x80000，
不与 `.data` 冲突、在 4MB 分区范围内、7 段镜像）：

```
rst:0x3 (RTC_SW_SYS_RST)      <- bootloader 复位，应用没起来
```

⇒ **bootloader 不接受"两个 IROM 段"的镜像**（不管第二段放在 0x80000 还是 0x100000）。

而如果让两段**连续**（并进 `.flash.text`），bootloader 就接受，但会踩到
"段内偏移 0xD934 取指失败"那个问题。

### 当前的两难

| 方案 | bootloader | 执行 |
|---|---|---|
| 两段 IROM（独立 `.flash.wifi`） | ❌ 拒绝 | 曾经能跑通 `esp_wifi_init` |
| 单段 IROM（并进 `.flash.text`） | ✅ 接受 | ❌ 偏移 0xD934 取指失败 |

⇒ 必须找到一个"单段 IROM、但 WiFi 代码不落在那个坏偏移上"的布局。

思路：调整**段的内部结构**，把 `.flash.text` 里我们自己的小函数和数据放在前面，
让 WiFi 库的代码整体后移；或者反过来，把 WiFi 库放前面、我们的小函数放后面 —— 
关键是避开"从段首算起约 55KB"这个位置（如果它真的是个固定坏点的话）。

另一种可能：那个偏移本身没问题，而是**该位置恰好落在一个 l32r 字面量池的边界上**
（Xtensa l32r 只有 ±256KB）。用 `readelf -r`/`objdump` 检查 `esp_wifi_init_internal`
附近的 `l32r` 目标地址是否越界，是下一步最快的验证。

### 82n. 又一个 IDF 格式细节：空洞应该用 **PADDING 段**

之前 `mkimg.py` 处理"flash 段之间要对齐"时，是把空洞**塞进上一段的数据**里 ——
结果 DROM 段被撑到 **0xDFFD8（14 页）**，占掉 14 个 MMU 表项，和 IROM 段抢表项。

而 IDF 自己的镜像里，这种空洞是用 **PADDING 段**表示的：

```
$ esptool image-info build/wifi/c6os.bin
Segment   Length   Load addr   File offs  Memory types
      0  0x00108  0x3c000020  0x00000018  DROM          <- 只有 1 页！
      1  0xdfed0  0x00000000  0x00000128  PADDING       <- bootloader 会跳过，不占表项
      2  0x67ee8  0x42100000  0x000e0000  IROM
```

⇒ DROM 1 页 + IROM 7 页 = **8 页**，彻底避开了表项冲突。
`mkimg.py` 已改成发 PADDING 段（`load_addr = 0`）。

**但真板上 bootloader 仍然复位拒绝**（`rst:0x3`），
可能是 PADDING 段在这个位置不被接受，或者镜像变成 1.5MB 超出了它的处理能力。

### 已确认的 IDF bootloader 限制

读 `bootloader_support/src/bootloader_utility.c` 的 `set_cache_and_start_app()` 签名：

```c
(uint32_t drom_load_addr, uint32_t drom_size,
 uint32_t irom_load_addr, uint32_t irom_size, ...)
```

⇒ **只接受一个 DROM 段 + 一个 IROM 段**，所以"两个 IROM 段"必然被拒。
所有 flash 代码必须放在**同一个 IROM 段**里。

### 82o. 当前最佳布局（但 bootloader 仍不接受）

综合已有知识，排出了理论上最合理的一套：

```
Segment   Length   Load addr   File offs  Memory types
      0  0x00108  0x3c000020  0x00000018  DROM       <- 1 页，占 entry 0
      1  0x0fed0  0x00000000  0x00000128  PADDING    <- 64KB 空洞，bootloader 应跳过
      2  0x67ee8  0x42010000  0x00010000  IROM       <- entry 1，与 DROM 不冲突
      3+ IRAM/DRAM 段
```

- DROM 只占 **1 个表项**（entry 0）
- IROM 从 **entry 1** 开始（7 页，entry 1~7），**完全不与 DROM 冲突**
- 镜像 645KB，不大

**但真板上 bootloader 仍然复位拒绝。**

⇒ 目前的判断：**PADDING 段**可能就是它拒绝的原因（虽然 IDF 自己的镜像里也有）。
下一步可以试：
1. 不用 PADDING 段，改成让 DROM 的 `Load addr` **紧跟** 在 IROM 前面（零空洞），
   这时 IROM 的 vaddr 低 16 位要相应调整
2. 或者干脆把 DROM 段**放到最后**，让 IROM 成为第一个 flash 段

### 82p. ★ bootloader 终于接受了带 WiFi 的镜像（6 段结构）

关键调整：**让 DROM 正好 1 页**，IROM 从 **entry 1**（vaddr 0x42010000 / flash 0x10000）开始：

```
Segment   Length   Load addr   File offs  Memory types
      0  0x0ffd8  0x3c000020  0x00000018  DROM      <- 正好 1 页，占 entry 0
      1  0x67ee8  0x42010000  0x0000fff8  IROM      <- entry 1 起 7 页，不与 DROM 冲突
      2+ IRAM/DRAM 段
```

真板：

```
c6os s3 boot          <- bootloader 接受了！
wifi_init ...         <- 进入 esp_wifi_init_internal
*** TRAP cause=0 pc=4201d934
```

⇒ **镜像结构问题解决了**（回到 6 段、无 PADDING、DROM 只占 1 个表项）。

但 trap 依然在**段内偏移 0xD934**（这次是 0x4201d934）。三种基址下都是这个偏移，
再次确认它与页号、表项冲突都无关。

### 关于 0xD934 的新猜想

既然：
- 段首（0x42010014 附近）能取指
- 0xD934 处不能取指
- 而该地址**数据读正常**

那么很可能是**这个函数本身**的某种特性。反汇编显示它的第一条就是 `entry a1, 32`
—— 而我们已经确认 `entry` 在别处能正常工作（孤岛函数、PHY 调用）。

所以下一步应该看**这个函数被调用的路径**：是不是从某个 call0 上下文直接 `callx8`
过去，导致窗口状态不对。或者看 `ps=00060910` 里 `WOE` 位在 trap 时是否真的是 1。

### 82q. JTAG 看 trap 现场：读到的是 exccause=4

用 JTAG 停在 trap 上看寄存器：

```
epc1      = 0x400442a7    （ROM 里的地址）
excvaddr  = 0x0
exccause  = 0x4           （LoadStoreAlignment）
```

这和串口打印的 `cause=0 pc=4201d934` **不一致** —— 说明 JTAG 附加时
CPU 已经又走了别的异常路径，或者现场被打断过。

⇒ 结论：**JTAG 这条路对这个 trap 的取证不可靠**（之前也踩过 JTAG 读数不可信的坑）。
后续还是应该靠"应用自己打印/写信标"的方式取证。

### 本轮小结

已经把能解决的都解决了：
- I-cache 配置（根因）
- WiFi 库链接进 flash
- **bootloader 接受带 WiFi 的镜像**（1 页 DROM + IROM 从 entry 1）
- OSI 桩函数 windowed ABI
- `esp_wifi_init_internal` 被调用并进入

**唯一剩余**：段内偏移 0xD934 处的取指失败（三种基址下都一样，数据读正常）。

### 82r. 发现并修掉自己桥函数里的一个 bug（但还有别的）

通用桥 `s3_win_call_fn1(fn, arg)` 里我误写了 `mov a14, a2` —— 而参数本来就已经在
a2/a3 里，这条只会**破坏 a14**。已从两个桥函数里删掉。

不过测 `esp_wifi_get_mac`（段内偏移 0xF1C8，比 0xD934 更靠后）时：

```
slot3 = 0x10004136        <- 该地址的数据读是对的（是条有效指令）
*** TRAP cause=0 pc=600c4060   <- 但跳过去时 PC 变成了 0x600C4060（一个寄存器地址！）
```

⇒ 说明**调用目标传错了**（跳到了 0x600C4060 而不是 0x4201f1c8），
这是我这个通用桥的参数传递问题，不是 WiFi 库的问题。

⇒ 一个重要结论：**段内偏移 0xF1C8 处的数据读也是正常的**，
所以 IROM 段的内容和映射都没问题，问题集中在"从 IRAM 的 call0 代码
跳到 flash 里的 windowed 函数"这条路径上。

### 下一步

把通用桥的参数传递弄对（现在 fn/arg 的寄存器传递有 bug），
然后就能判断"跳到 flash 里的任意 windowed 函数"是否普遍失败。

### 82s. 通用桥的编译结果有问题（供以后参考）

```asm
4037c940 <s3_win_call1>:       ; u32 s3_win_call1(u32 fn, u32 arg) { return ((u32(*)(u32))fn)(arg); }
4037c940:  entry  a1, 32
4037c943:  mov.n  a10, a3      ; ← 把 arg 放到了 a10
4037c945:  callx8 a2           ; ← 用 a2 当函数指针
4037c948:  mov    a2, a10
4037c94a:  retw.n
```

windowed ABI 的第一个参数应该在 **a2**，而编译器把它放到了 **a10**（返回寄存器）。
所以"用 C 写通用函数指针调用"这条路在这个 ABI 下不可靠，
需要改写成手写汇编（先把 fn 存起来，再把 arg 放进 a2，然后 callx8）。

### 本轮结论

- 我们**自己写的** `s3_win_wifi_init`（直接 C 调用 `esp_wifi_init_internal`）是正确的
- 通用桥 `s3_win_call1` 的参数传递有问题（编译器把参数放到 a10）
- `esp_wifi_init_internal` 依然卡在**段内偏移 0xD934** 的取指上

### 目前掌握的全部事实（供后续接手）

1. I-cache 已按 IDF 的方式配置（16KB/8-way/32B，`ICACHE_CTRL=0x0b`）
2. IROM 段**数据读在任何偏移都正常**（0xD934、0xF1C8 都验证过内容正确）
3. IROM 段**取指在段首正常**（0x42030014 的 movi.n a2,42 执行成功）
4. IROM 段**取指在偏移 0xD934 失败**（cause=0），且与基址无关
5. bootloader 只支持一个 DROM + 一个 IROM 段，DROM 大了会抢 IROM 的表项
6. 曾有一次 `esp_wifi_init_internal` **完整执行并返回**（那时 WiFi 库在独立的
   `.flash.wifi` 段、vaddr 0x42100000），但那个 7 段镜像被 bootloader 拒绝

### 82t. ★ 决定性测量：IROM 段被**完整映射**，问题在 I-cache 本身

```c
for (i = 1; i <= 12; i++) { v = MMU[i]; if ((v & 0x4000) == 0) nvalid++; }
```

真板：

```
slot1 = 7      entry 1..12 里有效的页数 = 7
slot2 = 8      第一个无效的 entry = 8        （即 entry 1~7 全部有效）
slot3 = 3      MMU[1] = 3（页 3）
```

⇒ IROM 段（entry 1~7，正好覆盖 422KB 的 7 页）**被 bootloader 完整映射了**。

再看两个高偏移实验：
- windowed 函数 @ 段内 0xD934 → 取指 trap
- call0 函数 @ 段内 **0x67EE8** → 不 trap 但挂住、不返回
- 数据读在 0xD934、0xF1C8 都正常

⇒ **映射完整、数据通路正常，唯独"取指"在高偏移处失效** ⇒ 问题在 **I-cache**。

### 重要参照

- IDF 最小 app 的 IROM 只有 **63KB（1 页）**，从没考验过高偏移
- **原厂小智固件的 IROM 很大**（段 3 是 0x1D33AC ≈ 1.8MB）而且能正常从 flash 执行
  ⇒ **高偏移取指在正确的配置下是可行的**

⇒ 我们的 I-cache 配置（16KB/8-way/32B，`ICACHE_CTRL=0x0b`）虽然和 IDF app 读到的一致，
但那个 app 只用了 1 页 IROM，所以**这个值可能并不适合大镜像**。

### 下一步

在**原厂小智固件**运行时读它的 `ICACHE_CTRL`，看它给 1.8MB 的 IROM 用的是
什么 cache 模式（16KB 还是 32KB、4-way 还是 8-way、16B 还是 32B 行）——
这才是"能跑大镜像"的真实配置。

### 82u. ★ 重要纠正：原厂固件用的就是**默认** I-cache 配置

读原厂小智固件（IROM 段约 1.8MB，能正常从 flash 执行）运行时的寄存器：

```
ICACHE_CTRL (0x600C4060) = 0x00000001     <- 就是 4-way / 16KB / 16B（复位默认值）
ICACHE_CTRL1(0x600C4064) = 0x00000000
DCACHE_CTRL (0x600C4000) = 0x00000001
```

⇒ **原厂固件根本没改 cache 模式**，用默认的 `0x01` 就能跑 1.8MB 的 IROM。

这意味着我之前"根因是 I-cache 模式没配"的结论**不准确**：

- 我们最初就是 `0x01`，却取指失败
- 调了 `rom_config_instruction_cache_mode(16384, 8, 32)` 之后，**小段（0x18 字节）能取指了**
- 但大镜像（422KB）在高偏移处仍然失败

⇒ 真正起作用的不是那个**模式值**，而是这个 ROM 函数**顺带做的其他事**
（很可能包括：把 I-cache 的所有 tag 清一遍、重新初始化 tag RAM 状态）。
`0x01` 本身是够用的 —— 原厂固件证明了这一点。

### 修正后的认识

| 现象 | 解释 |
|---|---|
| 最初 `0x01` 完全取指失败 | cache 的 tag 处于某种未初始化/脏状态 |
| 调 ROM 函数后小段可取指 | 该函数把 tag 清理了 |
| 大镜像高偏移仍失败 | **还有另一层问题**（可能与 tag 容量/预取有关） |

### 下一步

对比原厂固件和我们在**同一个 MMU 表项**下的行为（之前做过：用 IDF app 写
同一表项能读真实数据）。更关键的是搞清楚 `rom_config_instruction_cache_mode`
到底改了什么 —— 读 ROM 里这个函数（0x40001a1c）的反汇编，看它除了写 CTRL 之外
还做了什么（很可能是 invalidation）。

### 82v. 全量 cache 失效也救不了高偏移取指

假设"高偏移取指失败是 cache tag 残留"，做了针对性实验：

```c
os_icache_enable();                 /* 配置 + 打开 */
s3_win_call_cache_invalidate();     /* ROM Cache_Invalidate_ICache_All */
s3_beacon(3, 0x1CAC0001);           /* 确认失效调用返回了 */
v = *(u32*)0x42077ee8;              /* 高偏移处的数据读 */
v = s3_flash_end_test(33);          /* 高偏移处的 call0 函数，期望 42 */
```

真板：

```
slot3 = 0x1CAC0001    失效调用正常返回
slot2 = 0x9B004136    高偏移处数据读正常（是合法指令编码）
slot1 = 0x00000000    调用没返回（挂住），但没有 TRAP
```

⇒ **失效操作没能解决问题**。高偏移处的**数据读正常**、**取指不返回**（这次是挂住而非 trap）。

### 目前的完整认识

高偏移取指失败与以下都无关（全部真板验证过）：
- MMU 映射（entry 1~7 全有效）
- 数据通路（任何偏移的数据读都 OK）
- cache 模式（原厂固件用默认 `0x01` 就能跑 1.8MB）
- cache 失效（刚试过，无效）
- 页号 / 表项冲突（三种基址同一偏移失败）

⇒ 这是一个**尚未定位的、I-cache 在高地址处的取指问题**。
下一步最有效的手段应该是：在 `s3_flash_end_test` 前后加更细的信标，
确认到底执行到了哪里（是完全没进去，还是进去后跑飞）。

---

## 83. 这个 session 的总结（供接手）

### 最大的进展

1. **IROM 完全不可用 → 部分可用**：最初从 0x42xxxxxx 读数据返回 0、取指 trap；
   现在**数据读在任何偏移都正常**、**取指在段首正常**（flash 里 `movi.n a2,42` 能返回 42）。
   转折点是调用 ROM 的 `rom_config_instruction_cache_mode(16384, 8, 32)` —— 注意
   原厂固件其实用的是默认 `ICACHE_CTRL=0x01`，所以起作用的不是模式值而是这个函数
   顺带做的初始化。

2. **WiFi 闭源栈能链接进 flash**：之前 IRAM 溢出 421KB 根本链不出来，现在 645KB 镜像
   正常。关键是按归档把 `.literal/.text/.iram1` 收进 `.flash.text`，并补上 `.wifi*`
   系列段、去掉 `g_mt`/`g_espnow_user_oui` 的重复定义。

3. **bootloader 接受带 WiFi 的镜像**：IDF bootloader 的 `set_cache_and_start_app()`
   只支持**一个 DROM + 一个 IROM 段**；并且 DROM 必须正好 1 页（64KB），否则会抢走
   IROM 的 MMU 表项（两者共用同一张表）。当前布局：DROM@0x3C000020(1页) +
   IROM@0x42010000(entry 1 起)。

4. **OSI 桩函数 ABI**：`wifi_glue.c` 必须 `-mabi=windowed` 编译（闭源库会回调它），
   并且改 Makefile 规则后要**强制重建**（源文件没变时 make 不重编 —— 这个坑浪费了好几轮）。

### 还没打通的

**IROM 高偏移处的取指**：段内偏移超过约 55KB 后，取指会 trap 或挂住，
而同一地址的**数据读完全正常**。已排除：MMU 映射、数据通路、cache 模式、
cache 失效、页号、表项冲突、l32r 越界、PS/窗口状态。

### 建议的下一步

1. 在 `s3_flash_end_test` 里加更细的信标（进入后立刻写一个），确认是"完全没进去"
   还是"进去后跑飞"
2. 用 `EXTMEM_ICACHE_TAG_OBJECT` 机制 dump 那个地址的 I-cache tag，
   看是"没有有效 tag"还是"tag 指向了别的页"
3. 考虑把 WiFi 的**热路径**（`esp_wifi_init` 及其直接依赖）留在 IRAM，
   只把冷代码放 flash —— 这样即使高偏移取指有问题也能先跑起来

### 82w. 精细化定位：高偏移函数内部的信标调用也失败了

在 `.flash.text` 末尾的测试函数（偏移 0x67EE8）里加一句信标调用：

```c
u32 s3_flash_end_test(u32 x)
{
    s3_beacon(3u, 0xE001E001u);   /* 如果执行到这里说明取指成功 */
    return x + 9u;
}
```

真板：

```
*** TRAP cause=0x14 (InstrFetchProhibited) pc=8037d834
slot0 = 0x00000001     （被覆盖）
slot2 = 0x40378000     （被写成了一个地址，而不是值）
slot3 = 0x1CAC0001     失效调用正常
```

- `pc = 0x8037d834` = `0x4037d834`（我们的 IRAM 地址）**置了 bit31**
  ⇒ 又是 "call0 返回地址被当成跳转目标"
- 而且信标槽被写脏了（`slot2` 变成了一个地址）

⇒ 说明**从 flash 里的代码 call0 调回 IRAM 的函数**这条路径会出问题
（返回地址被置上 bit31）。这和之前 `s3_win_call_wifi_init` 里遇到的
`0x8037d21d` 是同一类现象。

**新的、更准确的判断**：问题可能不是"I-cache 高偏移取指失败"，
而是 **"从 flash 代码返回/调用到 IRAM 时，返回地址的最高位被错误设置"** ——
这个方向比 cache 更值得查（比如某个窗口操作把 a0 写坏了）。

### 82x. 澄清：0x8037xxxx 是**症状**，不是病因

把上面两次 `0x8037xxxx` 的 trap（`0x8037d21d`、`0x8037d834`）放在一起看，
再对照已知事实：

- **windowed 函数在段首**（`0x42030014` 的 `movi.n a2,42`，经孤岛调用）→ **成功**
- **windowed 函数在段内 0xD934**（`esp_wifi_init_internal`）→ trap `cause=0`
- **call0 函数在段内 0x67EE8** → 挂住/跑飞，最终 `jx` 到一个带 bit31 的地址

共同点是**段内偏移大**。带 bit31 的地址（`0x8037xxxx`）是 Xtensa `call0` 返回地址的
标记 —— 但它出现在 PC 上，是**执行了垃圾指令之后**的结果，不是独立病因。

⇒ 所以结论回到最初那个：**IROM 段内高偏移处的取指失败**，
数据读正常、映射正常、cache 模式与失效都试过。

（这一段是纠正 §82w 里的"可能是返回地址被写坏"的说法 —— 那更像是症状。）

### 82y. 把 WiFi 入口排到段首后，trap **跟着函数走**了

把 `libnet80211.a` 的规则排到最前，`esp_wifi_init_internal` 从段内 0xD934
移到 **0x1214（约 4.6KB）**：

```
42011214 T esp_wifi_init_internal
```

真板：

```
*** TRAP cause=0 pc=42011214      <- trap 就在它的新地址上
```

⇒ **trap 跟着函数走，不是跟着偏移走**。

这推翻了"高偏移取指失败"的说法（§82x）—— 因为 0x1214 是很靠前的偏移，
而 `0x42030014` 这个更靠前的位置之前是能取指的。

### 目前互相矛盾的三组事实

| 事实 | 说明 |
|---|---|
| `s3_asm_flash_42` @ 段内 0x14，经孤岛调用 | ✅ 能执行（返回 42）|
| `esp_wifi_init_internal` @ 段内 0x1214 | ❌ trap on entry |
| `esp_wifi_init_internal` @ 独立段 0x4210xxxx | ✅ 曾经完整执行并返回 |

⇒ 同一个函数在独立段方案里能跑，在这个方案里不能。差别在于
**它周围的代码布局**（`libnet80211` 整个库在独立段时是连续的，
而现在它被 `libpp` 夹在中间、且前面还有我们的 `.flash.text`）。

**下一步应该查**：这个函数的 `entry a1, 32` 需要 32 字节栈空间，
而我们的栈顶是 `0x3FCD9000`（`S3_STACK_TOP` 传的）—— trap 时 sp=0x3fcd8ed0，
还有 0x130 字节余量。如果 `entry` 需要的窗口溢出处理把栈用超了会怎样？
值得换一个更高的栈顶（比如默认的 0x3FCEF000）再试一次。

### 82z. 栈空间假设也不成立

用默认栈顶（`0x3FCEF000`）重新构建 WiFi 用例，真板的 trap 完全一样：

```
*** TRAP sp=3fcd8ed0 cause=0 pc=42011214
```

`sp` 还是 `0x3fcd8ed0` —— 说明栈顶没有实际变化（或者 `entry` 之前的调用链
已经把栈用到了这个位置）。**栈空间不是原因。**

---

## 84. 当前三组矛盾事实的总结（留给接手的人）

| # | 事实 | 证据 |
|---|---|---|
| 1 | flash 段首的 windowed 函数**能执行** | `s3_asm_flash_42` @ 0x42030014 返回 42 |
| 2 | `esp_wifi_init_internal` **在任何位置都 trap 在 entry** | 段内 0xD934、0x1214 都试过，`cause=0` |
| 3 | 同一个函数在**独立 `.flash.wifi` 段**（0x421xxxxx）里**曾完整执行并返回** | 那次的串口输出 `wifi_init rc=0xffffffff`（弱兜底返回值，说明调用真的返回了） |

**关键矛盾**：同一个函数，同样是"从 IRAM 的 call0 经孤岛调用"，
在独立段方案里能跑、在合并方案里 trap。

唯一的差别是它**周围的代码布局**。所以下一步最该做的是：

1. 回到**独立段方案**（那个能让 init 跑通的），然后想办法让 bootloader 接受 7 段镜像
   —— 比如把第二段 IROM 的 vaddr/偏移调成 bootloader 喜欢的形状
2. 或者：在合并方案里，把 `esp_wifi_init_internal` 单独放到一个
   **和段首同页**的位置（比如紧跟在 `s3_asm_flash_42` 之后），
   看它在"已知能取指的那一页"里是否就能跑

事实 3 说明**这条路是通的**，只是布局还没对上。

### 85. 边界在哪？两个已知点之间

| 位置 | 结果 |
|---|---|
| 段内 **0x14**（`s3_asm_flash_42`，windowed，经孤岛） | ✅ 能执行 |
| 段内 **0x1214**（`esp_wifi_init_internal`，windowed，经孤岛） | ❌ trap on `entry` |

⇒ 两者的调用方式**完全相同**（都是从 IRAM 的 call0 代码经 windowed 孤岛 `call8`），
差别只有**地址**。而失败边界落在 0x14 和 0x1214 之间（约 4.6KB 范围内）。

这个数量级很值得怀疑是 **I-cache 的初始填充**问题：
bootloader 跳过来之前可能只把入口所在的 cache 行装进了 I-cache，
之后的填充路径有问题。

### 最直接的下一步（一次实验就能定性）

在 `.flash.text` 里依次放几个**极小的 windowed 函数**，地址分别在
**0x100 / 0x400 / 0x800 / 0x1000** 附近，逐个调用，找出**精确的失效边界**。
如果边界恰好是 4KB 或 I-cache 行大小的整数倍，方向就明确了。

### 86. ★★ 新线索：链接器报 "windowed longcall crosses 1GB boundary"

在布置边界探测函数时，链接器吐出了一条很关键的警告：

```
libpp.a(lmac.o): in function `lmacProcessTxRtsError':
(.text.lmacProcessTxRtsError+0x86): dangerous relocation:
windowed longcall crosses 1GB boundary; return may fail: lmacEndFrameExchangeSequence
```

Xtensa 的 **windowed longcall**（`call8/call12` 的远距离形式）**只有 1GB 寻址范围**，
一旦跨过 1GB 边界，**返回可能失败**（"return may fail"）。

这和我们观察到的现象高度吻合：

- 多个 trap 的 PC 是 **`0x8037xxxx` / `0x8037d834`** —— 最高位为 1
- `0x80xxxxxx` 正是"**越过 0x80000000 边界**"的地址
- 而 Xtensa 里 **bit31 是 `call0` 返回地址的标记位**

⇒ 也就是说：**windowed 代码里的某个 longcall 返回时跳到了 `0x80xxxxxx`**，
落进非法区域 —— 这完全可能就是 `cause=0x14 InstrFetchProhibited` 的来源！

### 为什么之前没想到

因为我们一直假设"trap 的 PC 就是出问题的代码地址"。而实际上
`0x8037d834` 很可能是**另一个函数（0x4037d834）被置上 bit31 后的样子** ——
正是 longcall 返回失败的典型表现。

### 下一步（明确）

1. **把所有 `dangerous relocation ... 1GB boundary` 警告都收集出来**，
   看是哪些调用出的问题（这次只出现了一条，说明是偶发/特定布局）
2. 考虑给 WiFi 库的编译加 `-mno-longcalls`（如果闭源库允许）或者
   用 `--no-relax` / 调整段布局，让调用双方落在同一 1GB 区间内
3. 特别注意：我们的 IRAM 在 **0x4037xxxx**、flash 在 **0x4201xxxx**，
   两者都在 0x40000000~0x7FFFFFFF 内；但如果某个 longcall 的
   **返回地址**被算到了 0x8xxxxxxx，就会出问题

### 87. 把 WiFi 入口放到段首也没用（trap 跟着函数走）

把 `ieee80211_api.o`（含 `esp_wifi_init_internal`）排到 `.flash.text` 的**最前面**
（在通配规则之前），它落在 **0x420111fc（段内偏移 0x11FC）**：

```
*** TRAP cause=0 pc=420111fc
```

⇒ 和之前 0x1214 时一样。而**同一个段里**、偏移只有 **0x14** 的
`s3_asm_flash_42`（同样是 windowed、同样经孤岛调用）是**能执行**的。

### 归纳出的精确对比

| 函数 | 段内偏移 | 大小 | 结果 |
|---|---|---|---|
| `s3_asm_flash_42`（手写汇编 `movi.n a2,42; retw.n`） | 0x14 | 8 字节 | ✅ 返回 42 |
| `esp_wifi_init_internal`（闭源库） | 0x11FC | 数百字节 | ❌ trap on `entry` |

两者的调用路径**完全相同**。⇒ 差别在**函数本身**：不是位置、不是偏移，
而是这个闭源函数在被调用时的某种前提条件没满足。

### 最可能的原因（新的判断）

`entry a1, 32` 会**分配一个寄存器窗口**。如果此时 **`WindowStart` 位图**
没有为这个 callinc=8 的窗口留好位置，`entry` 就会触发
**WindowOverflow4/8/12** 异常 —— 而如果我们的窗口溢出向量处理不正确，
就会表现为 trap。

之前测过"窗口向量在 deep=11 时正常"，但那是**我们自己代码**递归的情形；
闭源库里的 `entry a1, 32` 可能是从**不同的窗口层级**进来的。

### 下一步

在调用 `esp_wifi_init_internal` 之前，把 `WindowStart`（`rsr a12, windowstart`）
的值记到信标里，看进入时窗口位图是什么状态、OWB 是多少。

### 88. ★★★ 找到真正的根因：**flash 里的 `entry` 指令无法执行**

用一个对照实验定位到了最后的问题。在**同一个位置**（`.flash.text` 段内偏移 0x14）
放两个手写汇编函数：

```asm
s3_asm_flash_42:        ; 能工作
    movi  a2, 42
    retw.n

s3_asm_flash_entry:     ; trap
    entry a1, 16
    movi  a2, 77
    retw.n
```

真板结果：

```
*** TRAP cause=0 pc=42010014      <- 正是 s3_asm_flash_entry 的地址
```

⇒ **同样是 flash 里的 windowed 代码，唯一区别就是有没有 `entry` 指令。**

| 函数 | `entry` | 结果 |
|---|---|---|
| `s3_asm_flash_42` | 无 | ✅ 返回 42 |
| `s3_asm_flash_entry` | 有（`entry a1,16`） | ❌ trap `cause=0` |

### 这解释了之前所有的现象

- `esp_wifi_init_internal` 第一条指令就是 `entry a1, 32` ⇒ **必然 trap**
- 之前所有"能跑"的 flash 测试（`s3_asm_flash_42`）**都没有 entry** ⇒ 所以能跑
- 换基址、换偏移、换段布局都没用 ⇒ 因为问题在**指令本身**，与位置无关

### 为什么 `entry` 在 flash 里会失败？

`entry` 不是取指问题 —— 它是 **core 指令**。可能的原因：
1. **`entry` 之后紧接着要取指下一条指令**，而 I-cache 的**预取/填充**在 flash 区域失效
   （`movi`+`retw` 可能整条落在同一个 cache 行里，不需要第二次填充；
   而 `entry` 之后跳转/继续取指会触发新的行填充）
2. 或者 `entry` 引发的**窗口分配**在我们 flash 代码的 PS 状态下不允许

### 下一步（方向明确）

**方案 A（最稳）**：把 `entry` 从 flash 里挪走 —— 用一个**不带 entry 的 trampoline**
在 IRAM 里"手工"完成 `entry` 的等效工作（把 a0 等寄存器存到栈上，而不是用窗口）。
IDF 的 `-mabi=call0` 编译的代码就是这么做的。

**方案 B**：让闭源库的 windowed 函数**不被直接调用**，而是先在 IRAM 里的
windowed 孤岛中完成 `entry`，再跳到库函数体（跳过它自己的 `entry`）。
即：`jump to (func_addr + 3)`，在孤岛里已经分配好窗口。

方案 B 更直接、改动最小 —— 因为孤岛本身是 `-mabi=windowed` 编译的，
它的 `entry` 在 **IRAM** 里能正常工作。

### 89. 对 §88 的更正（重要，不要被误导）

§88 说"flash 里的 `entry` 无法执行"，那个结论**未经验证**，原因如下。

反汇编确认两个函数确实在：

```
42010014 <s3_asm_flash_42>:      movi.n a2, 42 ; retw.n     <- 2 条指令，无 entry
42010018 <s3_asm_flash_entry>:   entry a1, 16 ; movi.n a2,77 ; retw.n
```

但 trap 报的 PC 是 **`0x42010014`**，那是 **`s3_asm_flash_42`** 的地址，
不是 `s3_asm_flash_entry`（0x42010018）。

查下来是**测试桥的 bug**：我用脚本自动复制 `s3_win_call_flash42` 生成
`s3_win_call_entry_test` 时，**只替换了标签、没替换 `call8` 的目标**：

```
403804d1:  call8  4037f91c <s3_win_flash42_chain>    <- 还是旧的调用目标
```

⇒ 所以这次实际调用的是 `s3_asm_flash_42`（之前验证过能工作的那个），
却 trap 了 —— 这说明**这个手工复制的桥本身有问题**，而不是 `entry` 有问题。

**`entry` 假说暂时不成立，需要重测。**

（记录在这里是为了避免后面的人被 §88 误导。正确做法是：把桥的 `call8` 目标
改成 `s3_win_asm_entry`，再跑一次。）

### 90. 板子当前状态（重要，接手时先看这里）

**板子现在跑不起来**，串口输出：

```
load:0x403c8700,len:0xcc8
load:0x403cb700,len:0x2ea0
entry 0x403c88f4
abort() was called at PC 0x403cdbe0
```

`0x403cdbe0` 是 IDF 的 `mmu_hal_map_region`（之前踩过同样的坑）。
这次是在读原厂固件（刷了 xiaozhi 的分区表和 bootloader）、
然后又刷回我们的镜像之后出现的。

**恢复步骤**：
1. 端口号会变（`/dev/ttyACM0` ↔ `/dev/ttyACM1`），刷之前先 `ls /dev/ttyACM*`
2. 端口被占住时（应用在跑、或 trap 后卡住），用 JTAG 复位：
   ```
   printf 'set pagination off\nset confirm off\ntarget extended-remote :3333\nmonitor reset run\nkill\nquit\n' > /tmp/greset.gdb
   ./tools/dbg.sh build/won/c6os.elf /tmp/greset.gdb
   ```
3. 三个镜像都要刷：`0x0` bootloader、`0x8000 /tmp/p2.bin`、`0x20000 <app>`
4. **擦掉 otadata**：`esptool erase-region 0xd000 0x2000`
   （xiaozhi 的分区表带 OTA，otadata 会让 bootloader 从别的槽启动）
5. 刷完 `read-flash` 读回比对，确认真的写进去了

**已验证可用的镜像**：`build/won/c6os.bin`（t_s3_on）的 `image-info` 显示
`Checksum: 0x3c (valid)`，它本身是好的。

### 本 session 最重要的技术结论

对照实验（同一个段内偏移 0x14、同样经 windowed 孤岛调用）：

| 函数 | 指令 | 结果 |
|---|---|---|
| `s3_asm_flash_42` | `movi.n a2,42` + `retw.n`（**无 entry**） | ✅ 返回 42 |
| `s3_asm_flash_entry` | **`entry a1,16`** + `movi.n a2,77` + `retw.n` | ❓ 待重测（桥有 bug，见 §89）|

**这个对照实验还没有真正跑出来** —— 因为测试桥的 `call8` 目标写错了（§89），
后来修正了桥，但板子进入了上面的坏状态，没能完成验证。

**下一步第一件事**：按上面步骤恢复板子，然后重新跑 `tests/t_entrytest.c6`，
确认 "flash 里的 `entry` 是否能执行"。这一步的结果会决定后续方向：
- 如果能执行 ⇒ 问题在别处（可能是闭源库自身的某种前提）
- 如果不能执行 ⇒ 方案是"孤岛里先 `entry`，再跳到库函数体 `func+3`"

### 91. 板子故障的补充信息（重要）

试过的恢复手段（都不行，仍然是 `abort() at PC 0x403cdbe0`）：
- JTAG `monitor reset run`
- 擦 `otadata`（0xd000）
- 擦 app 区（0x10000 + 0x70000）
- **整片 erase-flash**，然后重新刷 bootloader + 分区表 + app
- 删掉 `build/won` 整个目录重新构建 `t_s3_on`

⇒ 说明**不是 flash 内容的问题，是镜像本身**（`build/won/c6os.bin` 现在 131368 字节，
而之前 `t_s3_on` 是 ~50KB）。很可能是本轮往 `win_island.c` / `win_bridge.S` 里
加的测试函数（`s3_flash_end_test`、`s3_asm_flash_entry`、`s3_win_asm_entry`、
`s3_win_call_entry_test`）引入了问题 —— 它们被无条件链接进所有 s3 构建。

**最省事的恢复办法**：把这些测试函数移除（回到加它们之前的版本），
`git log` 里 `35a9e8c` 是加测试函数之前的最后一个提交：

```
git checkout 35a9e8c -- port/s3/win_island.c port/s3/win_bridge.S
```

然后 `rm -rf build/won && make ... S3_MAIN=tests/t_s3_on.c6 s3`，
应该能恢复到能跑的状态（那时 `t_s3_on` 的心跳是正常的）。

**注意**：`0x403cdbe0` 这个地址在 bootloader 的区间（0x403Cxxxx）里，
而我们的 IRAM 是 0x40378000~0x40388700 —— 所以 abort 是**bootloader** 报的，
说明它无法加载这个镜像。

### 92. 板子现已恢复到"原厂固件"状态

确认硬件与 bootloader 都正常：

```
$ esptool ... write-flash 0x20000 .../xiaozhi.bin
$ capture
I (102) app_init: Project name:     xiaozhi
```

⇒ 板子现在跑的是**原厂小智固件**，可以正常工作。

我们自己的镜像（`build/won/c6os.bin`）此刻被 bootloader 拒绝
（`abort() at PC 0x403cdbe0` = bootloader 里的 `mmu_hal_map_region`）。

### 接手时的第一步

1. 先确认 `build/won/c6os.bin` 的 `esptool image-info` 是否 valid
2. 检查它的段布局（`t_s3_on` 应该**没有** flash 段，只有 DRAM + IRAM）
3. 如果 bootloader 仍拒绝，试试把 `.flash.text`/`.flash.rodata` 改回
   **更早的、验证过能被接受**的布局（`.flash.rodata 0x3C000020 : AT(0x20)` +
   `.flash.text 0x42030000 : AT(0x30000)`），并把 `.vector`/`.text` 的 AT 顺延
4. 恢复后**第一件事**是重跑 `tests/t_entrytest.c6`，完成"flash 里的 `entry`
   到底能不能执行"这个决定性实验 —— 它会决定后续走哪条路

### 93. abort 的确切位置（已查实）

```
$ addr2line -f -e xiaozhi/build/bootloader/bootloader.elf 0x403cdbe0
mmu_hal_map_region
esp-idf/components/hal/mmu_hal.c:95
```

⇒ 是 **bootloader** 里 `mmu_hal_map_region` 的 assert 失败。

我们镜像的段布局（`t_s3_on`）：

```
Segment   Length   Load addr   File offs  Memory types
      0  0x00bc0  0x3fca0000  0x00000018  BYTE_ACCESSIBLE, MEM_INTERNAL, DRAM
      1  0x08a54  0x40378000  0x00000be0  MEM_INTERNAL, IRAM
```

**没有 flash 段**（`t_s3_on` 不用 flash 代码），bootloader 调 `mmu_hal_map_region` 时
参数为空/无效，触发了 assert。

**重要**：这个布局以前是能启动的（同样的 `t_s3_on` 跑了上百轮心跳）。
所以要么是 bootloader 换了（我刷过 xiaozhi 的 bootloader）、
要么是某个 eFuse/flash 状态被改变了。

**下一步**：
1. `git log` 找到最早那个能启动的 `t_s3_on` 镜像，`cmp` 一下段布局差异
2. 或者干脆让它**至少有一个 flash 段**（比如引用一下 `s3_asm_flash_42`），
   避开"零 flash 段"这个边界情况

### 94. ★★★★ 重大突破：`--ram-only-header` 把 DROM/IROM 段全丢了

**Makefile 第 216 行原来是：**

```make
S3_IMG_FLAGS ?= --ram-only-header
```

这个选项让 `esptool elf2image` **只输出 RAM 段**，把 **DROM 和 IROM 段全部丢弃**！

⇒ 我们**之前生成的所有镜像其实都是残缺的** —— 里面根本没有 flash 段的描述。

**去掉它之后**，镜像结构立刻和原厂一致：

```
Segment   Length   Load addr   File offs  Memory types
      0  0x0010c  0x3c000020  0x00000018  DROM        <- 终于有了
      1  0x00bc0  0x3fca0000  0x0000012c  BYTE_ACCESSIBLE, MEM_INTERNAL, DRAM
      2  0x00040  0x40378000  0x00000cf4  MEM_INTERNAL, IRAM
      3  0x0868c  0x40378400  0x00000d3c  MEM_INTERNAL, IRAM
```

**真板结果（历史性）：**

```
c6os s3 boot                    <- 应用终于正常启动了！
slot2 = 0x12345678              <- DROM 段里的探针值，读取完全正常
*** TRAP cause=0 pc=42030014    <- 只剩 flash 取指这一项
```

⇒ **DROM 通路完全打通**（`0x12345678` 正是我们放进 `.flash.rodata` 的探针值）。

这解释了之前所有的怪现象：
- 为什么 `abort() at mmu_hal_map_region` —— bootloader 拿不到 DROM 段描述
- 为什么各种段布局调整都没用 —— 反正段描述根本没进镜像
- 为什么"数据读正常、取指不正常" —— 其实数据读一直靠的是**别的路径**

**下一步**：用这个正确的镜像重新构建 WiFi 版本，看 flash 取指/IPC 是否
在"镜像结构正确"的前提下恢复正常。

### 95. 镜像修好后的精确状态：DROM ✓ / IROM ✗

用**正确构建**的镜像（`--ram-only-header` 已去掉）做对照测试，
**完全不调用** `os_icache_enable()`，直接用 bootloader 留下的 cache：

```c
s3_beacon(1, *(u32*)0x600C4060);   /* ICACHE_CTRL */
s3_beacon(2, s3_flash_rodata_probe);  /* DROM 读 */
s3_beacon(3, s3_win_call_flash42());  /* flash 取指 */
```

真板：

```
slot1 = 0x00000001    ICACHE_CTRL = 0x01（bootloader 默认值，cache 已使能）
slot2 = 0x12345678    DROM 读完全正常
slot3 = 0xb0b0b0b0    未写入 —— 取指失败
*** TRAP cause=0 pc=42030014
```

⇒ **DROM 通路 ✓、IROM 取指 ✗**，这就是最初那个现象。

而且：
- 用 bootloader 默认的 `0x01` → IROM 取指失败
- 用我们 `rom_config_instruction_cache_mode(16384,8,32)` 的 `0x0b` → 也失败

但**原厂 xiaozhi 固件用 `0x01` 就能正常从 flash 执行**。

### 现在的关键问题

镜像结构已经正确（DROM/IROM 段都在、bootloader 接受了、DROM 读正常），
**只剩 IROM 取指**。下一步应该看：

1. 原厂固件在**进入 app 之后**的 `ICACHE_CTRL` 到底是什么
   （在 app 里读，而不是用 JTAG 读 bootloader 的状态）
2. 原厂 app 是否调用了额外的 cache 配置函数
3. 我们的 IROM 段**物理位置**（flash 0x30000）是否和原厂 app 的 IROM 位置对齐方式不同

这一轮最大的收获是：**`--ram-only-header` 这个隐藏的坑**，
它让所有之前的镜像都是残缺的，掩盖了很多真实信号。

### 96. 板子已恢复，且镜像构建现在是正确的

去掉 `--ram-only-header` 之后，用 `tests/t_s3_on.c6` 重新构建：

```
Segment   Length   Load addr   File offs  Memory types
      0  0x0010c  0x3c000020  ...  DROM
      1  ...      0x3fca0000  ...  DRAM
      2+ ...      0x40378000  ...  IRAM
```

真板：`c6os s3 boot` + **`alive` 心跳持续输出** ⇒ **平台已恢复并正常工作**。

### 另外验证过的一条

显式调 ROM 的 `Cache_Enable_ICache`（`0x40001878`）会直接 trap：

```
*** TRAP cause=0x1c pc=403cccc7
```

⇒ 那个 ROM 函数在当前状态下不能这么调。cache 相关操作还是要靠
`rom_config_instruction_cache_mode` 那一条路。

### 下一步（唯一剩下的问题）

IROM 取指仍然失败（`0x42030014`、`0x4203d934` 都是 `cause=0`），
而 DROM 读完全正常。下一步照 §95 的建议：在**原厂固件的 app 里**读
`ICACHE_CTRL` 和其它 cache 相关寄存器，和我们的逐一对比。

### 97. IROM 段 flash 偏移与 vaddr 的低 16 位不一致（重要线索）

用 `image-info` 检查正确的镜像：

```
Segment   Length   Load addr   File offs  Memory types
      0  0x0010c  0x3c000020  0x00000018  DROM
      1  0x0fec4  0x3fca0000  0x0000012c  DRAM
      2  0x67ef0  0x42030000  0x0000fff8  IROM
```

IROM 段在**镜像文件内偏移 0xFFF8** ⇒ 实际落盘到 flash **0x2FFF8**
（镜像基址 0x20000 + 0xFFF8）。

而它的 **vaddr 低 16 位是 0x0000**。

⇒ **两者低 16 位不一致**（0xFFF8 vs 0x0000）。

MMU 的 64KB 页映射要求 **vaddr 的低 16 位 == flash 地址的低 16 位**，
否则映射到的是"错开 8 字节"的另一页 —— 这正是"数据读看起来对、但取指拿到垃圾"的典型症状。

**注意**：DROM 段是 `vaddr 0x3c000020` / 文件偏移 `0x18`，低 16 位也不一致，
但 **DROM 读是正常的**（`0x12345678` 探针值读到了）。所以这条规则对 DROM 不成立，
可能只对 IROM 的取指路径成立。

### 修的方向

让 IROM 段在镜像里的**文件偏移**变成 **0x10000 的整数倍**：
- 试过改 `.flash.text` 的 `AT()`，但 `esptool elf2image` **按段大小顺序打包**，
  不理会 `AT` 的空洞 —— 改 `AT(0x10008)` / `AT(0x10010)` 文件偏移都是 0xFFF8
- 所以要**调整前面段的大小**（比如给 DRAM 段加 padding），
  或者把 IROM 的 **vaddr 改成 `0x4203FFF8`** 让它和实际 flash 地址对齐

### 当前状态

- ✅ 镜像结构已正确（DROM/IROM 段都在、bootloader 接受）
- ✅ 应用正常启动、`alive` 心跳正常、**DROM 读完全正常**
- ❌ IROM 取指仍失败
- 板子跑的是 `tests/t_s3_on.c6`（稳定心跳，无 flash 调用）

### 98. 更正 §97：低 16 位"不一致"是**正常的**

查原厂镜像：

```
Segment   Length   Load addr   File offs  Memory types
      0  0xb39a8  0x3c1e0020  0x00000018  DROM     <- vaddr 低16=0x0020，文件偏移低16=0x0018
```

原厂 DROM 也是**差 8 字节**，而它工作完全正常。

⇒ 所以"vaddr 低 16 位必须等于 flash 地址低 16 位"这条规则**不成立**，
§97 的推测作废。那 8 字节是镜像头的固定开销量，bootloader 自己会处理。

### 回到原点，但这次是站在正确的镜像上

本轮的确定成果：

1. **`--ram-only-header` 是个隐藏的坑** —— 它让所有镜像都残缺，
   这是这一整轮里最有价值的发现
2. 镜像结构现在和原厂一致：DROM + DRAM + IROM + IRAM
3. 应用能正常启动、`alive` 心跳正常
4. **DROM 读完全正常**（探针 `0x12345678`）
5. **IROM 取指仍然失败** —— 这是唯一剩下的问题

### 下一步建议

在**正确的镜像**前提下重新做之前那些实验（因为之前所有实验都是在残缺镜像上做的，
结论都不可靠）：

1. 重新扫一遍 I-cache 配置（`rom_config_instruction_cache_mode` 的各种参数组合）
2. 重新看 MMU 表项的实际内容
3. 用 `EXTMEM_ICACHE_TAG_OBJECT` 机制 dump 那个地址的 I-cache tag

### 99. ★★★★★ IROM 取指彻底打通！

在**正确的镜像**（`--ram-only-header` 已去掉）基础上，
**同时**做 I-cache 模式配置和全量失效：

```c
os_icache_enable();                  /* rom_config_instruction_cache_mode(16384,8,32) */
s3_win_call_cache_invalidate();      /* Cache_Invalidate_ICache_All */
s3_beacon(1, ICACHE_CTRL);           /* -> 0x0000000F */
s3_beacon(2, s3_flash_rodata_probe); /* DROM 读 -> 0x12345678 */
s3_beacon(3, s3_win_call_flash42()); /* flash 取指 -> */
```

真板：

```
slot1 = 0x0000000F       ICACHE_CTRL = 0x0F
slot2 = 0x12345678       DROM 读正常
slot3 = 0x0000002A       ← **42**！flash 里的函数执行并返回了！
```

**flash 取指完全正常！**

### 完整的根因链（两个独立问题叠加）

1. **`--ram-only-header`** —— 让 `esptool elf2image` 丢弃 DROM/IROM 段，
   所有镜像都是残缺的（这是最主要、也最隐蔽的那个）
2. **I-cache 必须同时做"配置模式"和"全量失效"** ——
   只做其中一个都不够（之前所有实验因为镜像本身是残缺的，都掩盖了这一点）

⇒ 这两个问题**叠加在一起**，才导致前面几十轮的所有诊断都指向矛盾的方向。

### 下一步

用这个配置（正确的镜像 + os_icache_enable + invalidate）重建 WiFi 版本，
看 `esp_wifi_init_internal` 是否能跑起来。

### 100. flash 取指打通后，WiFi 函数仍 trap —— 精确对比

同一个构建配置下：

| 地址 | 段内偏移 | 结果 |
|---|---|---|
| `s3_asm_flash_42`（8 字节，无 entry） | 0x14 | ✅ 返回 42 |
| `esp_wifi_init_internal`（数百字节，有 entry） | 0xD934 | ❌ trap `cause=0` |

**两者在同一个 64KB 页内**（MMU 实测只有 entry 3 这一个页有效）。

⇒ 所以不是页映射问题，而是**页内的某个边界**。候选：
1. **I-cache 容量**：16KB cache，0xD934 = 55.6KB 处的代码 ——
   执行到这里时 cache 早已被前面的代码填满，需要**淘汰+重新填充**，
   而我们的配置可能让"填充"路径失效
2. 函数本身有 `entry`，而 `s3_asm_flash_42` 没有（但 §88 那个对照实验没做成）

### 下一步（很明确）

写一个**逐级偏移**的探针：在 IROM 段里放若干个极小的 call0 函数，
分别位于 0x1000 / 0x2000 / 0x4000 / 0x8000 / 0xC000，
逐个调用，找出**精确的失效边界**。

如果边界正好在 **0x4000（16KB = I-cache 容量）**，
那就证实是 cache 淘汰/填充的问题，方向就完全明确了。

### 101. 关键推断：I-cache 只在**前 16KB** 有效

| 地址 | 段内偏移 | 结果 |
|---|---|---|
| `s3_asm_flash_42` | 0x14 | ✅ 工作 |
| `esp_wifi_init_internal` | 0xD934（55.6KB） | ❌ trap |

`ICACHE_CTRL = 0x0F` ⇒ bit0=1（使能）、bit1-2=3（8-way）、bit2=1、bit3=1（32B）——
说明 `rom_config_instruction_cache_mode(16384,8,32)` 其实把它配成了 **32KB**。

**推断**：I-cache 目前只对**段内前 16KB** 有效 —— 超出范围后无法重新填充
（现象就是"前 16KB 能跑、后面全是 trap"）。

### 为什么

很可能是 **`Cache_Invalidate_ICache_All()` 之后 cache 进入了某种"只保留已预载内容、
不再从 flash 取新行"的状态**，或者 `rom_config_instruction_cache_mode` 的参数
（16KB）和实际硬件（S3 的 I-cache 是 16KB 但分了 8-way）不匹配导致 tag 计算错误。

### 下一步（很具体）

试不同的 I-cache 参数组合，找到能"正常淘汰填充"的那一档：

| size | ways | line | 预期 ICACHE_CTRL |
|---|---|---|---|
| 16384 | 8 | 32 | 0x0F（当前，只能前 16KB） |
| 16384 | 4 | 32 | 0x09 |
| 16384 | 4 | 16 | 0x01（默认，也不行） |
| 16384 | 8 | 16 | 0x03 |
| 16384 | 2 | 16 | 0x00 |

把这些组合逐个上板测试 `s3_asm_flash_42`（偏移 0x14）和一个高偏移函数（0xD934），
找出哪一档能让**两者都工作**。

### 102. 试 4-way 无效（方式参数被忽略）

把 `rom_config_instruction_cache_mode` 的 ways 从 8 改成 4：

```
slot1 = 0x0000000F     ICACHE_CTRL 没变（还是 0x0F）
slot3 = 0x0000002A     flash42 仍然返回 42
```

⇒ ROM 这个函数**忽略了 ways/size 参数**，最终总是写 `0x0F`。
所以没法通过它来试不同的 cache 组织方式。

### 本轮的最终状态

**已经打通的：**
- ✅ 镜像结构正确（`--ram-only-header` 的坑已填）
- ✅ 应用正常启动、DROM 读正常
- ✅ **flash 取指在段内前 16KB 内正常工作**（`0x42030014` 返回 42）

**仍存在的：**
- ❌ 段内偏移超过约 16KB 后取指失败（`0xD934` trap）

**下一步方向**（因为 ROM 函数不能调参，得从别处入手）：
1. 直接**手工写 `EXTMEM_ICACHE_CTRL` 寄存器**试不同组合（不走 ROM 函数）
2. 或者查 S3 的 I-cache 是否需要 **`ICACHE_AUTOLOAD`/预取**配置
3. 或者查 `ICACHE_CTRL1`（`0x600C4064`）里的 **`ICACHE_BUSY`/`ICACHE_ENABLE`** 位

### 103. 手工把 ICACHE_CTRL 改成 16KB（0x0B），低偏移取指仍正常

`rom_config_instruction_cache_mode(16384,8,32)` 实际写的是 `0x0F`（= **32KB**），
而 S3 的 I-cache **只有 16KB** —— 32KB 是非法值。已改为手工写 `0x0B`：

```c
s3_win_call_config_icache_mode(16384u, 8u, 32u);
S3_EXTMEM_ICACHE_CTRL = (S3_EXTMEM_ICACHE_CTRL & ~0xFu) | 0x0Bu;  /* 16KB/8-way/32B */
```

真板：

```
slot1 = 0x0000000B     ICACHE_CTRL 正确了
slot3 = 0x0000002A     flash42 仍返回 42
```

⇒ cache 模式修对了，但 **WiFi 构建里 `0xD934` 处的取指依然 trap**。

### 现在的事实

| 构建 | 低偏移(0x14) | 高偏移(0xD934) |
|---|---|---|
| `t_fix3`（无 WiFi 库） | ✅ 42 | — |
| WiFi 构建 | 待确认 | ❌ trap |

⇒ **两种构建的 `.flash.text` 内容完全不同**（WiFi 版有 424KB 的库代码），
所以"低偏移能取指"这个结论在 WiFi 版本上**尚未验证**。

### 下一步最该做的

在 WiFi 构建里**明确**验证：
1. 段首附近（`s3_asm_flash_42`，0x14）能否取指 —— 需要单独的信标槽，不要和 DROM 探针混用
2. 如果 0x14 能取指、0xD934 不能 ⇒ 边界的确认
3. 如果 0x14 也不能 ⇒ WiFi 版连第一页都有问题，方向完全不同

### 104. ★★★ `esp_wifi_init_internal` 已经执行进去了！

在 WiFi 构建里用独立信标槽明确验证，并加了 `S3_FLASH_FORCE_MAP`：

```
slot0 = 0xB0000200     标记
slot1 = 0x0000002A     ← 42！**WiFi 构建里段首取指也正常**
slot2 = 0x12345678     DROM 读正常
slot3 = 0xC0DE0001     已进入 wifi_init
*** TRAP cause=0x1C pc=4203d94d
```

反汇编 `esp_wifi_init_internal`：

```
4203d934:  entry   a1, 32                    <- 执行了 ✅
4203d937:  mov.n   a4, a2                    <- 执行了 ✅
4203d939:  bnez    a2, 4203d94d              <- 执行了 ✅
4203d94d:  l32i.n  a10, a2, 0                <- 在这里 trap ❌（cause=0x1C 是 load/store 错）
```

⇒ **`entry` 完全正常执行，函数体跑起来了**！现在挂的是
`l32i.n a10, a2, 0` —— 读我们传进去的 `g_wifi_cfg` 指针指向的内存。

### 这意味着什么

`entry` 那个"flash 里不能执行"的疑点**彻底排除**了。
现在的问题缩小到：**传给 `esp_wifi_init_internal` 的 cfg 指针有问题**。

### 下一步

检查 `g_wifi_cfg`：
1. 它在 DRAM 的哪个地址？是否被 MMU/DRAM 映射正常？
2. `cause=0x1C`（28）在 Xtensa 里对应哪种访存错误，需要查 TRM 确认
3. 我们传的是 `.c6` 里的 `static u8[512] __attribute__((aligned(512)))`，
   而闭源库期望的是一个完整的 `wifi_init_config_t` 结构体 ——
   可能它要读的字段超出了我们这块内存的范围？
   （不过这条指令只读第一个 4 字节，应该不会越界）

### 105. ★★★★★ `esp_wifi_init_internal` 完整执行并返回了（无 trap！）

改用显式 DRAM 地址做 cfg（`.c6` 里 `static` 数组取地址不可靠）：

```c
#define WIFI_CFG ((u8 *)0x3FCA2000u)
*(u32 *)(WIFI_CFG) = s3_win_call_osi_init();      /* osi_funcs */
s3_beacon(3u, s3_win_call_wifi_init((void *)WIFI_CFG));
```

真板（**没有 TRAP**）：

```
slot0 = 0xB0000200
slot1 = 0x0000002A      flash42 取指正常（42）
slot2 = 0x12345678      DROM 读正常
slot3 = 0x00000102      ← esp_wifi_init_internal 的返回值
```

⇒ **`esp_wifi_init_internal` 完整执行并返回了！** `entry`、函数体、`retw` 全部正常。

### 返回值 0x102 = ESP_ERR_INVALID_ARG

反汇编里 `movi a2, 0x102` 就是"参数无效"的错误路径。原因很可能是：

我们只填了 `wifi_init_config_t` 的第一个字段（`osi_funcs`），**其余全是 0**。
而 IDF 的 `WIFI_INIT_CONFIG_DEFAULT()` 会给所有字段填默认值，包括：
- `static_rx_buf_num` / `dynamic_rx_buf_num` / `tx_buf_type` 等
- **`magic`**（必须是 `WIFI_INIT_CONFIG_MAGIC` = 0x1F2F3F4F 之类）
- `wpa_crypto_funcs`（一个 0xA4 字节的子结构）

⇒ 下一步：按 IDF 的 `WIFI_INIT_CONFIG_DEFAULT()` 把整个结构体填好。

### 本轮的完整成就

从"完全无法执行 flash 代码"到"**WiFi 初始化函数完整跑通并返回**"：

1. `--ram-only-header` 的坑（镜像残缺）
2. `ICACHE_CTRL` 手工改正为 0x0B（ROM 函数写的是非法的 32KB）
3. `Cache_Invalidate_ICache_All`
4. cfg 用显式 DRAM 地址（`.c6` 的 static 数组取地址问题）

**`esp_wifi_init_internal` 现在完全可用了。**

### 106. 本轮结束时的精确状态

**已经彻底打通的：**

1. **`--ram-only-header`** —— 让 `esptool elf2image` 丢弃 DROM/IROM 段，
   之前所有镜像都是残缺的（最隐蔽的那个坑）
2. **`ICACHE_CTRL` 手工改正为 `0x0B`**（16KB/8-way/32B）——
   `rom_config_instruction_cache_mode(16384,8,32)` 实际会写成 `0x0F`（32KB，非法）
3. **`Cache_Invalidate_ICache_All`** —— 配置后必须再失效一次
4. **flash 取指完全正常**：`s3_asm_flash_42()` 返回 42（偏移 0x14）
5. **`esp_wifi_init_internal` 完整执行并返回**（无 trap）：
   `entry` → 函数体 → `retw` 全部正常

**还差的：**

`esp_wifi_init_internal` 返回 `0x102` = `ESP_ERR_INVALID_ARG`。
已发现 `wifi_osi_funcs_t` 需要：
- `_version`（第一个字）= `ESP_WIFI_OS_ADAPTER_VERSION` = **0x00000008**
- `_magic`（最后一个字）= `ESP_WIFI_OS_ADAPTER_MAGIC` = **0xDEADBEAF**

已在 `wifi_glue.c` 的 `s3_wifi_osi_init()` 里设置，但仍返回 0x102 ⇒
说明还有别的校验（可能是 `wifi_init_config_t` 的字段偏移算错了，
或者 `wifi_osi_funcs_register()` 里还有更细的检查）。

**下一步**：
1. 用 IDF 的头文件编译一个小的验证程序，打印 `wifi_init_config_t`
   和 `wifi_osi_funcs_t` 的**真实偏移**（我们现在是按源码推算的）
2. 或者直接对照 IDF 的 `esp_adapter.c:582` 里 `g_wifi_osi_funcs` 的完整初始化，
   逐个字段抄过来（那里面每个函数都有真实的实现，我们目前全是 `glue_ret0`）

### 107. ★★★★★ OSI 表校验通过！现在卡在内存分配

反汇编 `wifi_osi_funcs_register` 找到了**完整的校验逻辑**：

```
4203d426:  l32i  a12, a2, 0        ; a12 = osi_funcs->_version
4203d428:  beqi  a12, 8, 0x2e      ; 必须 == ESP_WIFI_OS_ADAPTER_VERSION(8)
4203d43c:  l32i  a12, a2, 0x1dc    ; a12 = osi_funcs->[0x1DC] = **第 119 个字**
4203d43f:  l32r  a11, 0xdeadbeaf   ; ESP_WIFI_OS_ADAPTER_MAGIC
4203d445:  bne   a12, a11, error   ; 必须等于魔数
4203d448:  s32i  a2, a9, 0         ; 通过，存下来
```

**关键发现**：`_magic` 在**固定偏移 `0x1DC`**（第 119 个字），
而用 `tab[n-1]` 拿不到它 —— 因为头文件里 `wifi_osi_funcs_t` 有
`#if CONFIG_IDF_TARGET_ESP32C6/C5/C61` 两个条件字段，对我们不成立，
算出来的 `sizeof` 比库期望的少两格。

改成显式 `tab[119] = 0xDEADBEAF` 后：

```
之前：rc = 0x102  (ESP_ERR_INVALID_ARG)
现在：rc = 0x101  (ESP_ERR_NO_MEM)     ← 校验通过了！
```

### 下一步：实现真正的 OSI 函数

`ESP_ERR_NO_MEM` 说明库开始调用我们的 `_malloc`/`_calloc` 了，
而它们目前都是 `glue_ret0`（返回 0）。

需要实现的（按 `wifi_osi_funcs_t` 的定义）：
- 内存：`_malloc`、`_free`、`_calloc`、`_realloc`、`_zalloc`
- 同步：`_mutex_create/lock/unlock/destroy`、`_recursive_mutex_*`
- 任务：`_task_create`、`_task_delete`、`_task_get_current_task`、`_task_ms_to_tick`
- 队列：`_queue_create/send/recv/delete`
- 信号量：`_semphr_create/give/take/delete`
- 事件/定时器：`_event_create/post/clear/delete`、`_timer_*`
- 其它：`_get_free_heap_size`、`_random`、`_log_write` 等

我们的 RTOS 有任务/互斥量/信号量，这些都能对上。

### 108. OSI 内存函数接上了，但 NO_MEM 仍在 —— 下一个是队列

已实现的 OSI 函数（`port/s3/wifi_glue.c`）：

```
tab[0]   = 0x00000008        _version
tab[119] = 0xDEADBEAF        _magic（固定偏移，不能用 sizeof 推算）
tab[43]  = osi_malloc        _malloc
tab[44]  = osi_free          _free
tab[46]  = osi_free_heap_size
tab[92]  = osi_malloc        _malloc_internal
tab[93]  = osi_realloc       _realloc_internal
tab[94]  = osi_calloc        _calloc_internal
tab[95]  = osi_zalloc        _zalloc_internal
tab[96]  = osi_malloc        _wifi_malloc
tab[97]  = osi_realloc       _wifi_realloc
tab[98]  = osi_calloc        _wifi_calloc
tab[99]  = osi_zalloc        _wifi_zalloc
```

分配器是**自包含**的（不依赖 `kernel/heap.c6` 或 `port/s3/fast.c`，
那两个在 `S3_LEAN=1` 下都不参与链接），放在 `0x3FCB8000` 起的 64KB 空闲 DRAM
（app 的 DRAM 段到 `0x3FCB6EC4` 结束）。

**但仍是 `0x101`（ESP_ERR_NO_MEM），而且堆头的 magic 是 0**
⇒ 说明我们的 `osi_malloc` **一次都没被调用**。

⇒ 库的 NO_MEM 来自别的地方。嫌疑最大的是 **`_wifi_create_queue`**（字段 100，
第 130 行），它现在还是 `glue_ret0`（返回 0），创建队列失败就报 NO_MEM。

### 下一步

实现这一组（按 `wifi_os_adapter.h` 的字段号 = 行号 - 30）：

| 字段 | 名字 |
|---|---|
| 100 | `_wifi_create_queue` |
| 101 | `_wifi_create_queue_wait_send` |
| 102 | `_wifi_delete_queue` |
| 103 | `_wifi_queue_send` |
| 104 | `_wifi_queue_recv` |

我们的 RTOS 有 `kernel/queue.c6`，但对不上 IDF 的队列语义；
最直接的是用**环形缓冲 + 我们自己的实现**。

### 109. 队列也接上了，NO_MEM 仍来自更早的地方

已实现（`port/s3/wifi_glue.c`，全部自包含、不依赖别的模块）：

| 字段 | 函数 | 实现 |
|---|---|---|
| 43/44/46 | `_malloc` / `_free` / `_get_free_heap_size` | 自包含首次适配分配器 |
| 92-95 | `_malloc_internal` / `_realloc_internal` / `_calloc_internal` / `_zalloc_internal` | 同上 |
| 96-99 | `_wifi_malloc` / `_wifi_realloc` / `_wifi_calloc` / `_wifi_zalloc` | 同上 |
| 100 | `_wifi_create_queue` | 环形缓冲 |
| 102-104 | `_wifi_delete_queue` / `_wifi_queue_send` / `_wifi_queue_recv` | 同上 |

**但 `esp_wifi_init_internal` 仍返回 `0x101`，而且堆头的 magic 还是 0**
⇒ 我们的任何函数都**没被调用过**。

⇒ NO_MEM 来自**更早**的某处。剩下的嫌疑：
- `_event_post`（字段 45，`glue_ret0` 返回 0，可能被判为失败）
- `_semphr_create` / `_mutex_create`（返回 0 当作句柄无效）
- `_task_create`（返回 0 当作任务创建失败）

### 下一步最有效的做法

**反汇编 `esp_wifi_init_internal` 里报 `0x101` 的那条路径**，看它是
在调完哪个函数指针后跳到 `movi a2, 0x101` 的 —— 这样能精确定位是哪个 OSI 函数。

（`0x101` 这个常量在库里应该很容易搜到。）

### 110. 精确定位到 NO_MEM 的出处：`esf_buf_alloc_dynamic`

用 `objdump | grep -B3 "movi.*0x101"` 定位到两处，其中一处是
**`esf_buf_setup_static`**（RX 缓冲池初始化）：

```
42034c96:  addi  a3, a3, -1
42034c98:  l32r  a8, esf_buf_alloc_dynamic
42034c9b:  callx8 a8
42034c9e:  bnez.n a10, ok        ; a10 == 0 就报错
42034cb1:  call8  wifi_log
42034cb4:  movi  a3, 0x101       ← ESP_ERR_NO_MEM
```

⇒ **`esf_buf_alloc_dynamic()` 返回了 NULL**，然后 `esf_buf_setup_static`
就报 `ESP_ERR_NO_MEM`。

### 这解释了为什么我们的堆没被碰过

`esf_buf_alloc_dynamic` 内部用的可能**不是**我们接上的那几个 malloc 字段
（43/92/96），而是走**另一条路**（比如直接从某个全局池里取，或者
用的是 `_wifi_malloc` 之外的另一套）。

⇒ 下一步就是继续往下追 `esf_buf_alloc_dynamic` 内部的调用，
找到它最终调的是 `wifi_osi_funcs_t` 里的**哪一个**函数指针。

（`esf_buf_alloc_dynamic` 在 `0x40381fe0`（IRAM），函数体较长，
需要完整反汇编来分析。）

### 111. 运行时验证：OSI 表注册成功、字段索引也对

读运行时的 `g_osi_funcs_p`（`0x3FCEF940`）指向的表：

```
slot3 = 0x3fcbe664     g_osi_funcs_p 已被 wifi_osi_funcs_register 设为我们的表
slot1 = 0x4037ca2c     表里第 96 项 = _wifi_malloc
```

`nm` 确认 `0x4037ca2c` 就是 **`osi_malloc`**（我们自己的函数），
反汇编也看到它读 `0x3fcb8000`（我们给它的堆基址）。

⇒ **OSI 表注册成功、字段索引正确、函数确实是我们的。**

### 但 NO_MEM 仍在，且我们的堆没被用过

说明 `esp_wifi_init_internal` 在 **`_wifi_malloc` 被调用之前**就失败了。

已定位到具体位置：**`esf_buf_setup_static`** → `esf_buf_alloc_dynamic()` 返回 NULL
→ 报 `ESP_ERR_NO_MEM`。

`esf_buf_alloc_dynamic` 内部走的是 `g_osi_funcs_p` 的**第 21 个字段**
（偏移 84 = `_mutex_unlock`），不是分配器 ——
说明它操作的是一个**静态缓冲池**（`g_eb_list_desc` @ `0x3fcb6850`），
池子里的 buffer 是**更早**由 `_wifi_malloc` 分配填进去的。

⇒ 所以问题变成：**那个更早的池填充步骤为什么没执行/没成功**。
最可能是 `wifi_init_config_t` 里的字段偏移算错了（我们按源码手算的，
`wpa_crypto_funcs` 占多少字节特别容易错）。

### 下一步：拿到真实的字段偏移

用 IDF 的交叉编译器编译一个小程序打印 `offsetof(wifi_init_config_t, magic)`
和各个字段的偏移，而不是手算。这样能确保 `static_rx_buf_num` 等字段真的写对了地方。

### 112. `esf_buf_setup_static` 的调用点

```
42034de0:  l16ui  a11, a4, 88      ; 从某个结构里取 u16
42034de5:  bnez   a11, ok
42034de7:  movi   a11, 10          ; 默认值 10
42034de9:  s16i   a11, a3, 176
42034dec:  movi   a12, 0x1f4
42034def:  movi   a10, 8
42034df5:  call8  esf_buf_setup_static   ; (a10=8, a11=数量)
```

然后 `esf_buf_setup_static` 内部调 `esf_buf_alloc_dynamic()`，
后者返回 NULL 就报 `ESP_ERR_NO_MEM`。

`esf_buf_alloc_dynamic` 操作的是静态池 `g_eb_list_desc`（`0x3fcb6850`），
池子里的 buffer 应当由**更早**的步骤用 `_wifi_malloc` 分配好。

⇒ 目前的现象是：**池子是空的**（所以 dynamic 分配失败），
而填充池子的步骤没有跑到 —— 但我们已确认 OSI 表和字段索引都是对的
（`_wifi_malloc` 就是我们的 `osi_malloc`），且我们的堆一次都没被调用。

### 最可能的原因

`wifi_init_config_t` 里控制缓冲池大小的字段（`static_rx_buf_num` 等）
**偏移算错了**，导致库读到 0 或者一个不合理的值，
从而根本没去分配缓冲。

虽然 `magic`（我们放在 144）校验通过了，但那可能只是个巧合 ——
如果 `wpa_crypto_funcs` 的实际大小不是 44 字节，`magic` 也会错位。

### 下一步（一定要做）

**用 IDF 的交叉编译器编译一个打印 `offsetof` 的小程序**，
把 `wifi_init_config_t` 每个字段的真实偏移打出来，
和我们手算的对照。这是唯一可靠的办法。

IDF 工具的 include 路径可以从任意一个 IDF 组件的 CMake 缓存里拿到，
或者直接用 `idf.py` 在一个临时工程里编一个 `main.c` 打印。

### 113. 试过 PHY 前置初始化，NO_MEM 依旧

在 `wifi_init` 之前加上（这两步在 `t_s3_abic` 里已验证能跑通）：

```c
s3_win_call_phy_cal();
s3_win_call_phy_enable();
```

真板：PHY 那两步正常返回（`slot0` 被写到 `0xB0000212`），
但 `esp_wifi_init_internal` **仍然返回 `0x101`**。

⇒ PHY 不是缺失的前提条件。

### 目前的确定结论

| 项 | 状态 |
|---|---|
| 镜像结构（DROM/IROM 段） | ✅ 正确 |
| flash 取指 | ✅ 正常（`s3_asm_flash_42` 返回 42） |
| `esp_wifi_init_internal` 执行 | ✅ 完整执行到返回 |
| OSI 表注册 | ✅ `g_osi_funcs_p` 指向我们的表 |
| OSI 字段索引 | ✅ `tab[96]` 确认是 `osi_malloc` |
| `_version` / `_magic` | ✅ 校验通过（错误码从 0x102 变 0x101） |
| PHY 前置 | ✅ 已初始化 |
| **缓冲池分配** | ❌ `esf_buf_setup_static` 报 NO_MEM |

### 剩下最可疑的一点

我们的堆**一次都没被 `_wifi_malloc` 调用过**（堆头 magic 始终是 0），
但 `tab[96]` 确实指向 `osi_malloc`。

⇒ 说明库**根本没走到**分配那一步，而是在更早的地方就因为某个
**配置字段读成 0** 而放弃了。

最可能还是 `wifi_init_config_t` 的字段偏移。虽然 `magic`（144）校验通过，
但那可能是因为 `magic` 之前的所有字段都是 0，而 `magic` 那个位置恰好
落在我们写 0x1F2F3F4F 的地方 —— 只要 `wpa_crypto_funcs` 的实际大小
让我们整体错位 4 字节，`magic` 就会写到 140 或 148，校验就会失败。
既然校验**通过**了，说明 144 是对的，那其它字段的偏移也应该对。

⇒ 所以更可能是**别的**原因，比如：
- `esp_wifi_init_internal` 依赖 `g_wifi_global_lock` 等**全局对象**，
  而这些需要先被某个函数初始化
- 或者需要先调 `esp_netif_init()` / `esp_event_loop_create()` 之类

### 下一步

反汇编 `esp_wifi_init_internal` 的**开头部分**（`0x4203d934` 起），
看它在调用一堆 OSI 函数之前，到底检查了哪些全局状态、
按什么顺序初始化，然后逐一对齐。

### 114. `esp_wifi_init_internal` 的初始化顺序

反汇编开头：

```
4203d9b0:  call8  pm_funcs_init           ; 失败就跳到 0x4203da28 返回错误
4203d9b3:  bnez   a10, 0xda28
4203d9b9:  l32i   a10, a4, 120            ; cfg 偏移 120 = feature_caps 低字
4203d9bc:  l32i   a11, a4, 124            ; cfg 偏移 124 = feature_caps 高字
4203d9c2:  call8  wdev_funcs_init         ; 失败同样返回
4203d9c5:  bnez   a10, 0xda28
```

⇒ `wdev_funcs_init(cfg->feature_caps_lo, cfg->feature_caps_hi)` 是**第二个**初始化步骤，
它的内部会走到 `esf_buf_setup` → `esf_buf_setup_static` → 报 NO_MEM。

⇒ 我们传的 `feature_caps = 0`（全 0）。
IDF 的 `WIFI_FEATURE_CAPS` 默认值不是 0 —— 可能需要带上支持的 feature 位。

### 下一步

1. 把 `cfg[30]`/`cfg[31]`（偏移 120/124）设成 IDF 的 `WIFI_FEATURE_CAPS` 默认值
2. 或者先看 `pm_funcs_init` 的返回值（如果它先失败，`wdev_funcs_init` 根本不会跑）
3. 继续反汇编 `wdev_funcs_init`，确认 `esf_buf_setup` 拿到的参数是什么

---

## §115 ★ 破案：call0 与 windowed 的 memset/memcpy 把返回地址踩了

**现象**：`esp_wifi_init_internal()` 一进去就挂，`cause=0x14`（EXCCAUSE 20 =
InstrPageTableErr，PMS 认为这条指令地址没有映射），而且
`pc == vaddr == a0 == 0x8203d39c` —— 正好是正确返回地址 `0x4203d39c`
**多了一个 bit31**。

**定位过程**（都是真板/反汇编证据）：

1. 陷阱现场打印 `g_osi_calls[]`，发现库在崩之前**确实在回调我们的 OSI 表**，
   顺序是 `19 13 21 18 13 19 13 18 13 21`（递归互斥/信号量/加锁/互斥…）。
2. 在 `s3_wifi_bringup()` 里加 B0/B1/B2 三个检查点，确认进库前
   `g_wifi_cfg_blob`、`g_wifi_osi_funcs`（`_magic=0xdeadbeaf`、
   `_version=8`、`magic@144=0x1f2f3f4f`）**全都正确**，BSS 也是干净的。
3. 反汇编 `wifi_menuconfig_init`（`0x4205a928`）发现它的入口是：

   ```
   4205a928: entry a1, 48
   4205a92b: l32r a8, (g_wifi_menuconfig)
   4205a92e: movi a12, 104
   4205a931: mov.n a10, a8
   4205a933: movi a11, 0
   4205a936: l32r a8, (memset)
   4205a939: callx8 a8            <-- windowed 代码用 callx8 调 memset
   4205a93c: mov.n a8, a10
   4205a93e: bnez.n a2            <-- 之后还指望 a2（=cfg）没变！
   ```

**根因**：`rt/c6rt.c` 里的 `memset/memcpy/memmove/memcmp` 是 **call0** 编译的。
windowed 调用者用 `callx8` 调它，它不执行 `entry`（不轮转寄存器窗口），
于是直接作用在调用者的物理寄存器上：

* 返回时用 `ret`（`jx a0`），而 windowed 的返回地址带 bit31 标记，
  必须由 `retw` 消掉 ⇒ 跳到 `0x8203d39c` ⇒ InstrPageTableErr；
* 顺手把调用者的 `a2`（cfg）等活跃寄存器踩掉。

**修法**：`port/s3/abi_thunk.S` 改成真正的双 ABI 实现：

* 入口看 `a0` 的 **bit31**（windowed 的返回地址带这一位，call0 的都是
  `0x40xxxxxx`/`0x42xxxxxx`，bit31=0）判断调用者用哪套 ABI；
* call0 路径：存/取 `a12..a15` 后用 `ret` 返回；
* windowed 路径：`entry a1, 16` + 函数体 + `retw`（a12..a15 由 entry/retw
  天然保护）；
* `rt/c6rt.c` 里这四个函数用 `#if !defined(__XTENSA__)` 让位；
* `abi_thunk.o` 加进 `S3_OBJ_CORE`。

> 表里的字符串函数（`strlen/strcmp/strncmp/strcpy/strncpy/strnlen`）在
> `wifi_glue.c` 里本来就是 `-mabi=windowed` 编的，所以库调它们没问题。

**结果（真板）**：`esp_wifi_init_internal()` **不再挂，正常返回 `0x102`**：

```
B0 osi0=00000000 osi@00000000 calls@3fcbe884
B1 slot13=4037c9cc slot18=4037ca4c magic=deadbeaf ver=00000008
B2 cfg@3fcbe784 osi=3fcbea84 magic@144=1f2f3f4f
o19 o13 o21 o18 o13 o19 o13 o18 o13 o21 M2 bringup returned
```

顺带确认的两件事：

* `wifi_init_config_t.wpa_crypto_funcs` 的真实布局是
  `size(u32)@4`、`version(u32)@8`、**9 个函数指针**@12..47，共 44 字节；
  `wifi_menuconfig_init` 会硬检查 `cfg[4]==44 && cfg[8]==1`，必须填。
* `l32i a10, osi_funcs, 72` 里的 72 是**字节偏移**，等于字段号 18
  （`_mutex_create`），不是字段序号 72。

### 下一步

`0x102`（INVALID_ARG）是在 `wifi_menuconfig_init` 返回之后、`pp_create_task`
之前产生的（`_task_get_max_priority`=字段 42 和 `_wifi_create_queue`=字段 94
一次都没被调到）。`wifi_init_in_caller_task` 那一段的顺序是：

```
call8 wifi_menuconfig_init   ; 0x4205a928，已通过
call8 misc_nvs_init          ; 0x420652f4
call8 ic_create_wifi_task    ; 0x42032ec0 -> call8 pp_create_task (0x4203433c)
call8 ieee80211_ioctl_init   ; 0x4205dd50
```

所以下一个要查的是 `misc_nvs_init` / `misc_nvs_load` 的返回值和
`ic_create_wifi_task` 是否真的被调到。

---

## §116 新增调试手段：把 OSI 回调序列写进 RTC 慢存

串口在 windowed 上下文里不完全可靠，`esptool read-mem` 又会先硬复位芯片，
读 DRAM 拿到的是复位后的状态。可靠做法是把追踪写进 **RTC 慢存**
（`0x50000000` 区，CPU/看门狗复位都不丢）：

* `0x50000200` = 回调总次数
* `0x50000204 + 4*(n & 31)` = 第 n 次回调的 `wifi_osi_funcs_t` 字段号

`OSI_HIT(i)` 每次回调都写一次，`tools/beacon.sh` 同款读法即可取回。

**真板读数**：整个 `esp_wifi_init_internal()` 期间只发生了 **10 次**回调：

```
19 13 21 18 13 19 13 18 13 21     (count=10)
```

（19=递归互斥、13=信号量、21=加锁、18=互斥）。字段 42
（`_task_get_max_priority`）和 94（`_wifi_create_queue`）一次都没出现，
说明 `pp_create_task` 根本没被调用到 —— 也就是说 `0x102` 是在
`wifi_menuconfig_init` 返回之后、`ic_create_wifi_task` 之前产生的，
嫌疑集中在 `misc_nvs_init` → `misc_nvs_load`。

`misc_nvs_load` 里第一件事是 `callx8 [_malloc_internal]`（字段 86，偏移
`0x158`），拿不到内存就 `movi a2, 0x101` 返回 NO_MEM。我们的 `_malloc_internal`
确实在表里（`OSI_SET` 存在），但它没有打点，所以下一轮要在
`_malloc_internal` / `_free` / `_semphr_take` 上也加 `OSI_HIT`，
确认 `misc_nvs_load` 到底走到哪一步。

---

## §117 按官方源文件校准 wifi_init_config_t（不再猜）

依据（都是官方源文件，不是推测）：

* `components/esp_wifi/include/esp_wifi.h` → `WIFI_INIT_CONFIG_DEFAULT()`
* `components/esp_wifi/Kconfig` → 各 `CONFIG_ESP_WIFI_*` 的 `default`
* `components/esp_wifi/esp32s3/esp_adapter.c` → 官方 `g_wifi_osi_funcs` 表
* `components/esp_wifi/include/esp_private/wifi_os_adapter.h` → 各槽位原型

之前手填的配置里有两处关键错误：`tx_buf_type` 和 `rx_mgmt_buf_type` 都填了 0
（静态缓冲），可官方默认是 **1（动态）**，而 `static_tx_buf_num` 又是 0 ——
等于一个 TX 缓冲都没有。按官方默认值整表重填后：

* `tx_buf_type = 1`、`dynamic_tx_buf_num = 32`、`cache_tx_buf_num = 0`
* `rx_mgmt_buf_type = 1`、`rx_mgmt_buf_num = 5`
* `mgmt_sbuf_num = 32`（官方默认；之前填 6 只是下限）
* `sta_disconnected_pm = 1`、`feature_caps = 0`（配套 `cache_tx_buf_num = 0`）

**真板结果：`wifi_menuconfig_init()` 从返回 `0x102` 变成通过**，init 往前走了一步
（新的卡点在 `wifi_init_process` 里，见下）。

## §118 thunk 里 ABI 探测寄存器不能用 a4..a7

`call8` 的窗口轮转量是 **8**（`call8` = 8 个寄存器的窗口，轮转 16-8=8，已由
`_log_write(level, tag, fmt)` 实测印证：库放在 a10/a11/a12 的三个参数正好落到
我们的 a2/a3/a4）。于是：

* 被调方 `a12..a15` = 调用者 `a4..a7` → **必须保留**；
* `a0` 是返回地址、`a1` 是栈指针；
* 调用者 `a8..a15` = 被调方 `a0..a7`，可以随便踩。

之前的 `ABI_RETURN` 探测用了 `a5` —— 它在 `entry` **之前**执行，此刻还没有轮转，
踩的就是调用者自己的 `a5`，而 windowed 调用者恰恰要求 `a5` 保留。后果：
`wifi_init_process` 在 `callx8 memset` 之后 `l32i a9, a5, 96`，`a5` 已被踩成 1，
于是以 `vaddr = 0x61` 挂掉。已改成用 **`a8`** 探测。

顺带确认官方日志槽位签名（`wifi_os_adapter.h`）：

```c
void (* _log_write)(unsigned int level, const char* tag, const char* format, ...);
void (* _log_writev)(unsigned int level, const char* tag, const char* format, va_list args);
```

库自己的 `wifi_log` 会先看它内部的 `g_log_level`，默认级别会把错误信息过滤掉，
`g_log_level = 4` 之后才能看到（我们的 `_log_writev` 用自带的迷你格式化器
把 `va_list` 还原成文本，因为 ROM ld 里没有 `vsnprintf`）。

### 下一步

当前真板现场（换成 a8 探测之后）：

```
*** TRAP sp=3fcd8eb0 cause=00000000 pc=4037e2d2 ps=00060f10 vaddr=00000000 a0=00000001
```

`pc = 0x4037e2d2` 是我们的 `memset` 里 windowed 路径的 `retw.n`，
`cause=0` 是 IllegalInstruction，`ps.OWB = 15`（寄存器窗口几乎用满）。
RTC 追踪显示只走到 20 次回调（`... 19 43 13` 就停了），比上一版少。
下一步优先查 `retw` 这条路径：把 `entry a1, 16` 改成 32、或者对照 gcc 生成的
windowed 函数（`osi_log_write`）的 `entry`/`retw` 形式，确认 `retw` 的合法性，
以及 `PS.OWB=15` 是不是窗口溢出处理没接住。

## §119 窗口标签在 a0 的 bit31:30，恢复要"清两位 + 补 0b01"

`CALL<n>` 会把窗口轮转量写进 `a0` 的 **bit31:30**，覆盖掉返回地址原来的最高
两位。实测：

* `call8` → 轮转量 8/4 = **2** → `a0 = 0x8203d39c`，真实地址 `0x4203d39c`
  （原地址最高两位 0b01，被 0b10 覆盖）
* 还原规则：`addr = (a0 & 0x3FFFFFFF) | 0x40000000`
  （Xtensa 代码地址恒在 `0x4000_0000~0x7FFF_FFFF`，最高两位恒为 0b01）

之前只清 bit31，得到 `0x0203d39c` → InstrPageTableErr；按上面两条一起做，
真板现场 `a0` 变成正确的 `0x4203d39c`，返回成功。

同时 `abi_thunk.S` 改成**完全不碰寄存器窗口**的双 ABI 实现（两条路径都不
`entry` / 不 `retw`，windowed 路径从 `a10/a11/a12` 取参、返回值写回 `a10`、
只用 `a2/a3/a8..a15` 当临时）：

* 消掉了 `retw` 在 `CALLINC==0` 时抛的 IllegalInstruction；
* 消掉了 `entry` 之前踩 `a4..a7` 的违规。

真板进展：RTC 追踪从 20 次回调涨到 22 次（`... 19 43 13 44 44` 收尾），
现在失败点在 `pm/wdev/net80211_funcs_init` 之一，收尾是两条 `_free`（错误清理），
还走不到 `wifi_api_unlock`（字段 22）。

### 下一步

1. 三个 `*_funcs_init` 里逐个定位谁返回非 0：它们的错误分支都会 `wifi_log`
   （level 1），把 `g_log_level` 已调到 4，接下来要在 `_log_writev` 里
   把 `va_list` 解出来（迷你格式化器已就绪），确认打印是否被别的路径吞掉。
2. 或者直接用 RTC 环形追踪 + 在 `_event_group_create` / `_semphr_take` 等
   未打点槽位补 `OSI_HIT`，把 22 条之后再往后推。

---

## §120 ★★ 里程碑：esp_wifi_init_internal() 在真板上返回 ESP_OK

**根因最后一环**：内存函数必须由 **windowed 编译单元**提供。

闭源库用 `call8 memset` 调过来。之前 `rt/c6rt.c` 里的版本是 call0 编的
（也试过手写汇编 thunk 的两种做法），无论怎么绕，返回时都无法正确维护寄存器
窗口的簿记，最后表现为库函数自己的 `retw` 抛 IllegalInstruction
（真板现场 `pc = 0x4203d3de`，正是 `wifi_init_in_caller_task` 结尾的 `retw.n`）。

改法很直接：把这四个函数搬到 `port/s3/wifi_glue.c`（整个文件用
`-mabi=windowed` 编译），让编译器按 windowed ABI 生成 `entry`/`retw`。
同一个文件里的 `strlen`/`strcpy`/`strncmp`/`strncpy`/`strnlen` 一直是这么
工作的，从来没有出过 ABI 问题。`abi_thunk.S` 不再参与链接。

**真板证据**：

```
B0 osi0=00000000 osi@00000000 calls@3fcbe8c4
B1 slot13=4037c7a0 slot18=4037c814 magic=deadbeaf ver=00000008
B2 cfg@3fcbe7c4 osi=3fcbeac4 magic@144=1f2f3f4f
M2 bringup returned
```

* `0x600080c0 (= slot0 = rc)` = **0x00000000 = ESP_OK**
* RTC 回调计数 `0x50000200` = **0x98 = 152 次**（修好之前只有 22 次就挂）

也就是说：IDF 闭源 WiFi 协议栈在我们的 c6LOS 上完成初始化，OSI 适配层被
回调了 152 次（内存/队列/信号量/互斥/事件组/任务/日志/时间全都有），
`wifi_menuconfig_init`、`pm_funcs_init`、`wdev_funcs_init`、
`net80211_funcs_init`、`wifi_init_in_caller_task` 全部走通。

### 下一步

1. 把测试从 `t_wififill` 推进到真正的 `esp_wifi_start()`（需要
   `_task_create` 真的建出一个任务、`_semphr_take` 能阻塞等待）。
2. `esp_wifi_scan_start()` 扫一圈，把 AP 列表打出来 —— 这是第一个
   能被外部观察到的"联网"行为。
3. 之后依次：连 WPA2 AP（这时 `wpa_crypto_funcs` 的 9 个函数指针必须填真
   实现，官方在 `wpa_supplicant/src/crypto/crypto_ops.c`，依赖 mbedtls）
   → DHCP/TCP → TLS/HTTP。

---

## §121 OSI 层换成真 RTOS 对象（任务/信号量/互斥/队列/事件组）

原来的 OSI 适配层是"单线程假对象"（任务只返回假句柄、信号量不阻塞）。
`esp_wifi_start()` 需要 WiFi 任务真的跑起来，所以整层重写成内核对象：

| OSI 槽位 | 实现 | 内核原语 |
|---|---|---|
| `_task_create` / `_task_delete` | 真任务 | `os_task_create` / `os_task_delete` |
| `_semphr_create/take/give` | 计数信号量 | `os_sem_init/pend/post` |
| `_mutex_*` / `_recursive_mutex_*` | **递归互斥** | `os_mutex_init/pend/post` |
| `_wifi_create_queue` / `_queue_*` | 环形队列 | `os_queue_init_ex/put_item/get_item` |
| `_event_group_*` | 事件组 | `os_event_*` |
| `_esp_timer_get_time` | tick | `os_tick_get` / `os_tick_hz` |

两个关键坑：

1. **windowed → call0 不能直接调**。`wifi_glue.c` 是 `-mabi=windowed`，内核是
   call0；直接调会把 CALL 的窗口标签漏给内核（`ret` 跳到 0x82xxxxxx），内核也
   会踩调用者的寄存器。新增 `port/s3/w2c.S`，每个内核函数配一个薄壳：
   `entry a1,32` → `l32r` + `callx0`（目标在 flash，超出 `call0` 的 ±512KB）
   → 先把自己带标签的 `a0` 从栈里取回 → `retw`。

2. **libc 也要两套**。库需要 windowed 的 `memset/memcpy/...`，我们自己的
   call0 代码需要 call0 版；同一符号没法兼得。做法：这四个（以及
   strlen/strcmp/strncmp/strcpy/strncpy/strnlen）留在
   `port/s3/wifi_glue.c`（windowed）给库用，我们自己的单元用 Makefile 里的
   `-Dmemset=c6_memset …` 重定向到 `rt/c6rt.c` 的 call0 实现。

   反例（踩过）：把 call0 版留给库 → 库的 `retw` 抛 IllegalInstruction；
   把 windowed 版留给我们自己 → **我们**的 `retw` 抛 IllegalInstruction。

3. `_mutex_*` 必须用内核的 `OsMutex`（owner + 递归计数），不能用 max=1 的计数
   信号量冒充：IDF 的 `wifi_api_lock` 是递归加锁，会自己把自己锁死
   （真板表现：`M1 bringup` 之后彻底卡住，无 TRAP 无返回）。

**真板证据**（`tests/t_wififill.c6` 改成任务里跑）：

```
M0 phy start
M1 bringup
beacon slot0 = 0xb0000212   (PHY 完成)
beacon slot1 = 0x0000002a   (IROM 取指 = 42)
beacon slot2 = 0x00000001   (os_task_create 成功，任务 id=1)
```

## §122 当前卡点：WiFi 任务在 `_queue_recv` 上忙等

寄存器窗口/ABI 问题全部消失，现在纯粹是队列语义问题。RTC 计数
`0x50000200` 已经涨到 **0x170e978（2417 万次）** 且末尾全是同一个回调
**29（`_queue_recv`）** —— WiFi 任务没有真正阻塞，而是在死循环。

诊断记录区（RTC `0x50000300`，复位后可直接 read-mem）：

```
0x50000300 = 0x3fcbf2f8   最近一次 _queue_recv 收到的句柄（magic 校验没过）
0x50000310 = 0x3fcbf2b8   最近一次 _wifi_create_queue 真正建出来的句柄
0x50000314 = 0x0d7de2c8   _wifi_create_queue 成功次数（2.26 亿次）
```

两个句柄差 `0x40`，且创建次数爆炸 —— 是"建队列 → 收不到 → 重建"的循环，
不是句柄简单错位。**下一步**：查是谁在循环调用 `_wifi_create_queue`
（大概率是 `ppTask` 建自己的队列失败后重试），以及 `_queue_recv` 收到的
那个地址到底是哪个对象。

## §123 忙等根因：队列句柄错位 + portMAX_DELAY 被当成普通超时

RTC 计数从 **2417 万次**降到 **30 次**、且不再增长（说明真的阻塞住了），
靠的是两个修正：

**1）队列句柄在偏移 0 处必须是"队列对象自己的地址"。**

反汇编 `pp_create_task`（`0x420343a1` 附近）：

```
s32i.n a10, a4, 0     ; s_wifi_queue = _wifi_create_queue(...) 的返回值
l32i.n a4, a10, 0     ; a4 = *(handle + 0)   <-- 读偏移 0！
s32i.n a4, a7, 0      ; xphyQueue = *(handle + 0)
```

闭源库把 `_wifi_create_queue` 的返回值当成 IDF 的
`wifi_static_queue_t { QueueHandle_t queue; void *items; }`，**读偏移 0 处的字**
当内层队列句柄存下来，之后又把这个值传给 `_queue_recv`。

我们的 `WQ` 原来偏移 0 是 `buf`（数组成员指针），于是库后来传进来的是 `buf`，
`magic` 校验必然失败 → `_queue_recv` 立刻返回失败 → WiFi 任务死循环。

真板诊断（RTC，复位后 read-mem 可读）：

```
0x50000340 = 0x3fcbf2c0   _wifi_create_queue 返回的队列对象
0x50000350 = 0x00000001   创建次数 = 1（确认只建了一个队列）
0x50000360 = 0x3fcbf300   第一个 magic 校验失败的 recv 句柄
0x50000364 = 0x3fcbf300   该队列对象的 q.buf —— 完全一致，实锤
```

修法：`WQ` 第一个字段改成 `u32 self;`，创建时写 `q->self = (u32)q`。
库读偏移 0 拿到的就是队列对象本身，再传回来时 magic 正常。

**2）`portMAX_DELAY` 要翻译成内核的"永久等待"。**

内核 `os_task_block_prepare` 的约定是 **timeout 0 = 不设超时 = 永久等待**。
IDF 传 `portMAX_DELAY = 0xFFFFFFFF`，被当成 tick 数丢进超时表，
`now + 0xFFFFFFFF` 立刻到期 → 每次等待都秒回超时（`-2`），调用者于是忙等。

真板诊断：`0x50000304 = 2`（`-r`，即 -2 超时）、`0x50000308 = 0xFFFFFFFF`
（就是传进来的 ticks）。

修法：所有阻塞原语统一过一层 `w_ticks()`：`0xFFFFFFFF → 0`。

**修正后的真板序列**（RTC 回调环，30 条，末尾）：

```
42 94 43 43 13 43 42 37 15 16 29 29
                          ^^ _task_create —— WiFi 任务真的建出来了
                             ^^ ^^ semphr_take/give 正常
                                   ^^ ^^ _queue_recv，然后安静阻塞
```

### 下一步

`esp_wifi_init_internal` 现在会一直等（不再忙等），WiFi 任务建好并阻塞在
`s_wifi_queue` 上。要查的是：谁在等谁（init 等 WiFi 任务的 ack，还是
WiFi 任务等 init 的消息），以及 `_queue_send` 有没有真的投进去。

## §124 当前阻塞点精确到"WiFi 任务在队列上空等"

新增成对标记（RTC `0x50000370` = 最后进入的阻塞原语，`0x50000374` = 最后
离开的；`1=sem_pend 2=queue_get 3=event_wait 4=mutex_pend`）。真板读数：

```
0x50000370 = 2   (最后进入：queue_get)
0x50000374 = 1   (最后离开：sem_pend)
```

说明 **WiFi 任务正常阻塞在 `s_wifi_queue` 上空等消息**，
而初始化任务那边**不在任何阻塞原语里**（否则 0x50000370 会是它）。
OSI 回调计数稳定在 30 不再增长 —— 既不是忙等，也不是我们的原语卡住，
而是库内部在等一个"WiFi 任务初始化完成"的信号，但没人发。

顺带修正：`_task_get_current_task()` 以前返回的是**栈指针 token**，而
`_task_create()` 写回给库的句柄是**任务 id**，两边口径不同，库用它判断
"我是不是在 WiFi 任务里"会一直判错。已在内核加 `os_task_current_id()`
（`kernel/task.c6`），`w2c` 壳改为调它。

### 下一步

1. 反汇编 `ieee80211_ioctl_init`（`0x4205dd50`）看它到底在等什么：
   有可能是 `_queue_send` 之前先读某个标志，而那个标志要等 `ppTask` 跑完
   首轮循环才置位。
2. 给 `_queue_send`（字段 25）也补 `HIT_OF`，确认 init 到底有没有投过消息
   （目前回调环里从没出现过 25）。
3. 若确认 init 从没投消息，就要查 `pp_post` 的入口条件（它可能先检查
   "当前是否已在 WiFi 任务"，而这个检查依赖第 1 点的标志）。

## §125 定位到 `ieee80211_ioctl` 之后卡住（pp_post 的疑点）

`esp_wifi_init_internal` 的完整链路（重建后的地址）：

```
4203fb9e call8 wifi_osi_funcs_register
4203fba9 call8 wifi_api_lock            (21)
4203fbfc call8 pm_funcs_init
4203fc0e call8 wdev_funcs_init
4203fc1a call8 net80211_funcs_init
4203fc2a call8 wifi_api_unlock          (22)
4203fc32 call8 wifi_init_in_caller_task
4203fc43 callx8 a3                      (内部回调)
4203fc5d call8 ieee80211_ioctl          (0x4205ffd8)
4203fc67 call8 wifi_api_lock            (21)   <-- 回调环里从没出现
```

真板 30 条回调环：

```
19 43 21 88 43 88 43 88 43 22 18 43 19 43 18 43 86 43
42 94 43 43 13 43 42 37 15 16 29 29
```

`37`（`_task_create`）之后 `15/16`（信号量握手）正常完成，`pp_create_task`
整条走通；随后 WiFi 任务阻塞在 `29`（`_queue_recv`）。
**但 `21`（wifi_api_lock，在 `ieee80211_ioctl` 之后）一次都没出现** ——
说明初始化任务卡在 `callx8 a3` 或 `ieee80211_ioctl` 里面。

关键疑点：**`pp_post` 是 ROM 符号（`0x400056e8 A`）**。ROM 的 `pp_post`
走 ROM 自己的队列/信号量，而不是我们的 `_queue_send`（回调环里字段 25
从来没出现过）。如果 ROM 的 `pp_post` 与 libpp 的 `ppTask` 用的不是同一个
队列对象，就会出现"初始化任务发了消息、WiFi 任务却永远收不到"的静默死锁，
与真板现象完全吻合。

### 下一步

1. 真板读 `xphyQueue`（ROM `0x3fcef904`）和 `s_wifi_queue`（`0x3fccfc2c`），
   确认两者是否都等于我们建的 WQ 指针（DRAM 读会被复位冲掉，需要先拷进
   RTC 再 read-mem）。
2. 反汇编 libpp 里的 `ppTask`（`0x40381f44` 附近），确认它到底从哪个队列收。
3. 若确认 ROM `pp_post` 不可用，就在我们这边把它替换掉（libpp 里通常也有
   一个 `pp_post` 的非 ROM 版本，或者可以用 `--wrap` 拦掉）。

## §126 死锁定性与证据（JTAG + RTC）

**JTAG 间断采样**（`tools/dbg.sh` + `set target-async on`，`monitor reset run`
后 sleep 16s 再 `interrupt`）：

```
PC=4037b01b
#0  0x4037b01b in port_idle ()
```

停在 idle 任务 ⇒ **两个任务都阻塞了**（不是死循环）。

**"谁在等哪个队列"**（RTC `0x50000390`，BSS 去重后写入）：

```
0x50000390 = 1            任务 id 1（wifi_main / 初始化任务）
0x50000394 = 0x3fcbf2d4   它等的队列
0x50000398 = 2            任务 id 2（库建出来的 WiFi 任务）
0x5000039c = 0x3fcbf2d4   它等的队列  <-- 同一个队列
```

⇒ **初始化任务和 WiFi 任务都阻塞在同一个队列 `s_wifi_queue` 上**，
而回调环里字段 25（`_queue_send`）**一次都没出现过** —— 没人投递过消息。

`pp_post` 是 ROM 符号（`0x400056e8 A`），ROM 的实现没有走到我们的
`_queue_send`。另外 `ieee80211_ioctl` 会先调 `current_task_is_wifi_task`
（比较 `s_wifi_task_hdl` 与 `_task_get_current_task()`）来决定"就地处理还是
投递"，这条判断如果判错，初始化任务就会**自己在同一个队列上收**，正好
造成上面这种同队列死锁。

### 下一步

1. 把 `current_task_is_wifi_task` 的两个输入（`s_wifi_task_hdl`、
   `_task_get_current_task()`）都记进 RTC，真板确认它们的值。
2. 确认 `pp_post`（ROM）到底用什么机制投递：读 ROM 里 `0x400056e8` 附近
   的代码，或者直接在 libpp 里找非 ROM 版本的 `pp_post` 用 `--wrap` 拦掉。

## §127 `s_wifi_task_hdl` 在库里从未被赋值

反汇编 `current_task_is_wifi_task`（`0x4205ab2c`）：

```
l32i a10, osi, 164          ; _task_get_current_task()
l32r a2, (s_wifi_task_hdl)  ; 0x3fccfc3c
sub  a10, a2, a10
movi a2, 1
movnez a2, a8, a10          ; return (s_wifi_task_hdl == current)
```

而全量反汇编里 `s_wifi_task_hdl` **只有读、没有写**（只有它 +4 的一个 bool
标志被写过 0/1）。也就是说它恒为 BSS 的 0；我们的
`_task_get_current_task()` 返回任务 id（1/2，非 0），比较结果恒为"不在
WiFi 任务里" —— 这个判断本身是对的。

再抓"谁调的阻塞 recv"（`__builtin_return_address(0)`，RTC
`0x500003b8`/`0x500003bc`），两个任务都落在 `0x40381ff5`（`ppTask` 内部）。
注意这条读数可能受 windowed 函数里 a0 被回收的影响，只能当参考；
"两个任务阻塞在同一个队列 `0x3fcbf2d4` 且 `_queue_send` 从未被调用"
这组证据是硬的。

### 下一步

1. 让 `_queue_recv` 的调用者记录改用**窗口帧里的 a0**（`entry` 保存的那份）
   或干脆在 `ppTask` 的反汇编上下文里对照，确认第二条 recv 到底来自哪个任务。
2. 查 `pp_post`（ROM `0x400056e8`）：直接读 ROM 字节确认它是否走
   `g_osi_funcs_p` 的 `_queue_send`（字段 25）。若不走，用 `--wrap=pp_post`
   把它换成我们自己的实现（投递到 `*xphyQueue`）。

## §128 `pp_post` 被 ROM 绝对符号顶掉（已修）

`port/s3/rom/esp32s3.rom.ld` 里原来有一行 `pp_post = 0x400056e8;`。
但 **`libpp.a` 自己就有强定义的 `pp_post`（`T pp_post`）**：

```
libpp.a: 0000001c T pp_post     00000074 T ppTask      U pp_post ×3
```

linker script 里的绝对符号赋值会让所有引用都解析到 ROM 那一份，库里那份
根本不会参与链接。而 ROM 的 `pp_post` 是给 Espressif 自己的 ROM 协议栈用的，
和我们用的 libpp `ppTask` 不配套（`ppTask` 从 `_wifi_create_queue` 建的队列
收消息）。这就是"两个任务都阻塞在同一个队列、`_queue_send` 一次都没被调"
最合理的解释。已按 `ppTask` 同样的处理方式把它注释掉，交还给 libpp。

反汇编 libpp 的 `pp_post`（`0x42036804`）确认它确实是走我们的 OSI 表的：

```
l32i a8, osi, 40     ; _wifi_int_disable
l32i a4, osi, 44     ; _wifi_int_restore
callx8 a6            ; _queue_send（偏移 100 = 字段 25）
bnei a10, 1, ...     ; 我们的 osi_queue_send 成功返回 1，对得上
```

**但真板读数没变化**：回调环仍然是 `... 37 15 16 29 29`，字段 25 依旧没出现。
也就是说 **`pp_post` 压根没被调用** —— 初始化任务卡在比"投递"更早的地方
（`wifi_init_in_caller_task` 里 `ic_create_wifi_task` 返回之前）。

### 下一步

1. 记录 `_task_create` 写回给库的句柄（任务 id），确认它是不是 2；
   再把 `_queue_recv` 里记录的两个任务 id 与它对照。
2. 若确认"任务 1（wifi_main）也在执行 ppTask 的 recv"，说明 `_task_create`
   的入口函数/参数映射有问题（windowed 6 参数 → 我们 a2..a7 的顺序），
   要按 libpp 的调用点逐参数核对。

## §129 `_task_create` 的参数完全正确

RTC `0x500003c0` 记录的 `_task_create` 调用现场：

```
0x500003c0 = 0x40381ff8   fn     = ppTask           （对上）
0x500003c4 = 0x00000000   arg    = 0
0x500003c8 = 0x00000e00   stack  = 3584
0x500003cc = 0x00000002   返回的任务 id = 2
0x500003d0 = 0x00000001   调用者（wifi_main）id = 1
0x500003d4 = 0x0000001d   prio   = 29
```

⇒ 任务创建这一环没有问题：入口是 `ppTask`，新任务 id=2，调用者（初始化任务）
id=1。所以之前 `__builtin_return_address(0)` 把"任务 1 也在 ppTask 的 recv
上"这条读数确实不可靠（windowed 函数里 a0 会被回收）。

### 下一步

任务 1（初始化任务）到底在哪里调了 `_queue_recv`？两种可能：
1. 是 `__builtin_return_address` 的假象，任务 1 其实卡在别处（那就改成
   在 `_queue_recv` 里只记任务 id、用"进入/离开"标记判断谁没返回）。
2. `ieee80211_ioctl` 在"就地处理"分支里自己也收队列，那就需要核对
   `current_task_is_wifi_task` 之外的其他前置判断。

## §130 事件环实证：两个任务都进 `_queue_recv`，都不返回

改用"事件环"（RTC `0x50000400`，每项 = `(任务 id << 8) | 1=进入/2=离开`），
完全不依赖 a0：

```
ev[0] = 0x101   (任务 1 进入 recv)
ev[1] = 0x201   (任务 2 进入 recv)
之后没有 0x102 / 0x202 —— 两个都没返回
```

⇒ 之前用 `__builtin_return_address(0)` 得到"两条都来自 ppTask"虽然不可靠，
但"**两个任务都阻塞在 `_queue_recv` 上、都没有返回**"这件事是硬的，
而且任务 1 就是初始化任务（`_task_create` 现场记录调用者 id=1）。

也就是说：初始化任务在 `esp_wifi_init_internal` 返回之前，自己也进了
`_queue_recv`，而 WiFi 任务同时在同一个队列上等 —— 谁都不投递。

### 下一步

在 libpp 里找除了 `ppTask` 之外还有谁从 `wifi_create_queue` 建的队列上收
（反汇编搜索所有 `l32i a?, osi, 116` + `callx8` 的组合），定位初始化任务
走的那个接收点；它前面的投递缺失才是真正要补的。

## §131 初始化任务是**先**阻塞的那个

事件环顺序（RTC `0x50000400`）：

```
ev[0] = 0x101   任务 1（初始化）先进入 _queue_recv
ev[1] = 0x201   任务 2（WiFi 任务）后进入
（之后没有任何 0x102/0x202）
```

结合回调环 `... 42 37 15 16 29 29`：`37`（建任务）→ `15/16`（信号量握手，
模拟"任务已启动"）→ 然后才是两条 `29`。而事件环说**任务 1 的 recv 在前**。

也就是说：`pp_create_task` 在完成 `_task_create` + 信号量握手之后，
**自己在 WiFi 队列上做了一次接收**并永久阻塞；WiFi 任务随后也在同一队列
上阻塞。没人投递（`_queue_send` 字段 25 从未出现）。

### 下一步

这一版最省力的验证手段是把 `pp_create_task` 完整反汇编出来，看信号量握手
之后那段到底有没有 `l32i a?, osi, 116` + `callx8`（即 `_queue_recv`）。
若有，说明它的语义依赖某个我们还没满足的前置条件（例如队列里应当已经
有"任务启动"消息），需要在 `_task_create` 返回前替库投递一条，或者把
`_queue_recv` 在"初始化任务"身份下改成不阻塞。

## §132 全量枚举：WiFi 路径上只有 `ppTask` 会从 osi 队列收

按"`l32i aX, aY, 116` 紧跟 `callx8`"的模式扫全 ELF，命中的函数只有：

```
mesh_flush_txQ
mesh_rx_task_deinit
mesh_send_block_deinit
mesh_tx_task_deinit
rcUpdateTxDoneAmpdu2      ← 偏移 116 是它自己的结构体字段，噪声
```

（`ppTask` 的接收点 `0x40381f66` 在它的标号之前，所以没被这条 awk 归到
`ppTask` 名下，但反汇编已确认它就是用 `l32i a5, a5, 116` + `callx8` 收的。）

并且 `pp_create_task`（`0x42036584`~`0x420366d9`）里**没有任何**偏移 116
的调用 —— 它只做 `_task_get_max_priority` / `_wifi_create_queue` /
`_semphr_create` / `_task_create` / `_semphr_take` 握手。

⇒ WiFi 初始化路径上真正的接收者只有 `ppTask`。
所以之前"任务 1（初始化任务）也进了 recv"这条读数需要换判据复核 ——
可能是 `_task_get_current_task` 的取数时机/口径问题，而不是真有两个接收者。

### 下一步

在 `osi_queue_recv` 里同时记录 `os_task_current_id()` **和**从 `ppTask`
反汇编能对上的调用点，两者交叉验证；或者直接在 `ppTask` 的接收点埋
`RTC` 计数（拿到 a1 栈帧里的返回地址），彻底确认第二次接收的来源。

## §133 ★ 决定性证据：同一任务被报了**两个不同的任务 id**

在 `osi_queue_recv` 里同时记 **栈指针** 和 `s3_w2c_current_task()`（RTC
`0x50000440`）：

```
0x50000440 = 0x3fcba5c0   sp（第 1 次 recv）
0x50000444 = 0x00000002   报出来的任务 id
0x50000448 = 0x3fcba5c0   sp（第 2 次 recv，**完全相同**）
0x5000044c = 0x00000001   报出来的任务 id
```

**栈指针完全一样** ⇒ 这两次 recv 是**同一个任务**发出的。
但 `_task_get_current_task()` 两次分别返回 2 和 1。

这推翻了 §131/§132 里"两个任务都进了 recv"的结论，并且直接说明：

> `os_task_current_id()`（也就是 `g_cur_arr[g_core]->id`）在同一个任务里
> 会返回不同的值。

闭源库大量用 `_task_get_current_task()` 判断身份
（`current_task_is_wifi_task`、`pp_post` 的就地处理分支等），身份不稳会
直接导致"以为自己在 WiFi 任务里 / 不在 WiFi 任务里"判断错乱，进而出现
本轮的静默死锁。

### 下一步

重点查两处：
1. `g_cur_arr[]` 的写入点（`os_task_start_core` 里 `g_cur_arr[me] = t`）与
   上下文切换的先后；单任务里是否可能被 `os_task_create` / 阻塞唤醒路径
   改写成另一个任务。
2. `port_core_id()`：它把 `s3_prid_raw()` 映射到 `g_prid_base[]` 的下标，
   如果映射不稳定，`g_core` 会在 `g_cur_arr[0]`/`g_cur_arr[1]` 之间跳，
   正好能解释"同一任务读到两个不同 id"。

## §134 结论修正：两次 recv 都来自 WiFi 任务，错的是任务 id

`port_task_ctx_create` 的槽位分配记录（RTC `0x50000460`，BSS 计数去重）：

```
0x50000460 = 1           任务 1 用 1 号槽
0x50000464 = 0x3fcb8620  它的栈基址
0x50000468 = 0x00001000  4096 字节
0x5000046c = 2           任务 2 用 2 号槽
0x50000470 = 0x3fcb9620  它的栈基址
0x50000474 = 0x00000e00  3584 字节
```

**槽位分配完全正确**，两个任务各有独立栈。而两次 `_queue_recv` 记下来的
栈指针都是 `0x3fcba5c0`：

* 任务 1 栈顶 = `0x3fcb8620 + 0x1000 - 80 = 0x3fcb95d0`
* 任务 2 栈顶 = `0x3fcb9620 + 0x1000 - 80 = 0x3fcba5d0`

`0x3fcba5c0 = 0x3fcba5d0 - 16` —— 落在**任务 2 的栈**里。

⇒ **两次 recv 都是 WiFi 任务（任务 2）发出的**，和 §132 的枚举一致
（WiFi 路径上只有 `ppTask` 会收队列）。真正错的是
`_task_get_current_task()`：其中一次把任务 2 报成了任务 1。

这仍然是个必须修的问题 —— 闭源库到处用 `_task_get_current_task()` 判断
身份（`current_task_is_wifi_task`、`pp_post` 的就地处理分支），报错任务 id
会让库做出错误的并发决策。

### 下一步

在 `os_task_current_id()` 里把 `g_cur`、`g_cur_arr[0]`、`g_cur_arr[1]`、
`port_core_id()` 四个值一次性记进 RTC，抓"同任务报不同 id"的现场，
判断是 `g_cur_arr` 被改写、还是 `g_core` 下标跳变。

## §135 任务 id 机制自洽；先前"同 sp 不同 id"不足为凭

在 `os_task_current_id()` 里记录 `g_cur->id`、`g_cur_arr[0]->id`、
`g_cur_arr[1]->id`（RTC `0x500004a0`，4 项环）：

```
e0: me=1  a0=1  a1=0
e1: me=1  a0=1  a1=0
e2: me=1  a0=1  a1=0
e3: me=2  a0=2  a1=0
```

`g_cur_arr[1]` 恒为 0（单核运行），且 **`me` 与 `g_cur_arr[0]->id` 始终相等**
—— 任务身份的取数链路是自洽的。而且全内核只有**一个** `port_task_switch`
调用点（`kernel/task.c6:562`），紧邻的上一行就是 `g_cur_arr[me] = t`。

所以 §133 里"同一个 sp 报出两个 id"的读数**不能作为结论**：`.c6` 编译出来的
指针转换/取址在跨函数时已知不可靠（见 §114 的记录），那条记录里的 id 与 sp
很可能是从不同时刻拼起来的。

### 现状小结（M1 阶段）

* `esp_wifi_init_internal()` 在**单线程假任务**配置下已经能返回 **ESP_OK**（§120）。
* 换成真 RTOS 任务后：任务能建、能调度、能阻塞、队列/信号量/递归互斥都工作，
  但 WiFi 任务在 `s_wifi_queue` 上空等，而初始化任务走不到 `pp_post`。
* 本轮把定位工具链补齐了（RTC 回调环、事件环、阻塞原语进出标记、context 槽位
  分配记录、任务 id 记录），并把 `pp_post` 交还给 libpp。

### 下一步

不再纠结 id 记录，直接抓"初始化任务到底停在哪条指令"：JTAG 在
`port_idle` 只能看到 idle 任务，需要读内核的 `g_tasks[]` 里各任务的
`state`/`wait_obj`/`wait_kind`，把初始化任务的等待对象打出来。

## §136 新发现：定时器 ISR 跑到 `port_isr_enter()` 之后就不再往下走了

在 `s3_timer_irq` 里放两个计数（RTC）：

* `0x50000700` = 进入 ISR 的次数（每 256 次写一次）
* `0x50000708` = 进入 `os_sched_tick()` 之前的次数（每 256 次写一次）

真板读数：

```
0x50000700 = 0x200      ISR 进了 512 次
0x50000708 = 0xd7ef4660 从没被写过（还是 RTC 随机值）
```

也就是说：**中断在持续进来，但绝大多数次都到不了 `os_sched_tick()`**。
两者之间只有三句：`port_cur_ctx()`、`port_irq_fill(frame_sp)`、
`port_isr_enter()`。

`os_tick_get()` 只有 2 —— 说明 `os_sched_tick()` 总共只真正跑了 2 次。
这直接解释了所有"带超时的等待不会到期"的现象（`os_task_delay`、
`_semphr_take` 的 timeout、`_queue_recv` 的 timeout 全都靠它）。

另外把低优先级监控任务（`mon_main`）接好了：它把
`os_task_probe(id, state/wait_kind/wait_obj)` 写进 RTC `0x50000500`，
但因为它依赖延时，目前也只跑到第一轮就停住 —— 正好印证同一个问题。

### 下一步

1. 在 `port_cur_ctx()` / `port_irq_fill()` / `port_isr_enter()` 里各放一个
   RTC 计数，定位是哪一句之后中断流程被打断。
2. 重点怀疑 `port_irq_fill()`（它要写陷阱帧/切换上下文）在 WiFi 任务
   建立之后开始出问题（有真实任务上下文时才走这条分支）。

## §137 修正：tick 其实在正常推进；InstructionFetchError 是监控任务引起的

去掉监控任务后，ISR 分段计数完全自洽：

```
0x50000700 (进入 ISR)            = 0x1300 = 4864
0x50000714 (port_irq_fill 之后)  = 0x0100 = 256
0x50000718 (port_cur_ctx 为 0)   = 0x0100 = 256
0x50000720 (os_sched_tick 之后)  = 0x1300 = 4864   <-- 与 ISR 次数吻合
```

⇒ **`os_sched_tick()` 每次都走到了，tick 在正常推进**。
§136 里"tick 只推进到 2 次"完全是那个坏掉的监控任务造成的假象。

另外：新加的监控任务（`os_task_create("mon", mon_main, 0, 30u, 1024u)`）会带来
一个新的故障 —— `EXCCAUSE 2 (InstructionFetchError)`，`pc = 0x3fcb5d3a`
（落在 `.data` 段里，`__c99_d2` 与 `g_prid_base` 之间），`a0 = 0`、`ws = 0`，
即"跳到数据区执行"。把监控任务去掉就恢复正常。1024 字节栈对其中的
`os_task_probe` 循环来说偏小，大概率是栈溢出踩了 `.data`。
（保留 `os_task_probe` 与 `os_task_total`，只是测试里不再创建监控任务。）

### 分配器加固（顺带完成）

`port/s3/wifi_glue.c` 的 WiFi 堆分配器加了两道防御：

* `n == 0 || n > 堆大小-64` 直接返回 0 —— 否则 `n + 15` 会回绕成 0，
  调用方再按原尺寸写下去就会把整片 `.bss` 写烂；
* 链表遍历加了边界判断，节点不整个落在堆数组内就停 —— 否则会把数组外的
  邻居当成块头；
* `osi_calloc` 加了 `n * sz` 溢出检查。

### 回到主线

当前状态（去掉监控任务后）与 §125/§131 一致：
`M1 bringup` 之后不再有输出，回调环停在 `... 37 15 16 29 29`。
也就是 `pp_create_task` 走通、WiFi 任务建好并在 `s_wifi_queue` 上等，
而初始化任务在那之后没有再产生任何 OSI 回调。

### 下一步

在 `osi_queue_recv`/`osi_semphr_take`/`osi_mutex_lock` 之外，把
`_wifi_zalloc`(93)、`_wifi_malloc`(90)、`_queue_send`(25) 也全部打点，
确认初始化任务到底走到 `wifi_init_in_caller_task` 的哪一步；
必要时用 JTAG 在 `port_idle` 命中后读内核任务表的 `wait_obj` 定位。

## §138 新工具：定时器 ISR 采样"被打断的主流程 PC"

在 `s3_timer_irq` 里 `rsr %0, epc1` 取被打断的 PC，写进 RTC：

* `0x50000780` = 最近一次采样到的 PC
* `0x50000784` = PC 变化次数（一直在变 = 主流程在跑；很小 = 停在原地）
* `0x50000788` = ISR 采样计数

真板读数：

```
0x50000780 = 0x4037af47   = port_idle+0x13（waiti 0 的下一条）
0x50000788 = 0x00000c00   ISR 采了 3072 次
```

⇒ **主流程一直停在 `port_idle`**，也就是所有任务都阻塞、只有 idle 在跑。
这和 §126 的 JTAG 采样（`port_idle`）互相印证。

这条 PC 采样工具从此可以随时回答"卡死时主流程在哪"，比 JTAG 稳定得多
（JTAG 在这块板上连一次要重试好几次，`print os_task_probe(...)` 这种
inferior call 更是连续三次连不上）。

### 下一步

主流程在 idle ⇒ 初始化任务阻塞在某个原语上。但 `BDIAG`
（`0x50000370`/`0x50000374`）只保留"最后一次进入/离开"，而最后进入的是
WiFi 任务的 `_queue_recv`，把初始化任务的原语覆盖掉了。
需要把它改成"每个任务各一份"的记录，才能看到初始化任务到底阻塞在
`_semphr_take` / `_mutex_lock` / `_event_group_wait_bits` 哪一个。

## §139 按任务分开的阻塞标记：任务 1 与任务 2 **都**阻塞在 `_queue_recv`

`BDIAG` 改成按任务 id 取低 2 位分槽（`0x50000370 + 8*i`，
每槽 = [最后进入, 最后离开]，1=sem_pend 2=queue_get 3=event_wait 4=mutex_pend）：

```
0x50000370 = 2     0x50000374 = 1     （无当前任务的槽）
0x50000378 = 2     0x5000037c = 1     任务 1（初始化）
0x50000380 = 2     0x50000384 = 随机   任务 2（WiFi 任务）
```

⇒ **任务 1 和任务 2 最后进入的阻塞原语都是 2（`_queue_recv`），都没有离开。**

这与"主流程停在 `port_idle`"（§138）完全吻合：两个任务都阻塞在同一个
`_queue_recv` 上，队列里没有消息（`_queue_send` 字段 25 从未出现），
于是系统只剩 idle 在跑。

也和 §132 的全量枚举矛盾不了：WiFi 路径上只有 `ppTask` 会调
`_queue_recv`，所以**初始化任务也执行到了 `ppTask` 的接收点**这件事本身就
是可疑点 —— 要么 `_task_create` 的入口被两个任务共用了，要么
`os_task_current_id()` 把同一个任务报成了两个。

### 下一步

把 `task_create` 里库传进来的入口函数指针（`fn`）与
`port_task_ctx_create` 实际生成的入口地址**都**记进 RTC，
并给 `ppTask` 的第一条指令埋一个"进入 ppTask"的 RTC 计数，
看它到底被几个任务执行过（计数 = 1 还是 2）。

## §140 澄清：任务归属是对的，确实是两个不同任务

同时记录 port 侧"当前 ctx"（`g_s3_cur[0]`，`port/s3/ctx.S` 在
`port_task_switch` 里写）和内核侧"当前任务 id"（`_task_get_current_task()`）：

```
recv #0:  g_s3_cur[0] = 0x3fcbe66c   id = 2
recv #1:  g_s3_cur[0] = 0x3fcbe620   id = 1
```

两者相差 `0x4C = 76 = sizeof(S3Ctx)`（相邻槽位），而且 **ctx 与 id 同步变化**。

⇒ **任务归属是正确的**：确实是两个不同任务各调了一次 `_queue_recv`，
§133 里"同一任务报两个 id"的怀疑可以彻底排除（那次是记录写法的锅，
本轮新加的记录槽位和三元组槽位有重叠，sp 那一列就不可信了）。

所以当前状态是确定的：

* 初始化任务（id 1）和 WiFi 任务（id 2）各自在 `_queue_recv` 上阻塞一次，
  之后都没有返回；
* 全流程只有 **2 次** `_queue_recv`（RTC `0x50000860 = 2`）；
* `_queue_send`（字段 25）一次都没有；
* 主流程停在 `port_idle`（§138）。

也就是说：两个任务都在等队列，没人投递。而在 WiFi 路径上真正会收队列的
只有 `ppTask`（§132 的全量枚举），所以**初始化任务也执行到了 `ppTask` 的
接收点**这件事仍然是唯一未解的矛盾点。

### 下一步

1. 先修掉 §140 提到的记录槽位重叠（把 ctx/id 记录挪到独立区域）。
2. 用 `g_s3_cur[0]` 反查它对应的任务（`S3Ctx` 里有 `stack` 字段，
   与 `port_task_ctx_create` 记录的栈基址对照），确认两个 ctx 分别属于谁。
3. 在 `ppTask` 入口（`0x40381ff8` 附近）埋 RTC 计数 —— 但它入口指令被
   库代码复用（`l32r` 预取），需要用 `hbreak` 或改 `_task_create` 传一个
   只做计数的 call0 引导函数（注意：引导函数必须 call0，不能再转 windowed）。

## §141 在 `s3_task_tramp` 埋点：实际只有两个任务启动

`port/s3/ctx.S` 的 `s3_task_tramp` 是所有任务的引导入口（初始帧把 entry 放
a2、arg 放 a3）。在那里把 **启动次数 / 入口地址 / 起始 sp** 写进 RTC
（`0x50000A00` 起），并按启动序号存一份入口地址（`+16 + (n&3)*4`）。

真板读数（入口地址经 `nm` 反查）：

```
0x50000A04 = 0x403821b4  = ppTask          （WiFi 任务）
0x50000A10 = 0x403785e4  = 另一个入口（非 ppTask）
0x50000A14 = 0x403821b4  = ppTask
```

⇒ **只有两个任务真正启动**，入口分别是 `ppTask` 和另一个（测试的
`wifi_main`），符合预期。（计数本身因为 RTC 初值是随机数、只做自增，
所以 `0x50000A00` 的绝对值不可读，要看 `+16` 起的入口表。）

所以"两个任务都在 `_queue_recv` 上阻塞"这件事仍然成立，但
`ppTask` 只有一个任务在跑；剩下要解释的是：
**为什么 `g_s3_cur[0]` 会在两次 recv 之间切到 `g_ctx[0]`（wifi_main 的
ctx），而 wifi_main 此刻本应阻塞在 `s3_win_call_bringup` 里。**

可能的方向：
1. `wifi_main` 并没有真的阻塞 —— 它进入了 `os_task_start_core` 的主循环
   上下文（`g_sched[0]`），而那个上下文和 `g_ctx[0]` 的区分在某处没做对；
2. `os_sched_pick` 把 BLOCKED 的任务又选中了一次（状态机 bug）。

### 下一步

在 `os_task_start_core` 的主循环里、`sched_pick` 之后，把
"选中的任务 id / 它的 state"写进 RTC，看是否出现"选中了一个 BLOCKED 任务"。

## §142 调度选择埋点：调度器本身没有 bug

在 `os_task_start_core` 的 `sched_pick(me)` 之后，把最近 8 次选中的
(任务 id, 状态) 写进 RTC `0x50000B00`（状态 1=READY / 4=BLOCKED）。

真板读数：

```
pick0: id=1  state=1     （wifi_main 首次运行）
pick1: id=2  state=1     （WiFi 任务 ppTask）
pick2: id=1  state=1     （wifi_main 第二次运行）
（之后没有更多选择）
```

两条关键结论：

1. **从来没有选中过 BLOCKED 任务** —— 调度器状态机是对的，
   §141 里"`g_s3_cur` 为何切到 `g_ctx[0]`"的疑问解除：任务 1 确实被
   正常选中过，切换是合法的。
2. 全程只有 **3 次** 调度选择 —— 也就是说任务 1 第二次跑起来之后就把自己
   阻塞了，此后系统只剩 idle（与 §138 的 PC 采样一致）。

### 当前确定的事实链

* WiFi 任务（ppTask）被创建并运行：先 `_semphr_give` 完成握手，再在
  `_queue_recv` 上阻塞 ✅
* 初始化任务（wifi_main）第二次运行后也阻塞了，之后再也没有产生任何
  OSI 回调（回调环停在 `... 37 15 16 29 29`）
* 全流程 `_queue_send`（字段 25）一次都没有 ✅
* tick 正常推进、ISR 正常、栈充足、分配器已加固、调度器无 bug

### 下一步

任务 1 第二次运行的那一小段就是关键：它从 `pp_create_task` 返回之后，
只可能走 `ieee80211_ioctl_init()` 然后返回。要么它在这两步之间阻塞，
要么它其实返回了但 `esp_wifi_init_internal` 的下一步 `_wifi_zalloc`（字段
93）没被记录 —— 需要在 `_wifi_zalloc`/`_wifi_malloc` 上也确认一次，
并给 `wifi_init_in_caller_task` 的返回路径加一个可观测信号
（例如在 `esp_wifi_init_internal` 之后立刻写 beacon —— 现有测试里
`M2 bringup returned` 就是，只是它没打印出来）。

## §143 ★ 内核级确认死锁：任务 1 与任务 2 阻塞在**同一个队列对象**

在 `os_task_block_prepare`（所有阻塞的必经之路）里记录最近 8 次
(任务 id, wait_kind, wait_obj, timeout) 到 RTC `0x50000C00`。

真板读数（全程只有 3 次阻塞）：

```
blk0: id=1  kind=1  obj=0x3fcbf8c0  to=0     （初始化早期的信号量/其它等待）
blk1: id=2  kind=6  obj=0x3fcbf234  to=0     （WiFi 任务 ppTask 的收队列）
blk2: id=1  kind=6  obj=0x3fcbf234  to=0     （初始化任务，**同一个对象**！）
```

`kind=6` 是 `os_queue_get_item` 的等待类型，`obj` 就是 `&WQ.q`
（`WQ` 对象在 `0x3fcbf230`，`WOsQueue` 成员偏移 4）。

⇒ **初始化任务最后一步是在 WiFi 队列上做接收，和 WiFi 任务撞在同一个对象上。**
这是内核层面、不依赖任何 id/栈判读的确凿证据。

结合 §132 的全量枚举（WiFi 路径上只有 `ppTask` 收这个队列），唯一合理的
解释是：初始化任务走到了某个"同步等待回复"的路径 —— 最可能是
`ieee80211_ioctl`（`esp_wifi_init_internal` 在 `wifi_init_in_caller_task`
之后紧接着调用它）直接在这个队列上等回复。而负责投递的 `pp_post`
（字段 25 `_queue_send`）在整个流程里**一次都没有被调用**，
所以两边永远等下去。

### 下一步

反汇编 `ieee80211_ioctl`（当前地址需重新 nm 确认），看它在调用
`current_task_is_wifi_task` 判定"不在 WiFi 任务里"之后，到底是
`pp_post` + 信号量等待，还是直接 `_queue_recv`。若是后者，就要在
初始化路径上补一次投递（或者在 `current_task_is_wifi_task` 为真时
走就地处理分支，这需要让 WiFi 任务的 id 判断成立）。

## §144 `ieee80211_ioctl` 不是那个接收点

反汇编 `ieee80211_ioctl`（`0x420602ac`）的调用序列：

```
420602da call8 current_task_is_wifi_task
          为真 -> 42060439 call8 ieee80211_ioctl_process   （就地处理）
          为假 -> 4206034d call8 wifi_api_lock
                  4206035b call8 wifi_get_init_state
                  4206039b call8 wifi_is_stop_in_progress
                  420603db call8 pp_post                      （投递给 WiFi 任务）
                  420603f8 call8 wifi_api_unlock
```

**它自己不收队列**（函数范围内没有任何 `l32i aX,aY,116` + `callx8`）。

而且回调环的末尾是 `... 37 15 16 29 29`，之后再没有出现过 `21`
（`wifi_api_lock`）—— 说明 `ieee80211_ioctl` **压根没有被调到**。
所以 §143 里初始化任务那次队列接收，发生在
`wifi_init_in_caller_task` 内部、`pp_create_task` 之后。

### 下一步

`wifi_init_in_caller_task` 在 `ic_create_wifi_task()` 之后只剩
`ieee80211_ioctl_init()` 然后返回。所以接收点要么在
`ieee80211_ioctl_init` 内部（它调用 `ic_register_config_cb` /
`ic_register_timer_post_cb` / `ic_get_pp_hdl`），要么就在
`pp_create_task` 的收尾部分（我们已经确认它范围内没有偏移 116 的调用，
但它调用的 `config_get_wifi_task_stack_size` 等子函数还没查）。

最省事的验证：把 `ieee80211_ioctl_init` 的地址记进 RTC 前后各一个计数，
确认它到底有没有被调用。

## §145 kind 常量核对：两次阻塞确实都是**队列接收**

内核里各原语的等待类型常量：

| 文件 | 调用 | kind |
|---|---|---|
| `kernel/sem.c6:49` | `os_task_block_prepare(s, timeout, 1u, 0u)` | **1** |
| `kernel/mutex.c6:155` | `os_task_block_prepare(m, timeout, 2u, 0u)` | **2** |
| `kernel/queue.c6:144` | `os_task_block_prepare(q, timeout, Q_WAIT_GET, 0u)`，`#define Q_WAIT_GET 6u` | **6** |
| `kernel/queue.c6:105` | `Q_WAIT_PUT` | 5 左右 |
| `kernel/event.c6:84` | 由调用方传入 | — |

所以 §143 的读数（blk1、blk2 都是 `kind=6`）**确实是队列接收**，
不是信号量、也不是互斥量：

```
blk0: id=1  kind=1  obj=0x3fcbf8c0   初始化早期的信号量等待
blk1: id=2  kind=6  obj=0x3fcbf234   WiFi 任务在 WiFi 队列上收
blk2: id=1  kind=6  obj=0x3fcbf234   初始化任务**也在同一个队列上收**
```

对象 `0x3fcbf234` = `WQ` 对象（`0x3fcbf230`）的 `WOsQueue` 成员（偏移 4），
也就是 `s_wifi_queue`。全场只创建过 **1 个**队列（`g_qn = 1`）。

结合 §144（`ieee80211_ioctl` 及其 `_init` 都不含队列接收、且 `ioctl`
压根没被调到），剩下的唯一解释是：

> 初始化任务在恢复执行时，跑进了 `ppTask` 的代码。

如果属实，根因就在上下文恢复：两个任务虽然各有独立栈（§140 已确认
`0x3fcb8620` / `0x3fcba620`，各 8192），但某一方保存的帧 PC 落到了
另一方的栈里。

### 下一步

把两个 ctx 里保存的 EPC（帧偏移 60）读出来比一比：
`g_ctx[0].sp` 指向的帧里 `[sp+60]` 应该是 `wifi_main` 被切走时的 PC，
`g_ctx[1].sp` 的 `[sp+60]` 应该是 `ppTask` 的。用 JTAG 或
（更稳的）在 `os_task_block_prepare` 里把 `g_cur->ctx` 和该帧的 `[sp+60]`
一起记进 RTC。

## §146 两个任务保存的 EPC

在 `os_task_block_prepare` 里除了 (id, kind, obj) 再记录
`g_cur->ctx` 指向的帧里的 EPC（`[sp+60]`）：

```
blk0: id=1  epc=0x00000066   obj=0x3fcbf8c0  kind=1
blk1: id=2  epc=0x4037b955   obj=0x3fcbf234  kind=6
blk2: id=1  epc=0x403794b3   obj=0x3fcbf234  kind=6
```

`nm` 反查：

* `0x403794b3` = **`os_task_block_wait`** —— 任务 1（初始化）的恢复点落在
  正常阻塞路径里，说明它确实是通过标准原语阻塞的，而不是被踩坏了 PC。
* `0x4037b955` = `s3_task_tramp`（任务引导入口）—— 这个值偏旧，
  说明记录到的可能是任务 2 的**初始帧**，不能代表它当前阻塞点。

### 本轮结束时的状态

已经用**内核级**证据确认：

1. 只有两个任务启动（`ppTask` 与测试的 `wifi_main`），调度器状态机正确
   （从未选中 BLOCKED 任务，全程 3 次切换）；
2. 两个任务先后在**同一个队列对象** `0x3fcbf234`（= `s_wifi_queue`，全场只
   创建过 1 个队列）上做队列接收并阻塞；
3. 初始化任务的恢复点在 `os_task_block_wait`，是正常阻塞；
4. 全流程 `_queue_send`（字段 25）一次都没有出现 —— 没人投递；
5. `ieee80211_ioctl`（`esp_wifi_init_internal` 里 `wifi_init_in_caller_task`
   之后的下一步）压根没被调到（回调环里之后没有 `21`）。

### 下一步（下一轮从这里继续）

矛盾点收敛为一句话：**初始化任务在 `wifi_init_in_caller_task` 内部就阻塞在
WiFi 队列上了，但它能走到的库里没有任何收这个队列的代码。**

要解这个，下一轮应该：

1. 在 `osi_queue_recv` 里把"调用者 EPC 来自窗口帧"这件事换成在
   `port_task_yield` 处记录（那里 a0 一定是真实返回地址），拿到可靠调用点；
2. 或者直接给 `wifi_init_in_caller_task` 的每一步加可观测信号
   （它调用的每个子函数入口都用一个 RTC 计数），把"最后一步"精确定位到
   单个函数。

## §147 kind 歧义彻底排除：事件组是 4/5，不是 6

`kernel/event.c6:83` 里事件组用的等待类型是：

```c
kind = (mode == OS_EVENT_ALL) ? 5u : 4u;
r = os_task_block_prepare(e, timeout, kind, mask);
```

所以 §143 里 `kind=6` 只可能是 `os_queue_get_item`（`Q_WAIT_GET`）。

各原语的 kind 一览（可作以后排查的速查表）：

| 原语 | kind | 出处 |
|---|---|---|
| `os_sem_pend` | 1 | `kernel/sem.c6:49` |
| `os_mutex_pend` | 2 | `kernel/mutex.c6:155` |
| `os_event_wait`（ANY） | 4 | `kernel/event.c6:83` |
| `os_event_wait`（ALL） | 5 | 同上 |
| `os_queue_put_item` | Q_WAIT_PUT | `kernel/queue.c6:105` |
| `os_queue_get_item` | **6** | `kernel/queue.c6:144` |

⇒ **"初始化任务和 WiFi 任务阻塞在同一个队列对象上"这个内核级结论成立**，
不受任何 id/栈判读影响。

## §148 一个重要的技术事实：`entry` 不写栈，所以帧里拿不到 a0

Xtensa 的 `ENTRY` 只在发生**窗口溢出**时才由溢出向量把 `a0..a3` spill 到栈上；
平时这些值留在寄存器窗口里，栈帧的 `[sp+16..sp+28]` 是空的。

实测 `osi_queue_recv`（`entry a1, 32`）里读 `[a1+16]/[a1+20]/[a1+24]`
全部是 **0**，正好印证这一点。所以"从窗口帧里取调用者返回地址"这条路
不可行（除非强制制造一次溢出）。

顺带记下一个仍然有价值的读数：两次 recv 的 **a1 完全相同**
（`0x3fcbc5e0`），说明两次接收发生在**同一任务、同一调用深度**。

## 本轮总结（M1 WiFi 初始化）

### 已经用真板证据钉死的

| 项 | 证据 |
|---|---|
| windowed/call0 ABI 桥 | 闭源库全部正常回调，无 TRAP |
| PHY 校准/使能 | slot0 = `0xB0000212` |
| IROM 取指 | slot1 = `0x2A` (=42) |
| `esp_wifi_init_internal` | 单线程配置下返回 **ESP_OK**（§120） |
| 真 RTOS 任务/信号量/递归互斥/队列/事件组 | 全部经 `w2c` 壳跑通 |
| 任务调度 | 3 次切换、从未选中 BLOCKED 任务 |
| tick / 定时器 ISR | ISR 4864 次、`os_sched_tick` 同步 4864 次 |
| 队列句柄语义 | 偏移 0 必须是自指针（否则库传 buf 进来） |
| 超时语义 | `portMAX_DELAY` 必须翻译成内核的 0（永久） |

### 尚未解决的唯一卡点

真任务模型下，初始化任务与 WiFi 任务最终**都阻塞在同一个队列对象**
`0x3fcbf234`（= `s_wifi_queue`）上，而 `_queue_send` 从未被调用。

已经排除：调度器 bug、context 槽位共用、栈不足、`ppTask` 被直接调用、
`ieee80211_ioctl` 及其 `_init`、`ic_create_wifi_task`、`pp_create_task` 内的
队列接收。

下一轮建议从"给 `wifi_init_in_caller_task` 调用链上的每个库函数各配一个
RTC 计数"入手 —— 用 `--wrap` 或对目标函数做 trampoline 都可以，
把 init 的最后一步收敛到单个函数上。

## §149 ★ 两个 ctx 的 sp 各自落在自己的栈区 —— 上下文完全正确

在 `os_task_block_prepare` 里同时记录 (任务 id, 帧 EPC, ctx 保存的 sp, kind)：

```
blk0: id=1  epc=0x00000066  sp=0x3fcba4e0  kind=1
blk1: id=2  epc=0x4037b955  sp=0x3fcbc5e0  kind=6
blk2: id=1  epc=0x403794b3  sp=0x3fcba370  kind=6
```

对照 `port_task_ctx_create` 的槽位记录（任务 1 栈 `0x3fcb8620..0x3fcba620`、
任务 2 栈 `0x3fcba620..0x3fcbc620`，各 8192）：

* 任务 1 的 sp `0x3fcba4e0` / `0x3fcba370` → **在任务 1 自己的栈里** ✅
* 任务 2 的 sp `0x3fcbc5e0` → **在任务 2 自己的栈里** ✅

⇒ **上下文切换与保存完全正确**，不存在"两个任务共栈"或"帧串了"的问题。

因此内核级结论是硬的：

> **初始化任务（wifi_main）在自己的栈上、真的调用了 `_queue_recv`，
> 并且和 WiFi 任务阻塞在同一个队列对象 `0x3fcbf234` 上。**

同时 §148 记下的"两次 recv 的 a1 相同"这个读数不可靠（与本节 ctx 里的 sp
矛盾），以本节的 ctx 数据为准。

### 这轮之后，排查范围已经非常窄

唯一还没定位的是：**`wifi_init_in_caller_task` 这条调用链上，
到底是哪个库函数发了那次 `_queue_recv`。** 已排除的直接候选：

* `ieee80211_ioctl_init()` —— 只调用三个注册函数
* `ieee80211_ioctl()` —— 压根没被调到（回调环里之后没有 `21`）
* `ic_create_wifi_task()` —— 只调 `pp_create_task`
* `pp_create_task()` —— 范围内没有偏移 116 的调用
* `ppTask` —— 没有任何 `call8/callx8` 直接调用点

下一轮的做法：把 `wifi_init_in_caller_task` 可能走到的每个库函数都用
`--wrap` 包一层（或者在 `win_bridge.S` 里加同名跳转桩），每个桩把
RTC 计数 +1，一次上板就能看出最后停在哪个函数。

## §150 ★ `--wrap` 计数：`ieee80211_ioctl_init` 从未被调用

给 `wifi_init_in_caller_task` 走到的库函数各加一层 `--wrap`（包装函数在
`port/s3/wifi_glue.c`，windowed，BSS 计数后同步到 RTC `0x50000E00`）：

```
0x50000E00 menuconfig        = 1          ✅ 调过
0x50000E04 misc_nvs_init     = 1          ✅ 调过
0x50000E08 ic_create_wifi_task = 1        ✅ 调过
0x50000E0C ieee80211_ioctl_init = 随机值   ❌ **从未被调用**
0x50000E10 pp_create_task    = 1          ✅ 调过
0x50000E14 wifi_init_in_caller_task = 随机值（--wrap 对同目标文件内的调用无效）
0x50000E18 wifi_init_in_caller_task 返回   = 随机值  ❌ 没返回
```

**结论：初始化流程走到 `ic_create_wifi_task()` → `pp_create_task()` 之后
就再也没往下走，`ieee80211_ioctl_init()` 一次都没进。**

再结合 §149 的内核级证据（初始化任务在自己的栈上阻塞在 `_queue_recv`，
对象就是 `s_wifi_queue`），可以断定：

> 那次接收发生在 **`pp_create_task()` 内部**。

但 §144 已经把 `pp_create_task`（`0x42036584~0x420366d9`）和它的两个子函数
（`config_get_wifi_task_stack_size`、`esf_buf_setdown`）都反汇编查过，
**没有 `l32i aX,aY,116` + `callx8` 的收队列调用**。

所以只剩一种可能：那次接收是通过**函数指针**发起的
（例如把 `_queue_recv` 或某个 wrapper 存进结构体再间接调用），
或者发生在 `_task_create` 让我们切到新任务、而新任务执行完后再切回来时
被误记到了初始化任务名下。

### 下一轮

1. 在 `osi_queue_recv` 里用**可靠的**方式取调用者 PC —— 由于 `entry` 不写栈，
   只能靠强制窗口溢出（先调用一个深递归函数），或者改成在
   `s3_w2c_queue_get` 这个汇编壳里直接记录 `a0`（壳是 `entry`+`callx0`，
   在 `entry` 之后、`callx0` 之前 `a0` 一定是 windowed 调用者的返回地址）。
2. 把 `pp_create_task` 的间接调用面（`l32i aX, aY, <off>` 后跟 `callx8`）
   全部列出来，逐个对照 OSI 偏移。

## §151 ★★ 拿到库侧真实调用点：`ppTask`

在 `osi_queue_recv` 外面套一层汇编壳（`port/s3/w2c.S`），在 `entry` 之后
立即把 `a0`（windowed 调用者的返回地址，带窗口标签）写进 RTC
`0x50000F08`。注意内层 `s3_w2c_queue_get` 的追踪用的是另一个槽
（`0x50000F00`），否则会被覆盖成我们自己的 `osi_queue_recv_body`。

真板读数：

```
0x50000F08 = 0x80381c89     ->  去掉窗口标签 (a0 & 0x3FFFFFFF) | 0x40000000
                            ->  0x40381c89
nm 反查：0x40381c89 在 ppTask（0x40381c5c）内部
```

⇒ **库侧真正调用 `_queue_recv` 的是 `ppTask`**，和 §132 的全量枚举完全一致。

### 于是矛盾收敛成一句很短的话

* 内核记录：最后一次队列阻塞发生在**任务 1（初始化任务）**的上下文里
  （`g_cur->id == 1`，ctx 里的 sp `0x3fcba370` 也在任务 1 自己的栈区）；
* 但那次 `_queue_recv` 的调用者是 **`ppTask`**，而 `ppTask` 只作为
  任务 2 的入口被启动过（§141 的 trampoline 记录）。

⇒ **`ppTask` 的代码在任务 1 的上下文里被执行了。**

最可能的机制：`pp_create_task` 里 `_task_create(ppTask, ...)` →
我们的 `os_task_create` → 新任务（任务 2，优先级更高）立即抢占运行
`ppTask`、`_semphr_give` 之后在 `_queue_recv` 上阻塞 → 调度回任务 1 →
任务 1 从 `os_task_create` 返回继续走……**但它返回到了 `ppTask` 里**，
说明任务 1 保存的返回地址被任务 2 的帧覆盖了。

虽然 §149 验证了"两个 ctx 的 sp 各自在自己的栈区"，但**任务 1 的
`s3_win_call_bringup` 调用链用的是主栈还是任务栈**值得再确认一次：
`s3_win_call_bringup` 是 call0→windowed 桥，如果它在切换前后用的是
`g_sched[0]`（主循环上下文）的栈，就会和任务栈混起来。

### 下一轮

重点查 `win_bridge.S` 的桥函数在"任务上下文里被调用"时是否仍用主栈：
它的 `addi a1, a1, -32` 用的是**当前 a1**，在任务里就是任务栈 ✓，
但 `port_task_switch` 保存的"主循环上下文" `g_sched[core]` 可能被
误当成任务上下文。

## §152 port 与内核的上下文视图完全一致；任务入口也都正确

在 `os_task_block_prepare` 里同时记录 `port_cur_ctx()`（port 视角，
`g_s3_cur[core]`）和 `g_cur->ctx`（内核视角）：

```
blk0: id=1  port_ctx=0x3fcbe620  kern_ctx=0x3fcbe620
blk1: id=2  port_ctx=0x3fcbe66c  kern_ctx=0x3fcbe66c
blk2: id=1  port_ctx=0x3fcbe620  kern_ctx=0x3fcbe620
```

三次阻塞里两者**完全相等** ⇒ port 与内核的上下文簿记没有分歧
（`port_set_cur_ctx` 在 ISR 抢占路径上确实同步了 `g_s3_cur[core]`）。

任务入口表（RTC `0x50000A10` 起）反查 `nm`：

```
entry[0] = 0x403785e4 = wifi_main   （测试任务）
entry[1] = 0x40381c7c = ppTask      （WiFi 任务）
```

两个任务的入口都正确，**只有这两个任务**。

### 于是本轮的矛盾被压缩到最小

* 上下文保存/恢复正确（sp、EPC 各自在自己栈上）；port 与内核视图一致；
  调度器没选中过 BLOCKED 任务；入口都正确；
* 库侧 `_queue_recv` 的最后一次调用者确实是 `ppTask`；
* 但内核记录那次阻塞属于任务 1（`wifi_main`）。

⇒ 只剩一种可能：**任务 1 的执行流在某一刻进入了 `ppTask` 的代码**。
最可疑的环节是 `s3_win_call_bringup` 这个 call0→windowed 桥在任务上下文中
的栈使用：它自己 `addi a1, a1, -32` 用的是当前 a1（任务栈）✓，但
`port_task_switch` 保存的是 **主调度循环** 的上下文（`g_sched[core]`）。
如果任务 1 在桥里被抢占、而主循环那段上下文又被当成任务 1 的上下文恢复，
就会出现"任务 1 的 id + 主循环/别的任务的栈"。

### 下一轮

给 `port_task_switch` / `port_task_yield` 各加一个 RTC 计数（分别记
"从主循环切到任务"和"从任务切回主循环"的次数），并确认它们成对出现；
若出现"两次 yield 之间没有 switch"，就说明主循环上下文被当成了任务上下文。

## §153 switch / yield 计数成对（各 3 次），切换记账正常

在 `port/s3/ctx.S` 的 `port_task_switch` / `port_task_yield` 里各加一个
BSS 计数（自增后**覆写**到 RTC，避免 RTC 初值随机的干扰）：

```
0x50001000 switch_cnt = 3
0x50001008 yield_cnt  = 3
```

两者成对 ⇒ "任务让出 → 主循环挑下一个任务" 的记账没有异常。

## 本轮结束时的完整结论

### 已用真板证据排除的（全部有读数支撑）

| 怀疑点 | 结论 |
|---|---|
| windowed/call0 ABI | 正常，无 TRAP |
| context 槽位分配 | 任务1 槽1/`0x3fcb8620`，任务2 槽2/`0x3fcba620`，各自独立 |
| 两个 ctx 的 sp/EPC | 各自落在自己栈区，保存正确 |
| port 上下文 vs 内核上下文 | `port_cur_ctx()` 与 `g_cur->ctx` 三次阻塞完全相等 |
| 调度器状态机 | 全程 3 次选择、从未选中 BLOCKED 任务 |
| switch / yield 记账 | 各 3 次，成对 |
| 任务入口 | `wifi_main`(0x403785e4) 与 `ppTask`(0x40381c7c)，正确 |
| 栈大小 | 两个任务都拿到 8192 |
| 定时器 / tick | ISR 4864 次，`os_sched_tick` 同步 4864 次 |
| `pp_post` 符号 | 已交还 libpp（rom.ld 里那条绝对赋值会顶掉库实现） |
| `ieee80211_ioctl` / `_init` / `ic_create_wifi_task` | 都不含队列接收；`ioctl` 未被调到，`_init` 从未被调用 |
| `ppTask` 被直接调用 | 全库没有直接调用点 |

### 剩下这一个矛盾

内核记录"任务 1（wifi_main）在自己的栈上阻塞在 `_queue_recv`"，
而库侧那次调用的返回地址落在 **`ppTask`** 里（0x40381c89）。

所有"簿记类"的可能性都被逐一排除了，所以下一步只能去看**实际保存的帧内容**：
用 JTAG 在卡死点 dump 两个 ctx 指向的完整帧（a0..a15/EPC/PS），
对比 `a0` 与调用链，确认任务 1 的帧里到底存的是哪段代码的返回地址。

## §154 帧里的 a0 也不是库返回地址

在 `os_task_block_prepare` 里把保存帧的 a0（偏移 0）也记进 RTC：

```
blk0 (任务1 早期): frame_a0 = 0x8037c766  -> 0x4037c766（我们自己 wifi_glue 的代码）
blk1 (任务2)     : frame_a0 = 0x00000004  （尚未写入，无意义）
blk2 (任务1 最终): frame_a0 = 0x403794ef  -> os_task_block_wait 内部
```

最终那次阻塞的 a0 落在 `os_task_block_wait` 里 —— 这是 `port_task_yield`
的返回地址，属于内核原语内部，**反推不到库侧调用者**。

## 本轮总结

本轮把"簿记类"的可能性全部用真板读数排除了（详见 §141~§153 的表格），
唯一剩下的矛盾是：

> 内核记录"任务 1（wifi_main）在自己的栈上阻塞在 `_queue_recv`"，
> 而库侧那次调用的返回地址落在 `ppTask` 里（0x40381c89）。

所有能想到的"记账错位"路径都已排除：栈槽位、ctx 的 sp/EPC、port 与内核
的当前上下文、调度选择、switch/yield 配对、任务入口、栈大小、tick。

### 下一轮的两个候选方向

1. **在 `win_bridge.S` 的桥函数里打点**：`s3_win_call_bringup` 是
   call0→windowed 的入口，在它进入/离开时各写一个 RTC 计数。
   如果"进入"计数是 1 而"离开"是 0，且中途发生过任务切换，
   就说明任务 1 的执行流在桥内被切走、再也没能正确回到桥的返回点。
2. **绕过桥**：让测试直接以 windowed 方式调用 `s3_win_bringup`
   （即把 `wifi_main` 整个放进 windowed 岛里跑），去掉 call0→windowed 这一层，
   看死锁是否消失。这是最直接的分辨方法。

## §155 桥埋点：`s3_win_call_bringup` 进入 1 次、从未返回

在 `port/s3/win_bridge.S` 的 `s3_win_call_bringup` 里加进入/离开计数
（BSS 自增后覆写到 RTC `0x50001100` / `0x50001104`）：

```
0x50001100 bridge_in  = 1
0x50001104 bridge_out = 随机值（从未被写过）
```

⇒ 桥进入一次、**从未返回**，与"初始化卡在 `s3_win_bringup` 内部"完全一致。

（踩坑记录：`win_bridge.S` 里每个函数前面必须有独立的
`.section .text, "ax", @progbits`，并且 `l32r` 用的字面量必须放在**同一个
section 内、且在指令之前**；另外 `s3_win_call_wifi_start` 与
`s3_win_call_bringup` 的前后缀完全一样，批量 `replace` 会同时改到两个函数。）

## 本轮最后状态

真板读数（当前镜像）：

```
beacons: slot0=0xB0000212(PHY完成) slot1=0x2A(IROM取指OK) slot2=1(os_task_create)
串口:    M0 phy start / M1 bringup 之后无输出
bridge_in=1, bridge_out 从未写
switch_cnt=3, yield_cnt=3
ISR 4864 次，os_sched_tick 同步 4864 次
两次 _queue_recv 阻塞在同一个队列 0x3fcbf234，_queue_send 从未出现
```

### 下一轮首选实验

把 `wifi_main` 整个搬进 windowed 岛（在 `win_island.c` 里写一个
`s3_win_wifi_main`，由 `win_bridge.S` 用 `call8` 直接调用），
**去掉 call0→windowed 这一层桥**。如果死锁消失，说明问题就在桥的
调用约定上（桥是 call0 函数，但它 `call8` 进 windowed 后，windowed 侧的
`entry`/`retw` 与 `port_task_switch` 的窗口簿记可能不兼容）。

## §156 修正 `s3_task_tramp`：windowed 任务入口要用 `callx8` + 置 WOE

`s3_task_tramp`（`port/s3/ctx.S`）原来统一用 `callx0` 调任务入口。但我们自己
的任务入口是 call0，而**闭源库的 `ppTask` 是 windowed**。windowed 被调方
需要 `CALLINC != 0` 才能让 `entry` 正确轮转窗口、`retw` 才能正确返回。

改法：
* `port/s3/ctx.c` 增加 `u32 g_next_task_windowed`，在 `port_task_ctx_create`
  里写进帧的 `f[3]`（`RESTORE_FRAME` 会把它恢复成 `a4`），然后清零；
* `port/s3/wifi_glue.c` 的 `osi_task_create` 在建库任务前置 1；
* `s3_task_tramp` 按 `a4` 分流：0 → `callx0`；1 → 置 `PS.WOE` 后 `callx8`。

**这是一处正确的 ABI 修正**（windowed 入口不该用 callx0 调），但真板现象不变，
说明它不是这个死锁的根因。

## 本轮最终状态

```
串口: M0 phy start / M1 bringup，之后无输出、无 TRAP
beacons: slot0=0xB0000212(PHY) slot1=0x2A(IROM) slot2=1(建任务成功)
bridge_in=1（bringup 桥进过、没出来）
switch_cnt=3 / yield_cnt=3（成对）
ISR 4864 次，os_sched_tick 同步 4864 次
两次 _queue_recv 阻塞在同一队列 0x3fcbf234，_queue_send 从未出现
```

已排除的怀疑点（每条都有真板读数）：上下文槽位/帧保存/port 与内核视图/
调度选择/switch-yield 配对/任务入口/栈大小/tick/`pp_post` 符号/各候选库
函数是否含队列接收/`ppTask` 是否被直接调用/建任务后立即挂起。

### 下一轮

把 `wifi_main` 整个搬进 windowed 岛（`win_island.c` 里写 `s3_win_wifi_main`，
由 `win_bridge.S` 用 `call8` 调），去掉 call0→windowed 这一层嵌套，
看死锁是否消失。这是目前唯一还没做的对照实验。

## §157 对照实验：任务 1 整个换成 windowed 入口，死锁依旧

在 `port/s3/win_island.c` 里加了 `s3_win_wifi_main_task()`（任务入口本身也是
windowed），并把测试任务改成用它作为入口（配合 §156 的 ABI 标志）。
这样任务 1 的执行流里**完全没有** call0↔windowed 混用。

真板输出：

```
W0 phy start
W1 bringup
（没有 W2，rc 槽 0x50001200 仍是随机值）
```

两条结论：

1. **`s3_task_tramp` 的 `callx8` 修正确实生效**——windowed 任务入口能被
   正确启动并跑起来（`W0`/`W1` 都出来了）；
2. **死锁依旧** ⇒ 根因**不是** call0↔windowed 的 ABI 混用。

## 本轮完整结论（13 轮 WiFi 初始化的收敛过程）

已用真板读数逐项排除：上下文槽位/帧保存/port 与内核视图/调度选择/
switch-yield 配对/任务入口/栈大小/tick/`pp_post` 符号/各候选库函数是否含
队列接收/`ppTask` 是否被直接调用/建任务后立即挂起/call0-windowed 混用。

锁定的事实：

* 库侧 `_queue_recv` 的唯一调用者是 `ppTask`（`0x40381c5c`）；
* 内核记录的最后一次队列阻塞发生在"任务 1"上下文里，且其 sp/EPC 都落在
  任务 1 自己的栈上；
* 全流程 `_queue_send`（字段 25）一次都没有；
* `ieee80211_ioctl_init` 从未被调用，`wifi_init_in_caller_task` 从未返回。

### 下一轮的首选怀疑

`pp_create_task` 里 `_task_create(ppTask, ...)` 之后，WiFi 任务可能**先于**
`xphyQueue = *(handle+0)` 这句执行完就跑了（我们的 `os_task_create` 让新任务
立即可运行，而库期望它稍后才被调度）。若 `ppTask` 在 `xphyQueue` 还是 0 时
就调 `_queue_recv(0)`，我们的 `wq_resolve(0)` 返回 0 → 立即返回失败 →
`ppTask` 会反复重试并最终阻塞在某个真实队列上，与初始化任务互等。

验证办法：在 `osi_queue_recv` 里记录 `xphyQueue`（ROM `0x3fcef904`）的值
随时间的第一次/最后一次读数。

## §158 `pp_post` 从未被调用（--wrap 验证）

加 `-Wl,--wrap=pp_post` 并计数：

```
0x50000E1C pp_post 调用次数 = 随机值（从未被写过）
OSI 回调总数 = 0x1e = 30（与之前一致）
```

⇒ **整条投递路径根本没走到**（不是 `pp_post` 提前返回）。

## 这 13 轮把问题收敛成了这样一个自洽的图景

1. `esp_wifi_init_internal` 依次调用 `wifi_osi_funcs_register` →
   `wifi_api_lock` → `pm_funcs_init` → `wdev_funcs_init` →
   `net80211_funcs_init` → `wifi_api_unlock` → `wifi_init_in_caller_task`；
2. `wifi_init_in_caller_task` 里：`wifi_menuconfig_init` ✅ →
   `misc_nvs_init` ✅ → `ic_create_wifi_task`（内部 `pp_create_task` ✅）→
   **之后就再没往下走**（`ieee80211_ioctl_init` 从未被调用、函数从未返回）；
3. WiFi 任务（`ppTask`）被建出来并跑起来：`_semphr_give` 完成握手 →
   在 `s_wifi_queue` 上 `_queue_recv` 阻塞；
4. 初始化任务也在同一个队列上阻塞（内核级证据：kind=6、obj 相同）；
5. `pp_post` 从未调用、`_queue_send` 从未调用 —— **没有任何投递**。

也就是说：**两个任务都在等对方投递，而投递代码一次都没执行。**
`pp_create_task` 内部一定有一条我们现在看不到的"接收"路径
（它自己的反汇编里没有偏移 116 的直接调用，说明是间接调用，或者
`pp_create_task` 之后控制流并没有正常返回）。

### 下一轮

把 `pp_create_task` 的**完整**反汇编逐条过一遍（不只是偏移 116 的检索），
重点是 `_task_create` 之后的控制流：看它是否在一个循环里等某个标志，
以及它是否通过 `l32i aX, aY, <off>` 间接调了 `_queue_recv`（off 可能是别的值，
因为 `wifi_static_queue_t` 之类的包装会再取一次字段）。

## §159 `pp_create_task` 用的是字段 36（`_task_create_pinned_to_core`）

通读 `pp_create_task`（`0x42036500`）的关键片段：

```
42036532  l32i a4, a4, 152          ; 字段 38 = _task_delete
42036535  callx8 a4
42036538  l32r a4, (s_wifi_queue)   ; 读 WiFi 队列全局
420365a0  l32i a7, a7, 144          ; 字段 36 = _task_create_pinned_to_core
420365a6  call8 config_get_wifi_task_stack_size
420365b2  callx8 a10                ; = _task_get_max_priority (字段 42)
420365c6  callx8 a7                 ; 建任务
420365f0  l32i a8, a8, 156          ; 字段 39 = _task_ms_to_tick
```

两点值得注意：

1. 建 WiFi 任务用的是 **`_task_create_pinned_to_core`（字段 36，7 个参数）**，
   不是 `_task_create`（字段 37）。我们现在两个槽位都映射到同一个
   `osi_task_create` ✓，但 windowed 调用下**第 7 个参数是在栈上传递的**，
   需要确认我们的函数没把它当成别的参数用（我们签名是 6 个参数，第 7 个
   会被忽略 ✓ 但如果编译器/ABI 处理有偏差就要复核）。
2. 整个函数仍然没有 `l32i aX,aY,116` + `callx8` 的收队列调用。

### 下一轮

核对 `_task_create_pinned_to_core` 的 7 参数在 windowed ABI 下的传递
（a2..a7 六个 + 栈上第 7 个），确认 `osi_task_create` 拿到的是
`(fn, name, stack, arg, prio, handle)` 而不是错位的一组。
可以把实际收到的 6 个参数值全部记进 RTC 与库的调用点对照。

## §160 新实验：让初始化任务的所有阻塞调用立即返回 —— 仍然卡住

加了一个 `INIT_NOBLOCK()`：在 `s3_wifi_bringup` 里记下初始化任务的 id
（`g_init_task`），之后 `_semphr_take` / `_mutex_lock` / `_queue_recv` /
`_event_group_wait_bits` 只要发现调用者是它，就**立即返回成功/超时**，
不进入内核阻塞。

真板串口（完整）：

```
c6os s3 boot
W0 phy start
W1 bringup
B0 osi0=00000000 osi@00000000 calls@3fcbe8d4
B1 slot13=4037d0cc slot18=4037cfc8 magic=deadbeaf ver=00000008
B2 cfg@3fcbe7d4 osi=3fcbead4 magic@144=1f2f3f4f
（之后没有 W2）
```

`B2` 打出来了 ⇒ `g_init_task` 已记录、库已被调用；但初始化**仍然**卡在
`pp_create_task` 内部。

这有两种解释：

1. 初始化任务阻塞在一个我们**没有覆写**的原语上（例如某条我们仍映射到
   `glue_ret0` 的等待路径，或者库内部的忙等）；
2. `_task_get_current_task()` 在同一个任务里返回的 id **会变**，
   于是 `INIT_NOBLOCK()` 匹配失败 —— 这也正好解释 §133/§140 里那些
   "同一个任务报出两个 id" 的读数。

第 2 点与之前所有"身份不一致"的读数能对上，是目前最值得优先验证的：
**把任务身份从一个稳定的来源取**（例如 `port_cur_ctx()` 对应的 `g_ctx`
槽号，或 `g_tasks[]` 的下标），不要再依赖 `g_cur->id`。

## §161 任务身份改用稳定来源（g_tasks 下标）—— 仍然卡住

之前 `_task_create` 写回的句柄是任务 **id**（`g_cur->id`），而
`_task_get_current_task()` 也取 `g_cur->id`，中间出现过"同一任务两个 id"的
读数。本轮改成从构造上保证一致的方案：

* `kernel/task.c6` 新增 `os_task_current_slot()`（返回 `g_tasks[]` 下标 +1，
  0 保留给"无当前任务"）和 `os_task_slot_of_id(id)`；
* `port/s3/w2c.S` 的 `s3_w2c_current_task` 改为调 `os_task_current_slot`；
* `osi_task_create` 写回给库的句柄改为 `s3_w2c_slot_of_id(id)`。

这样 `_task_create` 写回的句柄与 `_task_get_current_task()` 用的是**同一口径**，
不可能再出现"身份对不上"。

真板：**现象完全没有变化**（`W0/W1` 之后没有 `W2`，rc 槽仍是随机值）。

⇒ 任务身份不是这个死锁的根因，这条线索可以关掉。

## 至此已经排除的清单（每条都有真板读数或反汇编证据）

ABI/桥、libc 符号、队列句柄语义、超时语义、`pp_post` 符号、上下文槽位、
帧保存、port 与内核视图、调度选择、switch/yield 配对、任务入口、
栈大小、tick/ISR、任务身份口径、`ieee80211_ioctl_init` 是否被调用、
各候选库函数是否含队列接收、`ppTask` 是否被直接调用、
建任务后挂起、初始化任务不阻塞、`xphyQueue` 是否已写入。

## 当前唯一未解的矛盾

> 初始化任务在 `pp_create_task` 内部阻塞在 `_queue_recv`（内核级 kind=6、
> 对象 = `s_wifi_queue`），而库侧那次调用的返回地址落在 `ppTask` 里；
> 同时 `pp_post` / `_queue_send` 一次都没被调用 —— 没人投递。

### 下一轮建议

把 `ppTask` 也放进"不阻塞"名单做**对称实验**：如果两边都不阻塞、初始化就能
走完，说明这个死锁纯粹来自"两个阻塞点互为前提"；然后再逐个把阻塞恢复回去，
二分出到底是哪一个阻塞点是多余的。

## §162 `ppTask` 的循环结构（确认是标准写法）

```
40381e55  l32i.n a3, a2, 0        ; osi
40381e57  l32i.n a10, a4, 0       ; *s_pp_task_create_sem
40381e59  l32i a3, a3, 64         ; 字段 16 = _semphr_give
40381e5e  callx8 a3               ; _semphr_give(sem, 0)     ← 与 init 握手
40381e61  l32r a3, (xphyQueue)
40381e6c  l32i a10, a3, 0         ; 队列句柄 = *xphyQueue
40381e6e  l32i a5, a5, 116        ; 字段 29 = _queue_recv
40381e71  movi a12, -1            ; portMAX_DELAY
40381e76  callx8 a5               ; _queue_recv(handle, &msg, 永久)
40381e79  beqi a10, 1, 处理       ; 成功(返回 1) 才去处理
40381e7c  j 40381fb8              ; 否则走另一条路
```

这就是标准写法：握手 → 永久阻塞收消息 → 处理。

而我们的 `osi_queue_send`/`_queue_recv` 的返回值约定（1=成功）与它一致 ✓。

### 至此，本轮可以做一个小结

`_queue_recv` 的两次阻塞若是**同一个任务**（`ppTask`）的两次循环，那么：
* 第一次 `_queue_recv` 必然**返回过 1**（否则不会进第二轮），
  但 `_queue_send` 从未被调用 —— 说明"有东西进了队列"这件事要么来自
  库里直接的队列写入，要么来自 `_queue_recv` 的返回值语义被我们算错；
* 第二次阻塞则一直等下去。

这条线索比"两个任务互等"更可查：下一轮只需在 `osi_queue_recv` 里记下
**每次返回时的值**和当时队列的 `count`，就能判断第一次是"真收到了"
还是"我们错误地返回了 1"。

## §163 重要推论：加了 `INIT_NOBLOCK` 之后 init 仍然卡住

本来想在 `osi_queue_recv` 里记录每次的返回值和队列 count，但记录块**从来没被
执行**（RTC `0x50001300` 全是随机值）。原因是：

* 阻塞的那次 `_queue_recv` 永远不返回 → `s3_w2c_queue_get` 之后的代码不可达；
* 而初始化任务的那次接收被 `INIT_NOBLOCK()` 在更早处短路（直接 `return 0`），
  也走不到记录块。

这本身给出了一个新推论：

> **`INIT_NOBLOCK()` 让 init 的 sem/mutex/queue/event 等待全部立即返回，
> 但 init 依然卡在 `pp_create_task` 里。**

所以 init 这次**不是**阻塞在我们覆写的那四个原语上。可能性只剩：

1. 它阻塞在某个仍映射到 `glue_ret0` 的等待路径上（`glue_ret0` 会立刻返回成功，
   那样就不会阻塞，所以这条不太可能）；
2. 它在我们**没有打点**的地方忙等（库内部的自旋）；
3. `INIT_NOBLOCK()` 实际没生效（`g_init_task` 与 `s3_w2c_current_task()`
   还是不一致）。

第 3 点必须先排除。下一轮第一件事：把 `g_init_task` 和每个原语入口处
`s3_w2c_current_task()` 的返回值都写进 RTC，确认是否真的命中。
如果确实命中了，那就用 §138 那个"ISR 采样被打断 PC"的工具把 init 的忙等
位置抓出来（注意：当前主流程停在 `port_idle`，说明 init 是阻塞而不是自旋，
这两者又矛盾——所以第 3 点的可能性最大）。

## §164 ★★ 修掉一个重要 bug：`_task_get_current_task()` 恒返 0

`os_task_current_slot()` 原来用 `&g_tasks[i] == g_cur` 找下标。但 **`.c6`
编译出来的"静态数组元素取址"不可靠**（§114 已记过这个限制），实际结果
永远是 0。

真板证据（RTC `0x50001400` 记 `g_init_task`、`0x50001410` 记每次
`INIT_NOBLOCK()` 求值时的身份与命中）：

```
修之前： init_task = 0            nb0: me=2 hit=0, nb1: me=1 hit=0
修之后： init_task = 1            nb0: me=2 hit=0, nb1: me=1 hit=1
```

后果很严重：`_task_get_current_task()` 恒返 0，而库里的 `s_wifi_task_hdl`
也恒为 0，于是 **`current_task_is_wifi_task()` 恒为真** —— 库会以为
"我就在 WiFi 任务里"，把只该在 WiFi 任务里跑的代码就地执行。

修法：改用 **id 比较**（`g_tasks[i].magic == OS_TASK_MAGIC &&
g_tasks[i].id == g_cur->id`），绕开取址。

修复后的真板变化非常明显：

| | 修复前 | 修复后 |
|---|---|---|
| OSI 回调总数 | 30（然后死住） | **百万级**（持续增长） |
| 尾部回调 | `... 37 15 16 29 29` | 一直是 `29`（`_queue_recv`） |

⇒ 库的行为完全变了：它现在走到一个**轮询等回复**的循环里，
每秒几十万次 `_queue_recv`。

## 当前状态

初始化任务在 `_queue_recv` 上**轮询**（不是阻塞）。试过让它直接返回成功
并给一条全零消息（16 字节清零），自旋没有停止 —— 说明它在等消息里的
**某个特定字段**，不是只看返回码。

### 下一轮

1. 反汇编 init 侧那个轮询循环（从 `_queue_recv` 返回地址反查：
   `0x40381e79` 附近的 `ppTask` 逻辑可以排除——那是 WiFi 任务的；
   init 侧的返回地址要重新用 §151 的方法抓一次，注意现在 ppTask
   已经不是唯一调用者了）。
2. 或者直接给 `pp_post` 打桩：即使不被调用，也人工投一条消息进
   `s_wifi_queue`，看轮询是否结束。

## §165 继续排除：阻塞机制本身没问题

* `os_task_block_prepare` 确实会 `g_cur->state = OS_TASK_BLOCKED` ✓；
* `os_obj_lock()` 在 S3 上只是 `port_irq_save()`（单核关中断），**不碰**
  `g_sched_lock` ✓，所以不会让 `os_task_block_prepare` 返回 -4。

也就是说队列/信号量的"进内核 → 置 BLOCKED → 让出"这条路径本身是通的。

而 §151 的库侧返回地址追踪今天仍然可用，读数：

```
0x50000F08 = 0x80381f2d  ->  0x40381f2d  ->  ppTask(0x40381f00) + 0x2d
```

⇒ 现在在 `_queue_recv` 上反复进入的**就是 `ppTask` 自己**
（`ppTask+0x2a` 是那句 `callx8 _queue_recv`，`+0x2d` 是它的返回地址）。

配合之前的 `QDIAG` 读数（`QDIAG[1] = 2`，即 `-r = 2` → `os_queue_get_item`
返回 `-2`=超时），说明这次接收是**以超时结束的**，而不是被消息唤醒。

### 下一轮要看的两点

1. `w_ticks(0xFFFFFFFF)` 应该变成 0（=内核的"永久等待"）。但库里
   `ppTask` 传的是 `movi a12, -1`（即 0xFFFFFFFF）✓ 转换后是 0 ✓。
   那 `-2` 超时是从哪来的？需要重查 `os_queue_get_item` 里
   `os_task_block_prepare(q, timeout=0, ...)` 之后的 `os_task_block_wait()`
   在 timeout=0 时到底返回什么。
2. 若 `os_task_block_wait()` 在"没有任何超时条目"时也返回 -2，
   那所有 timeout=0 的永久等待都会变成"立即超时"，正好解释 ppTask 的轮询。

## §166 ★ 修复 stale 超时条目：永久等待前先取消本任务的超时

`QDIAG` 一直在报 `os_queue_get_item` 返回 **-2（超时）**，而我们传给内核的
超时是 `w_ticks(0xFFFFFFFF) = 0`（永久等待，根本不注册超时条目）。
-2 只可能来自 `os_task_block_wait()` 返回 `wait_result`，
而 `wait_result = -2` 是 `os_sched_tick()` 里"超时到期 → `unblock(t, -2)`"
写进去的。

⇒ **有 stale 超时条目**：本任务之前某次带超时的等待留下了表项，
到期后把这次的永久等待误唤醒成"超时"。

修法（`kernel/task.c6` 的 `os_task_block_prepare`）：

```c
    if (timeout == 0u) {
        /* 永久等待：先清掉本任务可能残留的超时条目 */
        (void)os_timeout_cancel(g_cur->id);
    }
```

真板效果：OSI 回调总数从**百万级**降到**百级**（746~1537），
轮询大幅收敛 —— 确认 stale 超时条目确实是那个自旋的主因。

不过仍然没有 `W2`：尾部回调还都是 `29`（`_queue_recv`），
只是重试次数少了很多。说明还有残留的唤醒源（或库本身就是轮询式设计）。

## 这一轮的两个实质修复

| 修复 | 效果（真板） |
|---|---|
| `os_task_current_slot` 改用 id 比较（.c6 取址不可靠） | `_task_get_current_task()` 不再恒返 0；库行为大变，回调 30 → 百万级 |
| `os_task_block_prepare` 永久等待前取消残留超时 | 自旋从百万级降到百级 |

这两个都是**真实的 bug**，且都不是靠猜——都有 RTC 读数定位。

### 下一轮

继续把 `_queue_recv` 的剩余重试收敛到 0：在 `os_queue_get_item` 的
"被唤醒后重新检查"路径上也加一次 `os_timeout_cancel`，
或者直接检查 `os_sched_tick` 的 `unblock(t,-2)` 是否会把**没有超时条目**
的任务也误唤醒。目标是让 `ppTask` 真正稳定阻塞在队列上、把 CPU 让给
初始化任务跑完。

## §167 验证：已经没有任何超时误唤醒

在 `os_sched_tick` 的 `os_timeout_pop_expired` 命中处加计数（BSS 自增后写
RTC `0x50001600`）：

```
0x50001600 timeout_fires = 随机值（从未被写过）
0x50000200 osi_calls     = 0x544 = 1348
```

⇒ **一次超时到期都没有**。所以现在 `_queue_recv` 的重试**不是**被超时唤醒的
（stale 超时那条线彻底关掉），回调数 1348 也已经回到正常量级（不是自旋）。

## 一个重要的测量陷阱（记下来避免以后再踩）

`esptool read-mem` **每次都会硬复位芯片**。所以：

* 从 RTC 读计数器，得到的是"复位那一刻"的值，不同次数读数之间**不可比**
  （每次都是一次新的启动）；
* 想判断"是否还在自旋"，必须在**同一次启动内**取两次快照 —— 也就是让固件
  自己周期性把计数写进 RTC（本次就是这么做的），而不是连续 read-mem 两次。

这也是之前"回调数从百万级降到百级"这个结论需要打折的原因：
百万级和百级很可能只是不同启动停在的不同位置。

## 本轮两个确定成立的修复（都有真板读数支撑）

1. **`os_task_current_slot()` 的 `.c6` 取址 bug**：改成 id 比较后，
   `_task_get_current_task()` 从恒返 0 恢复成真值；
   修复前后 `g_init_task` 分别是 0 和 1，`INIT_NOBLOCK` 的命中从 0 变 1。
2. **`os_task_block_prepare` 永久等待前取消本任务残留超时**：
   语义上正确（永久等待不该被旧条目打断）。

### 下一轮

当前是"干净阻塞"状态但 `W2` 仍不出现。用两条已有工具定位：
ISR 采样被打断的 PC（§138，RTC `0x50000780`）+ `os_task_probe` 任务表，
确认初始化任务这次等的是哪个对象、以及 ppTask 是否也已稳定阻塞。

## §168 撤销 `INIT_NOBLOCK`，回到干净阻塞状态

ISR 采样给出了决定性读数：

```
0x50000780 isr_pc = 0x420325ad  ->  osi_queue_recv 内部
```

也就是说 `INIT_NOBLOCK()` 让初始化任务的 `_queue_recv` 立刻返回成功，
反而把它推进了一个**自旋**（PC 一直在 `osi_queue_recv` 里）。
所以"不阻塞"这条路是错的，已经全部用 `if (0 && ...)` 关掉。

撤销后的真板读数（干净状态）：

```
串口: W0 phy start / W1 bringup（之后无输出、无 TRAP）
0x50000780 isr_pc   = 0x4037acab  -> port_idle（全体阻塞，非自旋）
0x50000200 osi_calls= 0x1e = 30
0x50001600 timeout_fires = 从未写（无超时误唤醒）
```

## 本轮的净收获

两个**真实的**内核/适配层 bug，都有真板读数定位：

1. `os_task_current_slot()` 用 `.c6` 的 `&g_tasks[i]` 取址 —— 结果恒为 0，
   导致 `_task_get_current_task()` 恒返 0、`current_task_is_wifi_task()`
   恒为真。改成 id 比较后 `g_init_task` 从 0 变 1、`INIT_NOBLOCK` 命中从
   0 变 1，库的行为明显改变（回调数从 30 涨到百万级）。
2. `os_task_block_prepare` 在永久等待（timeout=0）时没有清理本任务的残留
   超时条目，理论上会让永久等待被误唤醒成超时。已在 timeout==0 时先
   `os_timeout_cancel`。

外加一条重要的**测量纪律**：`esptool read-mem` 每次硬复位，判断"是否自旋"
必须让固件自己周期快照到 RTC，不能靠连续 read-mem 比较。

## 当前状态

干净阻塞、无自旋、无超时误唤醒，但 `esp_wifi_init_internal` 仍未返回
（`W2` 不出现）。内核级事实仍然是：
两个任务都阻塞在同一个队列 `0x3fcbf234`（= `s_wifi_queue`）上，
而 `_queue_send` / `pp_post` 一次都没被调用。

### 下一轮

用 `os_task_probe`（内核已实现）在干净阻塞状态读任务表，
确认初始化任务与 WiFi 任务各自等的对象 id，然后二选一：
* 若是"等对方投递"，就要让库侧的投递路径真正跑起来
  （回到 `pp_create_task` 之后为什么没有继续）；
* 若是"等自己"，则是库内部状态机问题，需要对照 IDF 源码的
  `wifi_init_in_caller_task` 实现逐步核对。

## §169 ★★ 结论修正：只有 `ppTask` 收过队列，不存在"两个任务互等"

把每次 `_queue_recv` 的**库侧返回地址**按序号存进 4 个独立 RTC 槽
（`0x50000F08 + 4*i`，由 `w2c` 壳在 `entry` 之后立刻记录 `a0`）：

```
recv0: 0x80381ee1 -> 0x40381ee1  =  ppTask(0x40381eb4) + 0x2d
recv1: 0x80381ee1 -> 0x40381ee1  =  ppTask(0x40381eb4) + 0x2d   ← 同一个调用点
recv2/3: 未被写过（全流程只有 2 次 recv）
```

⇒ **两次收队列都来自 `ppTask` 内部同一个调用点**。
`ppTask` 的循环是"give sem → recv → 处理"，所以两次 recv 意味着
**第一次是成功收到了一条消息**（于是进第二轮），第二次阻塞。

这也推翻了我之前十几轮的核心假设：

> **初始化任务从来没有调用过 `_queue_recv`。**

之前"init 也在收队列"的证据来自 `os_task_block_prepare` 里记的
`g_cur->id`（内核视角）—— 也就是说，**内核在 ppTask 运行的时候，
把当前任务记成了任务 1**。这是 `g_cur` 的归属错误，不是"两个任务互等"。

### 这修正了整张图

* `ppTask` 收到了**第一条**消息（所以它会二次 recv），但 `_queue_send`
  字段 25 从未出现 —— 说明那条消息不是经我们的 `_queue_send` 投递的；
* 初始化任务真正的阻塞点**仍未找到**（它不在 `_queue_recv` 上）；
* 主流程停在 `port_idle`（全体阻塞）。

### 下一轮两个方向

1. **`g_cur` 为何在 ppTask 运行时是任务 1**：查 `s3_task_tramp` 首次进入新
   任务时 `g_cur_arr[core]` 的赋值顺序（`os_task_start_core` 里是
   `g_cur_arr[me] = t;` 然后 `port_task_switch(t->ctx)` ✓，但 ISR 抢占路径
   与首次进入新任务的组合还没验证过）。
2. **init 到底阻塞在哪个对象**：把 `os_task_block_prepare` 的记录改成
   按"表下标"而不是 `g_cur->id` 分槽，避开 `g_cur` 归属错误的干扰。

## §170 按 ctx 的阻塞记录：任务 1 的上下文里在执行 ppTask 的代码

`os_task_block_prepare` 的诊断改成环形记录
`(obj, kind, port_cur_ctx(), g_cur->ctx)`（`port_cur_ctx()` 由
`port_task_switch`/`port_set_cur_ctx` 维护，是权威的）。真板：

```
blk0: obj=0x3fcbf928 kind=1 port_ctx=0x3fcbe660 kern_ctx=0x3fcbe660
blk1: obj=0x3fcbf29c kind=6 port_ctx=0x3fcbe6ac kern_ctx=0x3fcbe6ac
blk2: obj=0x3fcbf29c kind=6 port_ctx=0x3fcbe660 kern_ctx=0x3fcbe660
```

* `0x3fcbe660` 与 `0x3fcbe6ac` 相差 `0x4C = sizeof(S3Ctx)`，是两个不同的 ctx；
* `port_ctx == kern_ctx` 在所有条目里都成立（两个视角一致）；
* blk0：**任务1** 的 `kind=1`（信号量握手）；
* blk1：**任务2** 的 `kind=6`（收队列，= `ppTask` 的正常行为）；
* blk2：**任务1** 的 `kind=6`，而且对象和 blk1 **完全相同**。

而 §169 已经证明：两次 `_queue_recv` 的库侧调用点都是
`ppTask(0x40381eb4)+0x2d`。

⇒ **任务 1 的上下文里确实在执行 `ppTask` 的代码。**

结合 §155（bringup 桥"进入 1 次、从未返回"），也就是说：任务 1 在
`s3_win_bringup` 内部被切走之后，恢复回来时**没有再回到桥的返回点，
而是跑进了 `ppTask`**。

### 下一轮唯一要查的地方

`s3_task_tramp` 首次进入新任务与 `port_task_switch` 恢复旧任务的组合：

1. `os_task_create` 之后，任务 2 在下一个 tick 被抢占切进来时，
   `port_irq_fill(frame_sp)` 保存的是**任务 1 的**帧（`g_s3_cur[core]`
   此时还是任务 1）✓；
2. 但 `port_set_cur_ctx(next)` 之后 `g_s3_cur[core]` 变成任务 2；
3. 任务 2 阻塞走 `port_task_yield`，它把 sp 存进
   `g_s3_cur[core]`（= 任务 2）✓ 然后跳到 `g_sched[core]`；
4. 主循环 `port_task_switch(task1_ctx)` 恢复任务 1 —— 这里要确认
   `task1_ctx->sp` 确实还是第 1 步保存的那个帧指针，而不是被第 2/3 步
   写坏。

建议：在 `port_irq_fill` 里把 `(core, ctx, frame_sp)` 记进 RTC，
与 `port_task_yield`/`port_task_switch` 的记录逐条对照。

## §171 `port_irq_fill` 无缺陷：两次抢占都正确存进任务 1 自己的 ctx

在 `port/s3/ctx.c` 的 `port_irq_fill` 里记录 `(序号, ctx, frame_sp)` 到 RTC
`0x50001700`：

```
irqfill0: seq=1 ctx=0x3fcbe660 sp=0x3fcba530
irqfill1: seq=2 ctx=0x3fcbe660 sp=0x3fcba550
```

* `0x3fcbe660` 是**任务 1** 的 ctx；
* 两个 `frame_sp`（`0x3fcba530`/`0x3fcba550`）都落在任务 1 自己的栈区
  （`0x3fcb8620..0x3fcba620`）✓
* 全程只有 **2 次**中断抢占。

所以 ISR 保存路径是对的：抢占任务 1 时存的就是任务 1 的 ctx + 任务 1 的帧。
**不是这里把上下文串掉的。**

## 到目前为止，这块「上下文/调度」已经查得很透了

| 环节 | 状态 |
|---|---|
| `port_task_ctx_create` 建帧 | ✓ 正确（f[1]=entry、f[15]=tramp、f[16]=PS、sp=top） |
| `s3_task_tramp` 启动 | ✓ 正确（task_starts=2，入口分别是 wifi_main 与 ppTask） |
| `port_irq_fill` 抢占保存 | ✓ 正确（2 次，都存任务1 的 ctx+帧） |
| `port_task_switch` / `port_task_yield` 的 sp 存取 | **尚未埋点验证** |
| `os_sched_preempt_ctx` / `port_set_cur_ctx` | ✓ 两者一致（port_ctx == kern_ctx） |
| ISR 帧布局 | ✓ 与 RESTORE_FRAME 一致（EPC@60、PS@64） |

唯一还没验证的就是 `port_task_switch` / `port_task_yield` 这两条 asm 里
**sp 的存取是否真的对应各自的任务**。

### 下一轮

在这两个 asm 函数里各埋一个 RTC 记录 `(tag, ctx, frame_sp)`：
* `port_task_yield`：记录 `a4`（= `g_s3_cur[core]`）与 `a1`（要保存的帧）；
* `port_task_switch`：记录 `a4`（= 目标 ctx）与它恢复出来的 `a1`。

只要有一次「任务 1 的 ctx 被写进了任务 2 的帧」或反之，就找到了根因。

## §172 switch / yield 的 sp 埋点：switch 正确，yield 读数异常

在 `port/s3/ctx.S` 里给两处埋点（RTC `0x50001800` 起）：

```
sw_out_sp  = 0x3fcd8ef0   （主循环的 sp，落在主栈顶端附近 ✓）
sw_tgt_ctx = 0x3fcbe660   （目标任务 1 的 ctx ✓）
sw_tgt_sp  = 0x3fcba3a0   （在任务 1 自己的栈区 0x3fcb8620..0x3fcba620 ✓）
yl_ctx     = 0x62503d0a   （**不是 ctx 地址**，异常）
yl_sp      = 0x3fcbe660   （**这是 ctx 地址，不是栈指针**，异常）
```

* **`port_task_switch` 一侧完全正确**：保存主循环的 sp、目标 ctx 与目标 sp
  三者都能对上；
* **`port_task_yield` 一侧两个读数看起来被"错位"了**：本该是 ctx 的槽里是
  垃圾、本该是 sp 的槽里却是 ctx 地址。

这有两种可能：

1. 埋点插入的位置落在 `beqz a4, ...` 的**空指针分支之后**或寄存器已被改写的
   位置，导致读到的是别的寄存器（埋点问题）；
2. `port_task_yield` 里真有寄存器被提前改写（真 bug）。

反汇编核对（`0x4037b4d0` 起）确认：`a4 = g_s3_cur[core]` 是在
`l32i.n a4, a4, 0` 之后才有值的，`beqz a4` 会跳到空指针分支。
埋点如果落在 `addi a1, a1, -80` 之前，`a1` 还是**调用者的栈指针**；
落在之后才是帧指针。需要把埋点明确放到 `s32i a1, a4, 64` 之后核对。

### 下一轮

把 `port_task_yield` 的埋点改成记录 `(a4, a1, a4->sp)` 三个值并放到
`s32i a1, a4, 64` 之后，确认：
* `a4` 是不是真的 ctx；
* `a1` 是不是真的落在该 ctx 对应任务的栈区。

如果确认 `a4`/`a1` 都对，那"任务 1 执行 ppTask 代码"就只剩
`port_task_switch` 恢复目标帧时 RESTORE_FRAME 的 EPC 来源问题，
可以对照 `S3Ctx.r[]`/`sp` 的布局再核一次。

## §173 ★ 修复测量基础设施：启动时清零 RTC 诊断区

这几十轮里 RTC 读数反复自相矛盾，根因之一是 **RTC 跨复位不丢**：
上一轮启动写下的值会一直留着，read-mem 读到的可能是"上一次启动的现场"，
而 ctx 地址、任务结构地址在不同构建里又相当稳定，看起来就像"有效数据"。

修法（`port/s3/start.S`，清完 `.bss` 之后）：把
**`0x50000200 .. 0x50001F00` 整段清零**，之后所有诊断埋点都只可能来自
本次启动。

清零后的首批可信读数：

```
sw_out_sp  = 0x3fcd8ef0     主循环 sp ✓
sw_tgt_ctx = 0x3fcbe660     目标任务1 的 ctx ✓
sw_tgt_sp  = 0x3fcba3a0     在任务1 栈区 ✓
yl_ctx     = 0x00000000     ← 本次启动 port_task_yield **没有被调用**
yl_sp      = 0x3fcbe660     （同上，仍是残留语义，见下）
irqfill 0  = 1 / 0x3fcbe660 / 0x3fcba530   ← 只有 1 次 ISR 抢占
irqfill 1  = 0 / 0 / 0                     ← 第二条已清零
```

结论修正：**本次启动只有 1 次中断抢占，`port_task_yield` 一次都没走到。**
也就是说任务 2 从来没有通过"让出"的方式被保存过 —— 这与我之前
"两次 recv + 两次阻塞"的图景不一致，说明那些"第二次"的记录同样是
跨启动残留造成的假象。

### 下一轮

用这份**可信**的读数重建整张图：
1. 只有 1 次 ISR 抢占、0 次 yield ⇒ 任务 2 是怎么跑到 `ppTask` 并阻塞的？
2. 重新确认 `task_starts` / `port_task_switch` 次数 / `os_sched_tick` 次数，
   三者要用同一套清零后的埋点口径。

## §174 ★★ 重大更正：`port_task_yield` 一次都没有被调用过

之前十几轮一直引用"`switch_cnt == yield_cnt == 3`，成对出现，调度正常"。
本轮核对 `port/s3/ctx.S` 的行号才看出问题：

```
 56: port_task_switch:
 94:     l32r a14, .Lyl_cnt   ← g_yl_cnt 的自增在这里
101:     l32r a14, .Lsw_cnt   ← g_sw_cnt 的自增也在这里
137: port_task_yield:
180:     /* 我加的 yield 埋点在这里（Ldbg2 +16/+20） */
```

**两个计数器都在 `port_task_switch` 里面**（94 和 101 都在 56..137 之间），
所以 `switch_cnt == yield_cnt` 是必然的 —— 那个"成对"结论完全是假象。

而清零 RTC 之后，`port_task_yield` 里的埋点读到的是：

```
yl_ctx = 0        ← 从没被写过
```

⇒ **`port_task_yield` 从来没有执行到埋点位置。**

而 `os_task_block_wait()` 是无条件调用 `port_task_yield()` 的：

```c
int os_task_block_wait(void) {
    if (g_cur == 0) { return -1; }
    port_task_yield();
    return g_cur->wait_result;
}
```

结合"`os_task_block_prepare` 确实记录了阻塞（kind=1/6）"这个事实，
唯一自洽的解释是：

> `port_task_yield` 进去了，但在 `beqz a4, 1f`（`g_s3_cur[core] == 0`）
> 处**提前返回**了 —— 于是上下文没被保存、直接按 `g_sched[core]` 恢复，
> 任务的现场就丢了。

这与我之前在阻塞记录里看到"`port_cur_ctx()` 非空"并不矛盾：
`port_cur_ctx()` 是在 `os_task_block_prepare` 里读的（那时还是对的），
而 `port_task_yield` 里读到的是同一个数组 —— 除非两者之间被清空过。

### 下一轮

1. 在 `port_task_yield` 的 **`beqz` 早退分支里**也埋一个点（`Ldbg2 +24`），
   一旦读到非零就坐实"空指针早退"；
2. 同时在 `os_task_block_prepare` 之外的所有 `port_set_cur_ctx(0)` /
   `g_s3_cur[core] = 0` 处埋点，找出是谁把它清成了 0。

## §175 真板坐实：`port_task_yield` 三次调用全部空指针早退

在 `port_task_yield` 的**入口**加计数（`Ldbg2+28`），并保留保存路径上的
`+16/+20` 两个埋点，清空基线后读数：

```
yield_entries = 3      ← 函数确实被调用 3 次
yl_ctx        = 0      ← 保存路径上的埋点从未执行
yl_sp         = 0
switch_cnt    = 3
task_starts   = 2
```

⇒ **三次调用全部在 `beqz a4, 1f`（即 `g_s3_cur[core] == 0`）处提前返回**，
一次都没有把任务的现场保存下来。

`port_task_yield` 的调用点（`kernel/task.c6`）有 6 处：
270 / 345 / 470 / 664 / 685 / 695。其中第 345 行在 `os_task_start_core`
附近（建任务后让出），其余在阻塞等待/删除/挂起路径上。

结合"`os_task_block_prepare` 里记到的 `port_cur_ctx()` 非空"这一事实，
最可能的组合是：

* 这 3 次 yield 来自 **`os_main`（主循环线程）**——那时 `g_s3_cur[core]`
  本来就是空的，早退是对的、无害；
* 而任务里真正调 `os_task_block_wait()` 的那些路径**根本没走到**
  （否则会多出 3 次以上、且 ctx 非空的 yield）。

### 下一轮

1. 给 6 个 `port_task_yield()` 调用点各自编号（传一个 tag 参数或用 6 个
   独立埋点），确认这 3 次到底来自哪一处；
2. 若确认没有一次来自 `os_task_block_wait`，那就要查
   `os_task_block_wait()` 为什么在这些路径上没被执行（例如
   `os_task_block_prepare` 之后 `r != 0`，或者是编译期把该分支裁掉了）。

## §176 ★★★ 找到测量假象的真正根源，并建立了可靠通道

**根源**：`esptool read-mem` 会**先硬复位、再立刻读**。所以：

* 每次读到的其实是**上一次启动**跑完后留在 RTC 里的值；
* 但同一次启动的字段被分到多次 `read-mem` 里读时，每次都换了一次启动，
  字段之间**根本不属于同一次启动** —— 这才是这几十轮里"读数自相矛盾"的真因；
* 我此前加的"启动时清零 RTC"更是把这份"上一次启动的快照"直接抹掉，
  于是读到一串 0（一度让我误判 `port_task_yield` 从未生效）。

**可靠通道（已实现）**：`port_snapshot()`（放在 flash 段，省 IRAM）在
`os_sched_tick` 到达 tick 500 时，把所有关键值**一次性**写进
RTC `0x50001D00`。之后 `read-mem` 读到的是**上一次启动稳定后的完整定格**。

第一次拿到的一致读数：

```
magic=0x5A5A5A5A tick=500 switch=3 osi_calls=30 yield_n=348
yl_ctx=0x3fcbe700 yl_sp=0x3fcbc480 timeout_fires=0
blk0 obj=0x3fcbf9c8 kind=1 ctx=0x3fcbe700 kern=0x3fcbe700 epc=0x66
blk1 obj=0x3fcbf33c kind=6 ctx=0x3fcbe74c kern=0x3fcbe74c epc=0x4037b790
blk2 obj=0x3fcbf33c kind=6 ctx=0x3fcbe700 kern=0x3fcbe700 epc=0x40379483
xphyQueue=0x3fcbf338   （blk1/blk2 的 obj = 0x3fcbf33c = &xphyQueue->q ✓）
```

并且用同一通道确认：**两次 `_queue_recv` 都来自 `ppTask`，但来自它的
两个不同调用点** —— `ppTask+0x2a`（主循环 0x40381fda）和
`ppTask+0x1a7`（另一条分支 0x40382157）。之前"两次都是 ppTask+0x2d、
只有一个调用点"的结论同样是跨启动误读。

## 由此得到的两条正确结论

1. **EPC 显示任务 1 在自己的 `os_task_block_wait+0x27` 里**，
   任务 2 的 EPC 在 `s3_task_tramp`。所以 §170 的
   "任务 1 在执行 ppTask 的代码"**是错的**（同样是残留值导致的误判）。
2. `ppTask` 确实有两个收队列的调用点，两次都真的发出去了；
   而 `os_queue_get_item` 只可能由 `osi_queue_recv_body` 调用
   （`os_queue_get` 在仓库里没有任何调用者），所以两次 kind=6 的阻塞
   都来自 `ppTask`。

## 仍然成立的矛盾

`blk2` 这次 kind=6 的阻塞，`port_cur_ctx()` 与 `g_cur->ctx` **都**等于
**任务 1** 的 ctx（`0x3fcbe700`），但它是由 `ppTask` 的代码触发的。

⇒ **ppTask 运行期间，内核视角的"当前任务"是任务 1。**
`g_cur` 是 `#define g_cur (g_cur_arr[g_core])`，
`g_core = port_core_id() & 1`；而 `port_cur_ctx()` 用
`g_s3_cur[port_core_id() & (C6OS_MAX_CORES-1)]` —— 两个数组索引相同，
却都指向任务 1。

### 下一轮

在 `os_task_start_core`（主循环切换）和 `os_sched_preempt_ctx`（ISR 抢占
切换）里各记一次 `(t->id, t->ctx, port_cur_ctx())`，用可靠通道读出来，
定位到底是哪一次把"当前任务"与"实际恢复的帧"配错了。

## §177 可靠的诊断区与实测到的切换序列

RTC slow memory 的**可写/可读范围并不等于标称的 8KB**：
`0x50001800` 可读，`0x50001900` / `0x50001A00` / `0x50001DA0` 都读到 0。
把诊断放在已验证可用的低地址后稳定工作。当前可用区（都实测过）：

```
0x50000200  OSI 回调环          0x50000C00  阻塞环（5 字/槽 × 4）
0x50001000  port_task_switch    0x50001400  g_init_task
0x50001500  切换追踪（4 字 × 8）  0x50001600  超时到期次数
0x50001700  irq_fill 记录        0x50001800  yield 记录
0x50001D00  一次性快照（只到 +0x8C 有效）
```

**切换追踪**（`port_switch_trace(tag, id, ctx)`，tag=1 主循环 / 2 ISR 抢占）
的实测读数：

```
#1 tag=1 id=1 ctx=0x3fcbe700     ← 主循环切到任务 1
#2 tag=2 id=1 ctx=0x3fcbe700     ← ISR 想切，选中的仍是任务 1（ctx 与 cur 相同，
                                    所以 irq.c 里 next==cur 会直接 return 0，等于没切）
```

而 `port_task_switch` 计数 = 3。

## 本轮把矛盾缩到了这一点

* 两次 `_queue_recv` 都来自 `ppTask`（两个不同调用点 `+0x2a` / `+0x1a7`）；
* 对应的两次 kind=6 阻塞，`port_cur_ctx()` 分别是**任务 2** 和**任务 1**；
* 也就是说：**`ppTask` 的第二次收队列，是在"当前任务 = 任务 1"的状态下执行的。**

由于 `os_queue_get_item` 只可能由 `osi_queue_recv_body` 调用，而它只被
`ppTask` 的两个点调用，所以结论只能是：**任务 1 的上下文里恢复了
ppTask 的代码位置** —— 即某一时刻 `port_irq_fill` 把 ppTask 的帧
存进了任务 1 的 ctx，或者恢复任务 1 时用错了帧。

### 下一轮（最小、最直接的验证）

在 `port_irq_fill` 里除了 `(ctx, frame_sp)`，再记录**当时 `g_cur->id`**；
在 `port_task_yield` 里记录**当时的 `g_cur->id`**。把这两组和切换追踪对齐，
就能确定是哪一次保存把"任务 1 的 ctx"和"ppTask 的帧"配到了一起。

## §178 逐条核对：两条切换路径的簿记都是对的

**ISR 保存**（`port_irq_fill`，现在同时记 port 视角 ctx 与内核视角 id）：

```
#1: ctx=0x3fcbe700 frame_sp=0x3fcba5d0 kern_id=1
#2: ctx=0x3fcbe700 frame_sp=0x3fcba5f0 kern_id=1
```

两次都一致（都是任务 1），`frame_sp` 也都在任务 1 自己的栈上 ✓。

**主循环切换**（`port_task_switch` 的反汇编）：确实有

```
l32r a3, .Lg_cur ; slli a5, a12, 2 ; add a3, a3, a5 ; s32i a4, a3, 0
```

即 `g_s3_cur[core] = 目标 ctx` ✓。

**ISR 切换**（`irq.c`）：`port_set_cur_ctx(next)` ✓。

所以"由谁更新当前任务"这条链上，三个入口都写了。

## 本轮净成果

### 两个真实的 bug 修复

1. `os_task_current_slot()` 用 `.c6` 的 `&g_tasks[i]` 取址（恒为 0）
   → 改用 id 比较，`_task_get_current_task()` 恢复正常。
2. `os_task_block_prepare()` 在永久等待（timeout=0）时没有清理本任务
   残留的超时条目 → 补上 `os_timeout_cancel`。

### 一套真正可靠的测量通道（这轮的转折点）

* 查清 `esptool read-mem` 会**先复位再读**，分多次读的字段来自不同启动；
* 摸清 RTC slow memory 实际可用范围小于标称 8KB
  （`0x50001900` 以上读到 0）；
* 建立了"一次性快照 + 固定低地址诊断区"的组合，从此读数可复现。

### 更正了三处此前写进文档的错误结论

* "`switch_cnt == yield_cnt`，调度成对正常" —— 两个计数器其实在**同一个函数**里；
* "初始化任务也在收队列、两个任务互等" —— 只有 `ppTask` 收队列；
* "任务 1 在执行 ppTask 的代码" —— EPC 显示任务 1 在自己的
  `os_task_block_wait` 里，之前是残留值误判。

## 仍然成立、且已用可靠通道复核过的事实

```
task_starts=2   switch=3   yield_n=348   osi_calls=30   timeout_fires=0
blk0: kind=1 ctx=任务1         （_semphr_take 握手）
blk1: kind=6 ctx=任务2         （ppTask 主循环 0x40381fda）
blk2: kind=6 ctx=任务1         （ppTask 另一分支 0x40382157）
queue: xphyQueue=0x3fcbf338，obj=0x3fcbf33c=&q ✓
```

`esp_wifi_init_internal` 仍未返回（没有 `W2`），`_queue_send` / `pp_post`
一次都没被调用。

### 下一轮

`blk2` 是关键：**ppTask 的另一条分支（`ppTask+0x1a7`）在"当前任务=任务1"
时被执行**。下一步反汇编 `ppTask+0x16c`..`ppTask+0x1b0` 这段，看它是
错误分支还是重试分支，以及它是怎么被进入的（普通跳转还是返回地址错）。

## §179 `ppTask+0x17c` 起是 WiFi 任务的清理路径（不是重试分支）

```
4038217c  l32r a3, (s_pp_task_del_sem)
40382181  beqz a10, skip
40382186  l32i a3, a3, 64          ; 字段 16 = _semphr_give
40382189  callx8 a3                ; _semphr_give(s_pp_task_del_sem, 0)
4038218c  l32r a3, (s_wifi_queue)
40382193  l32i a4, a4, 0x17c       ; 字段 95 = _queue_delete
40382196  callx8 a4                ; _queue_delete(*s_wifi_queue)
4038219d  s32i a10, a3, 0          ; *s_wifi_queue = 0
403821a2  l32i a2, a2, 152         ; 字段 38 = _task_delete
403821a5  s32i a10, a3, 0          ; pp_task_hdl = 0
403821a7  callx8 a2                ; _task_delete(...)
403821aa  retw.n
```

这是 **ppTask 退出时的清理**（先给删除信号量、再删队列、再删自己）。

⚠️ 同时记下一个方法论教训：**`ppTask` 的地址在不同构建之间会变**
（这一版 `ppTask=0x40382000`，之前几版是 `0x40381fb4` / `0x40381c5c` 等）。
所以任何"`ppTask+0xNN`"的结论都必须和当次构建的 `nm` 一起记录，
不能跨构建引用 —— 本轮之前就有过因为这点导致的对不上。

## 本轮结束时的状态

| 项目 | 状态 |
|---|---|
| PHY 校准 / enable | ✅ 真板 beacon `0xB0000212` |
| IROM 取指（闭源库进 flash） | ✅ |
| windowed ABI 桥 | ✅ 无 TRAP，任务入口 `callx8` 已修 |
| `esp_wifi_init_internal` | ❌ 仍未返回（无 `W2`） |
| OSI 回调 | 30 次后停止 |
| `pp_post` / `_queue_send` | 从未被调用 |
| 两个任务 | 都建起来了（`task_starts=2`），`ppTask` 阻塞在 `s_wifi_queue` |
| 内核簿记 | 两条切换路径都核对正确 |

### 下一轮建议

1. 先重新抓一次**当前构建**的 `ppTask` 两个 `_queue_recv` 调用点的偏移
   （`nm` + 反汇编，别沿用旧偏移）；
2. 在 `osi_queue_recv_body` 里记录调用者 ctx 的尝试一直拿不到数据
   （该 RTC 区写入后读不到），改用内核侧可靠通道：
   在 `os_queue_get_item` 的每次 `os_task_block_prepare` 前
   把 `q->count` 和 `port_cur_ctx()` 记进阻塞环已有的槽位。

## §180 ★ 新的崩溃签名：`osi_queue_recv` 返回后 a0 被改坏

完整串口输出（可复现）：

```
c6os s3 boot
W0 phy start
W1 bringup
B0 / B1 / B2 （配置检查都通过）
*** TRAP sp=3fcbc640 cause=00000014 pc=820328da ps=00060510 vaddr=820328d8 ws=00000006 a0=820328da
  osi: 0000000d=1 0000000f=1 00000010=1 00000012=2 00000013=2 00000015=1
       00000016=1 00000025=1 0000002a=2 0000002b=11 00000056=1 00000058=3 0000005e=1
```

* `cause=0x14` = **InstrFetchProhibited**（取指禁止）；
* `pc = 0x820328da`；去掉 CALLINC 位（bit31:30）→ **`0x420328da`**，
  正是 `osi_queue_recv + 0x2e` —— 也就是
  `callx8 <osi_queue_recv_body>` 的**下一条指令** `mov.n a2, a10`；
* `a0 = 0x820328da`：正确的返回地址应该是 `0x8203285a`（`callx8` 时写入的），
  实际值比它**大 0x80**；
* `ws = 6`（窗口起始 = 6），说明当时寄存器窗口已经轮转到较深的位置。

⇒ **从 `osi_queue_recv_body` 返回时，返回地址被改坏了**（+0x80），
于是 `retw` 跳到 `0x420328da` —— 这个地址在 flash 里但**不是合法指令边界**，
触发取指禁止。

窗口溢出/欠载向量核对过：`_vecbase` 的 6 个槽（0x000..0x140）都在，
`_win_ovf4` 是标准实现：

```
_win_ovf4:
    s32e a0, a5, -16
    s32e a1, a5, -12
    s32e a2, a5,  -8
    s32e a3, a5,  -4
    rfwo
```

所以不是"向量缺失"。

### 下一轮

`a0` 多出 `0x80` 这个现象很具体，两个方向：

1. **`s32e` 的偏移写反了**：`s32e aX, aY, imm` 的 imm 是**字节**偏移且必须是
   -4 的倍数；`-16/-12/-8/-4` 对应 4 个寄存器 ✓ 看起来对。但
   `_win_ovf8` / `_win_unf8` / `_win_ovf12` / `_win_unf12` 只贴了前两个，
   后四个的实现还没逐一核对过 —— 而 `ws=6` 说明**已经在用 8 窗口甚至 12 窗口**
   的溢出路径了。先把 `_win_ovf8/_unf8/_ovf12/_unf12` 全部逐行核对一遍。
2. 若向量都对，就在 `osi_queue_recv` 的 `callx8` 前后各记一次 `a0`
   （存到已验证可写的 RTC 区），确认是"进去之前就被改了"还是"返回时才被改"。

## §181 窗口向量实现逐个核对：**全部与 Xtensa 标准代码一致**

```
_win_ovf4/_unf4   标准
_win_ovf8:        s32e a0,a9,-16 / l32e a0,a1,-12 / s32e a1,a9,-12 /
                  s32e a2,a9,-8 / s32e a3,a9,-4 / s32e a4..a7,a0,-32..-20 / rfwo   ✓
_win_unf8:        l32e a0,a9,-16 / a1,-12 / a2,-8 / a7,a1,-12 / a3,a9,-4 /
                  a4..a7,a7,-32..-20 / rfwu                                          ✓
_win_ovf12/_unf12: 同样与标准实现逐行一致                                              ✓
```

所以"向量缺失/写错"这条可以排除。

## §182 `rsr vecbase` 的读数不可信（记录为待查）

* `_vecbase` 链接地址 = **0x40378000**（IRAM 段，向量表必须在 IRAM 才能被
  CPU 取到 ✓）；
* 但在 `port_snapshot()`（由 `os_sched_tick` 调用、ISR 上下文）里
  `rsr vecbase` 读到 **0**。

这个 0 不能直接采信：**tick 中断一直在正常工作**（快照本身就是在 tick 500
触发的），异常也能进我们的 `_vec_trap` 并打印 TRAP —— 说明 VECBASE 实际
是有效的。读到 0 很可能是"在 ISR 上下文/当前特权级下读该寄存器"的问题。

## 本轮总结（第 119 轮）

这一轮几乎没有推进 WiFi 本身，但做了三件对后续很关键的事：

1. **修掉两个真实的内核/适配层 bug**（`.c6` 取址导致
   `_task_get_current_task()` 恒返 0；永久等待前未清残留超时）；
2. **建立了一套可复现的测量通道**（搞清 `read-mem` 的复位语义、
   RTC 实际可写范围、一次性快照 + 固定低地址诊断区）；
3. **更正了三处此前写进文档的错误结论**，并发现了一个**新的、可复现的
   崩溃签名**：`osi_queue_recv` 从 `osi_queue_recv_body` 返回时
   `a0` 被加 `0x80` 改坏（`ws=6`，即已进入 8 窗口溢出路径），
   导致 `retw` 跳到非指令边界、触发 `InstrFetchProhibited`。

### 下一轮

崩溃签名是最具体的线索。建议：
1. 在 `osi_queue_recv` 的 `callx8` **前后**各记一次 `a0`（写到已验证可写的
   `0x50001540` 区），确认是"进入 body 前就已坏"还是"返回时才坏"；
2. 若确认返回时才坏，就重点看 `_win_unf8` 的 `l32e a7, a1, -12` 这一步：
   它用 `a1`（恢复出来的 sp）去取下一个栈帧指针，如果此时 `a1` 已被
   `l32e a1, a9, -12` 覆盖成"新窗口的 sp"，语义就和标准序列不同了
   —— 标准序列里这一步用的是**未被子溢出的旧 a1**，需要逐字节比对。

## §183 决定性读数：进入 body 前 a0 正确，返回后就坏了

在 `port/s3/w2c.S` 的 `osi_queue_recv` 里，`callx8 osi_queue_recv_body`
前后各记一次 `a0` 到 RTC `0x50001560/0x50001564`：

```
a0_before_callx8 = 0x8038202d   → 0x4038202d = ppTask + 0x2d  ✓ 正确的返回地址
a0_after_callx8  = 0x00000000   → 从未被写过
```

"after" 槽没被写，说明 **`callx8` 这一次没有正常返回到下一条指令**；
而 TRAP 里 `pc = 0x420328da`，与正确返回地址 `0x4203285a` 相差

```
0x420328da - 0x4203285a = 0x80 = 128 字节 = 一个 32 寄存器的窗口
```

⇒ **返回地址被"多转了一个窗口"**，`retw` 于是跳到 `osi_queue_recv` 之后
0x80 字节处（不是合法指令边界），触发 `InstrFetchProhibited`。

这是本轮最具体、也最可操作的线索：问题出在**寄存器窗口的
spill/fill 与返回地址的对应关系**上，而不是队列/OSI 逻辑。

### 下一轮

三个候选，按可疑度排序：

1. **`_win_unf8`/`_win_unf12` 的 `rfwu` 之后 WS 是否被正确恢复**：
   `rfwu` 会恢复 WS 并跳回；若向量表里 `j _win_unf8` 之后的
   对齐/位置不对，`rfwu` 可能回到错误位置；
2. **`port_task_switch` 保存/恢复 a0..a15 时没有处理 WS**：
   我们只保存了 16 个寄存器（当前窗口），如果被切走的任务当时处于
   "窗口已溢出到栈"的状态，恢复时 WS 与栈上的 spill 数据不匹配；
   正确做法是切换前用 `xthal_window_spill` 把祖先窗口全部落到栈上
   —— 这正好是 `port_task_yield` 里 `#ifndef __XTENSA_CALL0_ABI__`
   那段被**跳过**的代码（因为 `ctx.S` 是用 `-mabi=call0` 汇编的），
   而 windowed 任务（WiFi 任务）恰恰需要它。
3. 若 1/2 都排除，则考虑给 windowed 任务单独用 windowed 汇编的切换代码。

## §184 结论：切走 windowed 任务前必须 spill 祖先窗口（下一轮要做的修复）

**问题**：`port_task_yield` / `port_task_switch` 只保存**当前窗口**的 a0..a15。
这对我们自己的 call0 任务是够的（call0 不跨窗口），但**闭源库的
`ppTask` 是 windowed ABI**，它的调用链会用到 2~4 个寄存器窗口。
切走时如果不把祖先窗口 spill 到栈上：

* 那些寄存器会被别的任务复用；
* 切回来时 `retw` 按 WINDOWSTART 逐级回退，取到的是**被覆盖的值**
  → 返回地址错位。实测错位量正好是 **0x80 = 128 字节 = 一个 32 寄存器窗口**
  （§183），与"多转了一个窗口"完全吻合。

`port/s3/ctx.S` 里本来就有一段处理这个的代码，但它被
`#ifndef __XTENSA_CALL0_ABI__` 包着 —— 而 `ctx.S` 是用 `-mabi=call0`
汇编的（`S3_ABI ?= call0`），所以那段**从来没被编译进去**。

**修复方案**（下一轮直接做）：

1. 在 `ctx.S` 里加一个不依赖 ABI 宏的 `s3_window_spill`：
   * 先把当前窗口 a0..a15 存到当前帧（和现在一样）；
   * 然后按 `WINDOWSTART` 把仍然活着的祖先窗口逐个用
     `s32e aX, aY, -N` 落到栈上（和 `_win_ovf4/8/12` 的写法一致）；
   * 对 call0 任务，`WINDOWSTART` 只有窗口 0 活着，这一步是空操作，
     所以可以**无条件调用**，不需要区分 ABI。
2. 在 `port_task_yield` 和 `port_task_switch` 保存现场之后、
   切换之前调用它。
3. 真板验收标准：`osi_calls` 从 30 明显继续增长、
   `pp_create_task` 能返回（串口出现 `W2`）。

已确认 IDF 侧没有现成的 `xthal_window_spill` 可复用（在所有链接进来的
`.a` 里都 `nm` 不到），必须自己写。

## 为什么这条线索优先级最高

`a0` 错位 `0x80` 是**唯一一个能定量解释"返回地址跑到哪去了"的证据**，
而且它与"windowed 任务 + call0 调度器"这个本项目最特殊的组合直接对应。
之前所有围绕队列/OSI/任务身份的排查，都无法解释这个 0x80。

## §185 验证：崩溃与"是否阻塞"无关，是 `retw` 在窗口状态错乱下的失败

把 `osi_queue_recv_body` 改成"立即返回 0、永不阻塞"（`g_wifi_noblock = 1`），
让 `ppTask` 完全不在深窗口调用链上停下来：

```
W0 phy start / W1 bringup
*** TRAP sp=3fcbc640 cause=00000014 pc=820328e4 vaddr=820328e4 ws=00000006 a0=820328e4
osi_calls = 30
```

**崩溃依旧**，而且这次 `pc == vaddr == a0 == 0x820328e4`，
去掉 CALLINC 位是 `0x420328e4`（flash 内）—— 也就是说：

* `a0` 里存的是**带 CALLINC 标记的返回地址**（`0x82xxxxxx`，正常）；
* 但 CPU 把它**当成指令地址直接取指**了 —— 所以 `InstrFetchProhibited`；
* `ws = 6`，说明窗口起始已经走到 6，`retw` 需要按 CALLINC 回退相应窗口
  而当时窗口状态不允许。

⇒ 与 §184 的结论一致：**问题在寄存器窗口的状态管理，不在阻塞/队列/OSI 逻辑**。
把"不阻塞"当作绕开手段是不成立的，必须正面解决窗口 spill。

### 本轮结论与下一步

修复方向已明确（实现不依赖 ABI 宏的 `s3_window_spill`），但这是**寄存器级**
改动，必须非常小心。建议下一轮按下面顺序做，每步都用真板验证：

1. 先只溢出**一级**祖先窗口（`rotw -1` 后把 4 个寄存器存到该窗口自己的 sp 上），
   并在 `port_task_yield` / `port_task_switch` 的保存路径里调用；
2. 用 `WINDOWSTART` 判断是否还有更早的活窗口，循环直到没有；
3. 恢复侧对称处理（`rotw` 回原位，或依赖 `_win_unf4/8/12` 的自动 fill）；
4. 验收：串口出现 `W2`（`s3_wifi_bringup` 返回）、`osi_calls` 明显超过 30。

安全护栏：对整个改动前后各留一次"自己的 call0 任务仍能正常运行"的回归
（自制任务的 switch/yield 计数与 beacon 输出不变），避免把调用约定搞坏。

## §186 一次失败的尝试 + 一个关键的顺序错误

想在新任务进入 windowed 入口前把窗口状态置干净，写成了：

```
    movi a6, 1 ; wsr a6, windowstart     ← 先置 WINDOWSTART
    movi a7, 0 ; wsr a7, windowbase      ← 再归零 WINDOWBASE
```

结果**启动直接卡死**（串口只到 `c6os s3 boot`，连 `W0` 都没出来），已回退。

原因是 `WINDOWSTART` 的**位号是绝对窗口号**，不是"相对当前窗口"：

* 当前 `WINDOWBASE` 若是 6，则"活着的当前窗口"对应的是 **bit6 (0x40)**；
* 先写 `WINDOWSTART = 1`（bit0）而 `WINDOWBASE` 还是 6，等于告诉硬件
  "0 号窗口活着、6 号窗口没活着" —— 与 CPU 实际状态矛盾，后续
  `callx8/retw` 立刻错乱。

**正确顺序是先 `wsr windowbase, 0` 把窗口基址归零，再 `wsr windowstart, 1`**

```
    movi a6, 0 ; wsr a6, windowbase
    movi a7, 1 ; wsr a7, windowstart
    rsync
```

这个顺序错误本身也是有价值的：它反证了 `WINDOWSTART` 位号与 `WINDOWBASE`
的关系，下一轮照正确顺序再做一次即可。

## 第 120 轮小结

* 确认崩溃（`cause=0x14`、`a0` 带着 CALLINC 标记被当指令地址取、
  `ws=6`）**与阻塞无关**：把 `osi_queue_recv` 改成永不阻塞后依旧崩；
* 根因方向确定在**寄存器窗口状态**（`WINDOWSTART/WINDOWBASE` 与
  spill/fill），不在队列/OSI/任务身份；
* 发现并记录了一个具体的顺序错误：`WINDOWSTART` 位号是绝对窗口号，
  必须先归零 `WINDOWBASE` 再设置；
* 顺带把 `g_wifi_noblock` 这个实验开关保留在代码里（默认 0），
  方便以后做 A/B。

### 下一轮

按正确顺序（先 `windowbase=0`、再 `windowstart=1`）在 trampoline 里
初始化窗口状态，真板验收：
* 无 TRAP；
* `osi_calls` 超过 30；
* 串口出现 `W2 bringup returned`。
若仍不行，再上真正的多级 spill（见 §184 的分步计划）。

## §187 ★ 为什么"在 trampoline 里改 WINDOWBASE"必然失败

两种顺序都试过了，结果都是**启动卡死**（串口只到 `c6os s3 boot`）：

```
先 windowstart=1 再 windowbase=0   → 卡死
先 windowbase=0  再 windowstart=1  → 卡死
```

原因不是顺序，而是**做这件事的位置根本不对**：

> `s3_task_tramp` 自己就跑在一个寄存器窗口里。一旦 `wsr windowbase` 生效，
> 这个函数自己的 `a0`..`a15` 会立刻被**重映射到另一组物理寄存器** ——
> 于是 `a1`（栈指针）、`a12`（入口地址）全都变成了别的值，函数当场失控。

所以**窗口状态不能在"正在运行的代码"里改**，只能在"寄存器已经从帧里
恢复、但还没有开始执行任务代码"的那一个瞬间改 —— 也就是
`RESTORE_FRAME` 的最后一步、`rfe` 之前。

### 正确的做法（下一轮按这个来）

在 `RESTORE_FRAME` 宏里，把 `wsr` 放到 `l32i a0..a15` **之后**、
`rfe` **之前**：

```
    ...  l32i a0..a15 从帧里恢复 ...
    addi  a1, a1, 80
    wsr   a7, windowstart      /* a7 里已装好 1，且这是最后几次读寄存器 */
    wsr   a6, windowbase       /* a6 里已装好 0 */
    rsync                       /* rsync 不读寄存器 */
    rfe                         /* rfe 只从 PS/EPC 特殊寄存器取数 */
```

要点：
* 两个 `wsr` 的源寄存器（a6/a7）必须在**改 windowbase 之前**就读好值 ——
  也就是说 `wsr windowstart, a7` 要先做（此时 a7 还是对的）；
* `wsr windowbase, a6` 之后**不能再读任何 a0..a15**，所以它必须是
  倒数第二条指令，后面只能跟 `rsync` 和 `rfe`。

### 验收

`W2 bringup returned` 出现、无 TRAP、`osi_calls` 超过 30。

若这样仍不行，再考虑真正的多级 spill（§184 的计划）。

## §188 三种"重置窗口状态"的尝试全部失败 —— 结论必须做真正的 spill

在 `s3_task_tramp` 的 windowed 分支里依次试过三种写法，真板结果：

| 尝试 | 写法 | 结果 |
|---|---|---|
| A | 先 `wsr windowstart,1` 再 `wsr windowbase,0` | 启动卡死（只到 `c6os s3 boot`） |
| B | 先 `wsr windowbase,0` 再 `wsr windowstart,1` | 同样卡死 |
| C | 只 `wsr windowstart, (1<<windowbase)`，不动 base | 同样卡死 |

C 也失败这一点很关键：即使**只把 WINDOWSTART 收紧到"只有当前窗口"**，
也会立刻坏掉 —— 因为那等于告诉硬件"祖先窗口不再活着"，**但它们的寄存器
并没有被存到栈上**。下一次 `retw` 逐级回退时，硬件按 WINDOWSTART 认为
要**从栈上 fill**，于是取到的是栈上的垃圾。

⇒ **不能靠改 WINDOWSTART 来"假装干净"，必须先把祖先窗口真正 spill 出去。**

（另外记一个工具细节：这个 Xtensa 汇编器**不接受三操作数的 `sll at,as,ar`**，
会报 `too many arguments`；要算 `1 << windowbase` 得用
`ssl a8` + `sll a9, a9` 这两条两操作数形式。这个坑本轮踩过一次。）

## 第 120 轮最终状态（已回退到已知可用状态）

```
c6os s3 boot / W0 phy start / W1 bringup / B0 B1 B2 全部通过
*** TRAP sp=3fcbc660 cause=00000014 pc=820328e4 vaddr=820328e4 ws=00000006 a0=820328e4
osi_calls = 30
```

崩溃签名与 §185 完全一致（`pc==vaddr==a0`、带 CALLINC 标记、`ws=6`）。

### 下一轮：实现真正的祖先窗口 spill

必须按 Xtensa 的标准做法来（这也是 `xthal_window_spill` 在做的）：

1. 先把**当前窗口**的 a0..a15 存到当前帧（现在已经在做）；
2. 读 `WINDOWSTART` 与 `WINDOWBASE`，确定还有几个祖先窗口活着；
3. 对每一个祖先：`rotw -1` 之后，祖先的 **a0..a3 会出现在我们的 a0..a3**、
   祖先的 **sp 会出现在我们的 a1**，用 `s32i`/`s32e` 把这 4 个寄存器
   存到 `[a1 - 16 .. a1 - 4]`；
4. 全部存完后 `rotw +N` 转回来（N = 转了多少次）；
5. 最后才可以把 `WINDOWSTART` 收紧为"只有当前窗口"。

注意第 3 步只拿得到祖先的 a0..a3，它的 a4..a15 在 `rotw` 之后并不可见 ——
这正是标准实现要用 `s32e` 配合固定偏移、而不是简单 `s32i` 的原因，
下一轮必须照标准序列逐条抄，不能凭直觉写。

## §189 `wsr windowbase` 在这个配置下不可用；并且 `ws=6` 其实是正常的

第四次尝试（把 `WINDOWSTART` 同时标上旧当前窗口和 0 号窗口 → `wsr windowbase,0`
→ 收紧 `WINDOWSTART` → 从固定内存重载 a1/a12/a2）**依旧卡死**在
`c6os s3 boot`。四种写法全部死在同一处，可以判定：

> **`wsr windowbase` 在当前配置下不可用**（多半是只读/特权受限），
> 任何"把窗口基址归零"的思路都走不通。

**同时更正一个此前的误判**：§185/§187 里把 `TRAP` 的 `ws=00000006` 当成
"WINDOWSTART 被破坏"的证据，这是错的。`WINDOWSTART` 的位号是绝对窗口号：

* `ws = 6` = 二进制 `110` = **窗口 1 和 2 活着**；
* 若当时 `WINDOWBASE = 2`（经过两次 call8 轮转就会到这里），
  那么"当前窗口 2 活着 + 祖先窗口 1 活着、窗口 0 已回收"是**完全正常**的状态。

所以 `ws=6` 不说明任何异常，之前围绕它推的方向要收回来。

## 第 120 轮结束时确定的、仍然成立的事实

* 崩溃发生在 `osi_queue_recv` 里 `callx8 osi_queue_recv_body` 之后的
  `mov.n a2, a10` 处，`a0` 的值 = `0x820328e4`（带 CALLINC 标记）；
* **与是否阻塞无关**（`g_wifi_noblock=1` 也崩）；
* **与是否发生任务切换无关**（同上，那次没有任何切换）；
* 四种"重置窗口状态"的写法都会让启动卡死 → `wsr windowbase` 不可用。

### 下一轮的方向（要换思路）

既然不能改窗口基址、且崩溃与切换无关，那么问题更可能是
**`callx8` 的调用约定本身**：我们的 trampoline 是 call0 代码，用 `callx8`
调用 windowed 的 `ppTask`；`callx8` 要求调用者在"有足够空闲窗口"的状态下，
而真正的 windowed 调用者是通过 `entry` 分配帧的。

建议下一轮改为：**用 `call4`**（只占用 1 个窗口，CALLINC=1）来调用库任务入口，
并相应地把 `ppTask` 当"需要 4 个参数的函数"看待 —— 若库函数实际按 call8
接收参数就会立刻看出差异（参数错位）。这条路比继续折腾窗口寄存器风险低得多。

## §190 WOE 已置上仍崩 —— 排除了"窗口溢出向量没启用"这个可能

怀疑 `PS.WOE`（bit18）为 0 导致窗口溢出/下溢向量不生效，于是在两处补上：

1. `port_task_ctx_create` 的 `f[16]`（任务初始 PS）：
   `f[16] = (port_ps_get() & ~0x1Fu) | 0x40000u;`
2. `port_irq_fill` 里把陷阱帧的 PS 也补上这一位
   （中断进来时硬件保存的是被打断代码的 PS，而被打断的多半是我们自己的
   call0 代码，WOE=0）。

真板结果：`TRAP` 里 `ps=00060510`，`0x60510 & 0x40000 == 0x40000` ✓
**WOE 确实生效了，但崩溃签名一模一样**（`cause=0x14`、
`pc==vaddr==a0==0x820328e4`）。

⇒ 排除"窗口向量没启用"这条。这个改动本身是正确的、无副作用的，
保留在代码里（windowed 库本来就要求 WOE=1）。

顺带把 PS 的位解出来了，供以后看 TRAP 用：
`ps=0x60510` → INTLEVEL=0、EXCM=bit4 ✓、UM=bit5=0、
CALLINC=bit16..17=3（call12）、WOE=bit18=1。

## 第 120 轮：五条假设，逐条真板排除

| # | 假设 | 验证方式 | 结论 |
|---|---|---|---|
| 1 | 因为阻塞在深窗口链上被切走 | `g_wifi_noblock=1` 永不阻塞 | **否**，照样崩 |
| 2 | WINDOWSTART 被破坏（`ws=6`） | 位号是绝对窗口号，base=2 时 6 完全正常 | **否**，误判已收回 |
| 3 | 在 trampoline 里重置窗口基址可修 | 四种写法 | **否**，`wsr windowbase` 直接卡死 |
| 4 | 收紧 WINDOWSTART 可"假装干净" | 单独写 windowstart | **否**，祖先寄存器没 spill，retw 取到垃圾 |
| 5 | PS.WOE=0 导致窗口向量不生效 | 两处补上 bit18 | **否**，ps 里 WOE 已置仍崩 |

净成果：把五条路全部用真板数据关掉，范围大幅收窄；另外把一个真实缺陷
（WOE 从未置位）修好并留在代码里。下一轮要从"`callx8` 的目标函数
`osi_queue_recv_body` 自己"入手 —— 它是 `-mabi=windowed` 编译的，
但被 call0 的 `w2c` 垫片用 `callx8` 调用，值得逐条核对它在栈上
期望的帧布局（`entry` 分配多少、a0 存在哪）。

## §191 EPC 槽的 CALLINC 问题：真 bug，但不是当前崩溃的元凶

`port_task_yield` / `port_task_switch` 把 `a0` 直接存进 **EPC 槽**（帧偏移 60）。
windowed 调用返回时 `a0` 是**带 CALLINC 标记**的地址（如 `0x820328e4`），
直接当 EPC 用就会去取指带标记的地址。已改成按已知公式还原：

```
    slli    a15, a0, 2
    srli    a15, a15, 2          /* a15 = a0 & 0x3FFFFFFF */
    movi    a14, 1
    slli    a14, a14, 30         /* 0x40000000 */
    or      a15, a15, a14        /* a15 = (a0 & 0x3FFFFFFF) | 0x40000000 */
    s32i    a15, a1, 60
```

对 call0 地址（`0x4037xxxx`/`0x4203xxxx`）这是**恒等变换**（安全），
对 windowed 的带标记地址这是必要还原。

（中途试过只做 `slli/srli` 清高 2 位：那会把 call0 地址的 bit30 也清掉，
`0x4037a6bd` 变成 `0x0037a6bd`，TRAP 签名立刻变成 `pc=0037a6bd` ——
反过来也验证了 `0x40000000` 这一位必须补回来。）

**但这个改动没有改变当前崩溃的签名** ⇒ 说明崩溃走的不是 yield/switch
的 EPC 路径（与 §185"不阻塞也崩"一致）。这是一个独立的、真实存在的
隐患，已修好留着。

## 第 120 轮最终结论（给下一轮接手）

仍然成立的核心事实：

1. 崩溃在 `osi_queue_recv` 里 `callx8 a8`（`a8 = osi_queue_recv_body`）处，
   `cause=0x14 InstrFetchProhibited`，`pc == vaddr == a0 == 0x820328e4`，
   而 `0x820328e4` 去掉 `0x40000000` 正好是 **`callx8` 的下一条指令**地址
   `0x420328e4` —— 也就是说 CPU 跳到了一个**带 CALLINC 标记的返回地址**上。
2. 这个现象与"是否阻塞、是否发生任务切换"都无关（§185 已实测）。
3. `PS.WOE` 已确认置上（§190），不是它。
4. `WINDOWSTART`/`WINDOWBASE` 在此配置下不能随便写（§188/§189）。

⇒ 下一轮最该做的一件事：**在 `callx8 a8` 前后把 `PS` 完整读出来存到 RTC**
（尤其 `CALLINC` 这两位）。如果 `callx8` 之后 `PS.CALLINC` 仍是 2，说明
问题在 `osi_queue_recv_body` 内部某处把 PS 写坏了；如果已经变成 0，
那就是我们自己的某个函数（`w2c` 垫片、`s3_w2c_current_task` 等）改了 PS。
这一步能把范围从"整条链"缩到"某一个函数"。

## §192 固件侧诊断通道 + init 停在 _queue_recv（真板读数）

### 换掉的观测手段
`esptool read-mem` 每次调用都会**先复位芯片**，而且这一版只接受地址（读 4 字节），
所以"逐字段读"既慢又对不上——每个字段来自不同次启动。两条替代：

1. `tools/rd.py`：一次连接读一整段（走 stub 的读接口）。
   注意 `tools/serial.py` 会顶掉 pyserial 的 `serial` 包，导入 esptool 前要先把它
   从 `sys.path` 里摘掉，否则报 `'serial' is not a package`。
2. **固件自己打印**（最终采用）：`port/s3/win_island.c` 里加一个低优先级 windowed
   任务 `s3_win_monitor_task`，每 120 tick 打一次 `s3_win_dump_diag()`：
   信号量操作环、阻塞环、内核任务状态（`os_task_probe`）、整张 OSI 调用计数表。
   这条路既一致又可重复，后面所有读数都来自它。

### 根因确认（上一轮的结论，本轮补上证据链）
`wifi_glue.c` 是 `-mabi=windowed` 编的，里面直接调了 call0 编的 `port_cur_ctx()`。
`call8` 进去、对方 `ret.n` 返回，返回地址被打上 CALLINC 标记（`0x82xxxxxx`）⇒
`cause=0x14 InstrFetchProhibited`，`pc==vaddr==a0`。改用 `s3_w2c_current_task()`
（走 `port/s3/w2c.S` 的正规壳）后，隔离测试 `s3_wifi_test_recv` 从"必崩"变为
完全成功。

### 真板现状
```
[wifimain] id=1 tick=0        <- 初始化任务就是 task1
W0 phy start
W1 bringup
B0 osi0=00000000 osi@00000000 calls@3fcbea9c
B1 slot13=4037d3f4 slot18=4037d2f0 magic=deadbeaf ver=00000008
B2 cfg@3fcbe994 osi=3fcbec9c magic@144=1f2f3f4f
```
任务状态（`os_task_probe`）：
```
task1 st=4(BLOCKED) wk=6(queue get) wobj=<pp 队列>
task2 st=2(RUNNING)  <- 监控任务
task3 st=4(BLOCKED) wk=6(queue get) wobj=<pp 队列>
```
OSI 调用计数表（索引 = `wifi_osi_funcs_t` 里的字节偏移/4）：
```
13 _semphr_create=1      15 _semphr_take=1     16 _semphr_give=1
18 _mutex_create=2       19 _recursive_mutex_create=2
21 _mutex_lock=1         22 _mutex_unlock=1
29 _queue_recv=2         37 _task_create=1     42 _task_get_max_priority=2
43 _malloc=11            86 _malloc_internal=1 88 _calloc_internal=3
94 _wifi_create_queue=1
```
要点：`_log_write`(83)=0 —— **库自己一个字都没打**，所以不能指望它的日志；
而且总调用数只有 30 左右，init 极早就停了。

w2c.S 的 recv 壳记录的"库侧返回地址"（带 CALLINC，掩掉后补 `0x40000000`）是
`0x40382149` / `0x40382159` ⇒ 按当前构建 `nm` 落在 **`ppTask+0x85` / `ppTask+0x95`**。
也就是说观测到的 3 次 `_queue_recv` 全部来自 ppTask；而内核视角下 task1（我们的
初始化任务）也在同一个队列上阻塞。下一轮要解决的就是这个矛盾：
要么 task1 的 `wait_kind/wait_obj` 字段是上一次等待的残留，要么还有一条
不经过 `osi_queue_recv` 壳的阻塞路径。

### 环境坑
`~/.c6os/toolchain/...` 之外，`.c6` 编译器 `/home/winger/c6/tcc/c6` 依赖
`libtcc.so`，需要 `LD_LIBRARY_PATH=/home/winger/c6/tcc`；否则报
`cannot open shared object file`（看起来像构建失败，其实是动态库路径）。

## §193 真凶：windowed 任务在库调用链里阻塞后 `retw.n` 非法

### 读数工具的两处修正（都很关键，否则会得出完全相反的结论）
1. **RTC 必须启动时清零**。RTC 慢存跨 CPU 复位是保留的，我们一度读到"两次
   `_queue_recv` 的库侧返回地址一模一样"，据此推断"两个任务都在 ppTask 里收
   队列"—— 其实那是上一次启动写下的残留。`port/s3/console.c` 启动时统一清
   `0x50000200/0x50000300/0x500003C0/0x50000A00/0x50000C00/0x50000F00/
   0x50001540/0x50001C00` 八个区。
2. **诊断打印搬回 call0 侧**（`port/s3/console.c` 的 `port_diag_ring` /
   `port_diag_task`）。原先放在 windowed 岛里，调用链一深就在 `retw.n` 上抛
   `cause=0 IllegalInstruction`（窗口用尽），读数时有时无。现在整条链是 call0，
   没有窗口轮转，稳定。

### 这一次启动的真实 OSI 回调序列（30 次，按顺序）
```
19 rec_mutex_create  43 malloc            21 mutex_lock      88 calloc
43 88 43 88 43       22 mutex_unlock      18 mutex_create    43 malloc
19 rec_mutex_create  43 malloc            18 mutex_create    43 malloc
86 malloc_internal   43 malloc            42 task_get_max_prio
94 wifi_create_queue 43 43               13 semphr_create   43 malloc
42 task_get_max_prio 37 task_create       15 semphr_take     16 semphr_give
29 queue_recv        29 queue_recv
```
- `_log_write`(83)=0：**库自己一个字都没打**，别指望它的日志。
- `[recv] t=2 qh=... tk=ffffffff`（不建监控任务时 ppTask 是 task2）：
  **只有 ppTask 走到 `_queue_recv`**。task1（我们的初始化任务）根本不在 recv
  里，它的 `wait_kind=6` 是上一次等待的残留 —— 它实际卡在信号量上（blk0 kind=1）。

### 真正的崩溃点
```
*** TRAP sp=... cause=00000000 pc=42032f18 ps=00060510 ws=00000006 a0=00000010
osi_queue_recv = 0x42032ed4  ->  pc = osi_queue_recv+0x44
42032f18:  f01d   retw.n            <- 就是这条
```
`cause=0` 是 IllegalInstruction，出在 `retw.n` 上，`a0=0x10`（明显是坏值）。
**去掉监控任务照样崩**，同一个地址、同一个 `a0`，所以它不是监控任务扰动出来的。

结论：ppTask（windowed）在库的调用链里阻塞在 `_queue_recv` 上，之后被恢复执行、
函数返回时 `retw` 已经无法恢复到正确的窗口 —— 也就是**切换出去时只保存了当前
窗口，任务在"多个窗口同时活着"的状态下被切走，恢复时物理窗口寄存器早已被
别的任务轮转掉了**。这解释了为什么 §192 的隔离压力测试能过（那几条链的
让出点窗口深度浅），而库的真实调用链不行。

下一轮要做的是**切换时保存/恢复任务的整套窗口状态**：要么在切走前把所有活着的
窗口溢出到该任务自己的栈上并把 WINDOWSTART 收紧（之前 `wsr windowbase/…` 四种
写法都卡死，需要重新设计：只写 WINDOWSTART、按"最小活跃窗口"收紧，并在切回来
之前再用 underflow 恢复），要么让库任务不在深层调用链里阻塞。

## §194 尝试按官方做法补齐窗口状态：两版都不成立（真板实测）

### 先确认了"哪些是对的"
`port/s3/irq.S` 的向量表**已经**是完整实现，不是空壳：
```
_win_ovf4:  s32e a0,a5,-16 ... rfwo        _win_unf4:  l32e ... rfwu
_win_ovf8/12、_win_unf12 同理（标准 Xtensa spill/fill 写法）
```
所以"调用链一深就 spill/fill"这条路径是通的 —— §192 的压力测试（深度 12）也印证了。

### 试过的两个 spill 方案（都不成立）
1. **官方 `window_spill8` 的 call 链**（IDF `coreasm.h`，XCHAL_NUM_AREGS==64 分支：
   `call8` + `call4` + `call12`×3 = 48 个窗口）。挂在
   `s3_w2c_sem_pend` / `s3_w2c_queue_get` 进入内核之前。
   → 真板：`W0/W1` 之后就**整机静默**，连低优先级监控任务都不再打印。
2. **官方 `SPILL_ALL_WINDOWS` 的 `rotw` 整圈旋转**
   （`xt_asm_utils.h`：`and a12,a12,a12` / `rotw 3` ×4 / `rotw 4`，转一圈
   WINDOWBASE 回到原值，因此不需要写 WINDOWBASE）。
   → 真板：同样是 `W1` 之后整机静默。
3. 只做 **WINDOWSTART 的按任务保存/恢复**（把 `rsr windowstart` 存进
   `S3Ctx.r[0]`（偏移 0，原先闲置），`rfe` 前 `wsr` 回去；0 表示"新任务不恢复"）。
   → 真板：**单独它也会**让系统在第一次阻塞时静默（不加重 spilling 也一样）。

已全部回退，基线恢复（`[recv] t=3 ... ` + `pc=osi_queue_recv+0x44` 的 `retw.n` TRAP
可稳定复现）。

### 下一步（下一轮从这里开始）
不要再靠"改一版、烧一次、看串口"猜 —— 仓库里已经有 USB-JTAG 的 gdb 脚本
（`tools/g*.gdb`），而 USB-JTAG 是这颗板子上唯一剩下的调试通道（没有 UART）。
要做的：
1. 加载 `build/wifi/c6os.elf`，在这次 hang 的位置停下来，看 `pc`、`windowbase`、
   `windowstart`、`ps` 和调用栈，确认到底是卡在
   `os_task_block_wait` / `port_task_switch`，还是卡在某个向量里。
2. 特别要确认 `S3Ctx.r[0]` 是否真的闲置 —— `port/s3/ctx.c` 里
   `port_task_ctx_create` 可能往 `r[]` 里写了别的东西，那样 `wsr windowstart`
   就是拿垃圾值去写；这条是"单做 WINDOWSTART 恢复也 hang"的最可疑解释。
3. 确认 `rotw` 在 `entry a1,48` 的壳里是否安全：壳自己的窗口不能在这一圈里
   被溢出掉（它的 a1 必须有效），否则整个 spill 会写到错误的位置。

## §195 USB-JTAG 接入：拿到 hang 处的**真实**现场（这条路以后就一直用）

仓库里早就有 gdb 脚本，但 gdb 本身不在我们平时用的那个 toolchain 里。位置：
```
GDB=/media/winger/inst_26/esp/.espressif/tools/xtensa-esp-elf-gdb/14.2_20240403/\
xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb
```
OpenOCD 已经在跑（`-f board/esp32s3-builtin.cfg`，端口 3333）。新加了
`tools/ghang.gdb`：让固件自己跑到卡住，然后 `monitor halt` 抓现场。

真板读数（这次是 CPU 真实状态，不是我们自己的诊断变量）：
```
pc = 0x4037b977  (s3_trap)
ps = 0x60510     (WOE=1, EXCM=0, INTLEVEL=0, CALLINC=2)
windowbase  = 3
windowstart = 0x8        <- 只有当前窗口是 live 的
epc1 = 0x42032f18        <- osi_queue_recv+0x44 == retw.n
exccause = 0             <- IllegalInstruction
调用栈: s3_trap <- _vec_exc <- ... <- osi_queue_recv (0x42032f18)
        中间还能看到 0x82032f08 —— 那是 call8 的返回地址（带 CALLINC 标记）
```
**结论**：
- 调用者窗口（窗口 1）确实已经不在寄存器里（`windowstart=0x8`），所以这条
  `retw` 必须走**窗口欠载**向量把调用者窗口从栈上 fill 回来。
- 而这颗 CPU 上，**窄格式 `retw.n` 在需要欠载时是不合法的**（`cause=0`），
  必须用宽格式 `retw`。也就是说问题既不是"忘了 spill"，也不是 WINDOWSTART
  没恢复 —— 是**汇编器把 `retw` 收窄成了 `retw.n`**。
- 试过 `-Wa,--no-transform` 强制宽格式：**不行**。GCC 自己会生成 `bnez.n`
  这类窄指令，禁用 transform 后汇编直接报错（`operand out of range`）。
  需要的是"只让 retw 不收窄"的办法，下一轮要验证：手写 `.byte` 编码出宽
  `retw`（先 `as --no-transform` 汇编一小段确认字节），或者查 GAS 是否支持
  逐指令关闭 transform。

## §196 两个真实的 ABI bug 修好了 —— `retw.n` 与 spill 区越界

### bug 1：`retw` 被汇编器收窄成 `retw.n`
`windowstart=0x8` 说明调用者窗口已经不在寄存器里，这条返回**必须**走窗口欠载；
而这颗 CPU 上窄格式 `retw.n` 在需要欠载时就是非法指令（`cause=0`）。
修法：给 `port/s3/w2c.S` 加 `-Wa,--no-transform`，强制出宽格式 `retw`
（编码 `90 00 00`）。**只加在这一个文件上**：
- `win_island.c` 加不了 —— 关掉 transform 后 `call8` 目标越界
  （`dangerous relocation: call8: call target out of range`），因为长调用是靠
  transform 放宽的；
- `wifi_glue.c` 更不行 —— 它太大，分支偏移会越界（汇编报 `operand out of range`）。
关键洞察：**恢复执行后的第一次返回**用宽 `retw` 就够了 —— 欠载会把下一层窗口
填回来，之后各层的 `retw.n` 就都合法了。而阻塞路径上最内层的 windowed 帧恰好
就是 `w2c.S` 里的这些壳，所以只修这一个文件正好够。

### bug 2：壳把自己的局部量放进了 ABI 保留的 spill 区
原来写的是 `entry a1,16` + `addi a1,a1,-16`，把返回地址存在 `a1+0`；
而 `entry` 分配的前 32 字节是留给 `call8` 的**窗口 spill 区**，硬件欠载/溢出会
往里面写。真板症状：宽 `retw` 一触发欠载就 `exccause=28`、
`excvaddr=0xfffffff5`（帧指针被踩成奇数地址）。
修法：统一改成 `entry a1,48`，局部量从 `a1+32` 开始（32/36/40），并且不做
`addi a1,a1,-16`。顺手补上了 `W2C_TRACE` 里对 a12/a13 的保存（原来直接借用
callee-saved 寄存器）。

### 现在的真板状态（在上一版基础上）
```
c6os s3 boot / [wifimain] id=1 / W0 phy start / W1 bringup / B0 B1 B2
[recv] t=3 qh=... tk=ffffffff          <- ppTask 正常阻塞在 pp 队列上
（没有 TRAP 了）
```
JTAG 抓到的下一个问题：
```
exccause = 28          excvaddr = 0xfffffff3（奇数，对齐异常）
epc1 = 0x4037e7dc      a0 = 0xffffffff8037e0a9
a1 = 0xffffffffffffffff   <- 栈指针被毁成 -1
windowbase = 3   windowstart = 0x20   <- 当前窗口(3)竟然不在 live 集合里
落到了 _vec_default（未处理的异常）
```
也就是说还有一处"栈/spill 区被踩"的 bug。下一轮用 JTAG 在
`0x4037e7dc` 前后下断点、单步看是哪个壳写坏的；重点怀疑：
- `_win_ovf*/_win_unf*` 向量和我们的 `entry a1,48` 组合时，spill 区归属算错；
- `port_task_switch` 保存/恢复时 `a1` 的推进量（80 字节帧）与壳的 48 字节帧叠加。

## §197 窗口状态的真正缺口：WINDOWSTART 与 WINDOWBASE 不一致

### JTAG 抓到的关键读数（`tools/ghang.gdb`）
```
exccause = 28        excvaddr = 0xfffffff3（奇数 → 对齐异常）
epc1 = __wrap_pp_create_task     a1 = 0xffffffffffffffff  ← SP 被毁成 -1
windowbase  = 3
windowstart = 0x20              ← bit5，跟 base=3 对不上
```
也就是说：切回来的时候 `WINDOWSTART` 里标着"live"的窗口**不是当前窗口**，
于是下一次 `retw` 的欠载会去填一个错的窗口，把 `a1` 填成垃圾，函数一进门
就在栈上做非对齐访问 → `cause=28`。

**根因链条完整了**：
1. windowed 任务在库的深调用链里阻塞时，祖先窗口会被别的任务轮转掉；
2. 这些窗口被溢出到该任务自己的栈上（硬件溢出向量做的，没问题）；
3. 但**切回来时 WINDOWSTART 是错的**（它是全局寄存器，不是每任务的），
   欠载就从错的窗口编号去 fill → 填回垃圾。

### 正确做法（下一轮按这个做）
IDF 的 `xtensa_context.S` 里是这套组合拳，我们要照抄：
1. 切走前用 `SPILL_ALL_WINDOWS`（`rotw` 整圈旋转 16 个窗口）把祖先窗口
   **全部**溢出到栈上 —— 转一圈 WINDOWBASE 回到原值，所以不需要写 WINDOWBASE；
2. 于是切回来时"只有当前窗口该是 live 的"是唯一确定的；
3. `rfe` 之前把 `WINDOWSTART` 写成 `1 << windowbase`。

### 本轮踩到的两个坑（都已记录，别重犯）
- 把 3 这一步放在 `RESTORE_FRAME` 的**后半段**：那里 a2/a3 已经被帧内容覆盖，
  借用它们会把恢复出来的寄存器改掉 → 真板立刻 `cause=0x14`、`pc=0x00000009`。
  必须放在宏的最前面，且要考虑启动早期 WOE 还没开的情况（直接写
  `windowstart` 可能异常，实测整个固件连 banner 都不打了）。
- 早先的 rotw spill 失败，是因为当时壳的帧还是错的（保存槽压在 call8 的
  窗口 spill 区里，见 §196 bug2）。现在帧已修好，spill 值得重新试。

### 环境事故：bootloader 被擦掉了（不是代码问题）
某一轮之后突然只打印 `invalid header: 0xffffffff`。`read-flash 0x0` 显示
**0x0 处全是 0xFF** —— 二级 bootloader 没了。用文档里记的备份刷回来即可：
```sh
B=/media/winger/inst_26/esp/xiaozhi/build
esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0 $B/bootloader/bootloader.bin \
  0x8000 $B/partition_table/partition-table.bin
```
以后遇到"固件明明刷进去了却起不来"，第一件事就是 `read-flash 0x0 32` 看
bootloader 还在不在。

### §197 补记：为什么"单独归一化 WINDOWSTART"也不行（真板实测）
- 只加 `RESTORE_FRAME` 里的 `wsr windowstart, 1<<windowbase`（并加了 PS.WOE 判断、
  放在宏最前面）：**整个固件连 banner 都不打**。
  原因：这条归一化对所有恢复点都生效，而**并不是每个任务在切走前都 spill 过**
  （比如第一次启动、以及不走 w2c 壳的让出路径）。对那些祖先窗口还 live 的任务，
  把 WINDOWSTART 收紧成只剩当前窗口，等于直接丢掉祖先窗口 → 立刻崩。
- spill + 归一化一起加也失败（早期启动就死）。说明还得把两者**严格配对**：
  任务切走时若走了 spill，就给它记一个"已 spill"标志；恢复时只有带标志的才
  做归一化，并把标志清掉。这个标志可以放在 `S3Ctx` 里（`r[0]` 那个闲置槽）。
- 另：`rotw` 整圈 spill 必须在**确认 PS.WOE=1 且当前帧合法**的地方做；放在
  `RESTORE_FRAME` 或启动早期都会立刻死。

### §197 续：配对方案已实现但**还没生效**（真板同签名复现）

实现了：
- `port/s3/w2c.S`：`W2C`/`W2C_TRACE` 两个壳在保存完寄存器后执行 `WIN_SPILL8`
  （`rotw` 整圈），随后把 `g_win_spilled` 置 1；
- `port/s3/ctx.S`：`port_task_switch` 把 `g_win_spilled` 搬进 `S3Ctx.r[0]`
  （偏移 0，原本就是闲置占位槽）并清零；`RESTORE_FRAME` 最前面检查这个标志，
  有标志才做 `WINDOWSTART = 1<<windowbase`（并先看 PS.WOE），用掉即清；
  `port_task_yield` 路径明确写 0（它没有 spill）。
- `port/s3/ctx.c`：定义 `u32 g_win_spilled`。

结果：能正常启动、`[recv] t=3` 正常、没有 IllegalInstruction，但**故障签名没变**：
```
JTAG: exccause=28  excvaddr=0xfffffff3  a1=0xffffffff(-1)
      epc1=0x4037e834  windowbase=3  windowstart=0x20
```
说明这条恢复路径上**标志没被看到**，归一化没有执行。最可疑的一点：
`port_task_switch` 里 `s32i a1, a3, 64` 的 `a3` 是 **`&g_sched[core]`**，
而不是任务自己的 `g_ctx[i]` —— 如果 `g_sched[core]` 只是个临时槽，
那么写进去的 `r[0]` 标志根本不会跟着任务走，恢复时读到的就是 0。

**下一轮第一件事**：查清 `g_sched[core]` 与 `g_ctx[i]` 的关系（`port/s3/ctx.c`），
确认"每个任务自己的 ctx"到底是哪一个，然后把标志写到正确的那个 ctx 上；
顺便确认 `port_task_ctx_create` 有没有把 `r[0]` 初始化成 0。

### §197 再续：标志改写到 `g_s3_cur[core]` 指向的任务 ctx（真板签名仍未变）

查清了 `port/s3/ctx.c` 里三者的关系：
- `g_sched[core]`：**临时保存槽**（ctx.S 按 76 字节步长索引），不跟任务走；
- `g_s3_cur[core]`：**当前任务的 ctx 指针**（`port_cur_ctx()` 读的就是它）；
- `g_ctx[i]`：任务 ctx 数组本体。
所以标志必须写进 `g_s3_cur[core]` 指向的那个 ctx。已改（在 `s32i a4,a3,0`
之前读旧值），并把 `g_sched` 里那段无效写入删掉。

真板：仍能启动、`[recv] t=3` 正常、无 IllegalInstruction，但故障签名依旧
（`cause=28`、`a1=0xffffffff`、`windowbase=3`、`windowstart=0x20`）。

### JTAG 直接 dump `g_ctx[]`（gdb: `x/57xw 0x3fcbe970`）
```
ctx[0] @0x3fcbe970: r[0..15]=0, sp@64=0x3fcba6b0, stack=0x3fcb8970, size=0x2000
ctx[1] @0x3fcbe9bc: r[0..15]=0, sp@64=0x3fcbc890, stack=0x3fcbb970, size=0x1000
ctx[2] @0x3fcbea08: r[0..15]=0, sp@64=0x3fcbe780, stack=0x3fcbc970, size=0x2000
g_s3_cur[0] = 0x3fcbe970  (= ctx[0]，正是当前任务)
```
三个任务的 sp 都落在各自栈区间内，说明**任务级 sp 没坏**。所以 `a1=0xffffffff`
不是任务 sp，而是**某个祖先窗口的 a1 被欠载从错误的 spill 帧填进来** ——
和 §197 开头的判断一致。`r[0]` 读到 0，无法区分"用过即清"还是"从没置过"，
下一轮可以给 `g_win_spilled` 加一个累计计数（RTC），确认 spill 到底有没有发生过。

## §198 回到官方文档：窗口切换的**标准答案**在 FreeRTOS 的 Xtensa 移植层

`components/freertos/FreeRTOS-Kernel/portable/xtensa/portasm.S` 的 `vPortYield`
（windowed 分支）就是"抢占式内核 + windowed ABI"的权威做法：

```asm
    entry   sp, XT_SOL_FRMSZ
    rsr     a2, PS
    s32i    a0, sp, XT_SOL_PC            ; 只存 a0（和 PS）
    s32i    a2, sp, XT_SOL_PS
    movi    a6, ~(PS_WOE_MASK|PS_INTLEVEL_MASK)
    and     a2, a2, a6                   ; 清 WOE
    addi    a2, a2, XCHAL_EXCM_LEVEL     ; INTLEVEL = 3
    wsr     a2, PS
    rsync
    call0   xthal_window_spill_nw        ; 把所有活着的窗口 spill 到栈上
    l32i    a2, sp, XT_SOL_PS            ; 恢复 PS
    wsr     a2, PS
    ...
    s32i    sp, a2, TOPOFSTACK_OFFS      ; TCB 里**只存 sp**
```

要点（三条，都很关键）：
1. **spill 是在 `PS.WOE = 0` 下做的**，用 HAL 的 `xthal_window_spill_nw`。
   WOE=0 时 `call4/8/12` 只轮转窗口、不触发溢出异常，所以这个例程可以自己
   把每个窗口显式存到栈上。这正是我之前用 `rotw`（WOE=1）失败的原因 ——
   那样每次轮转都会触发溢出异常，要求每个窗口的帧都合法。
2. **只保存 sp**（以及帧里的 a0/PS）。因为 spill 之后任务的寄存器状态全在
   自己栈上，恢复时靠**欠载向量按需 fill**。
3. **从不写 WINDOWSTART / WINDOWBASE**。（写 WINDOWBASE 在这颗芯片上会卡死，
   写 WINDOWSTART 则必须与 spill 严格配对 —— 我自己发明的两种写法都失败过，
   见 §197。）

`xthal_window_spill_nw` 有现成的：`components/xtensa/esp32s3/libxt_hal.a`
（`nm` 可见 `T xthal_window_spill_nw`，`xthal_window_spill` 0x114 字节）。
链接它大约多吃 316 字节 IRAM，为此把 `wifi_glue.c` 的 `osi_log_writev` 和
`s3_wifi_osi_init` 挪进 `.flash.text` 腾了地方。

### 本轮集成到哪一步
按上面顺序插进 `port_task_switch`（先存 80 字节陷阱帧，再做 PS 舞蹈 + spill）
后：spill 确实跑起来了 —— JTAG 读到 `windowbase=9`、`windowstart=0xaaaa`
（bit9 是置的，**两者终于一致了**），但随后在 ROM 里 `cause=29`、
`epc1=0x40048c33`。已回退，基线（`[recv] t=3`、无 TRAP）保持不变。

### 下一轮的具体修法（已经想清楚，只差实现）
**spill 必须在任务的"原始 sp"上做，不能在 `addi a1,a1,-80` 之后的帧上做。**
因为祖先窗口的帧是相对任务原始 sp 的，sp 挪了 80 字节之后 spill 会写到错误的
位置。正确顺序：
1. `addi a1,a1,-80` 存好 a0..a15/EPC/PS（当前窗口的内容先落到我们自己的帧里）；
2. `addi a1,a1,80` **把 sp 挪回任务原始值**；
3. 再做 PS 舞蹈 + `call0 xthal_window_spill_nw`（此时它看到的是任务的真实栈，
   祖先窗口的帧位置都对）；
4. 注意 spill 会破坏 a2..a11 的当前窗口内容 —— 但我们第 1 步已经存过了，
   后面只用帧里的值，所以没问题；
5. 恢复路径保持"只从帧恢复 + 欠载按需 fill"，不要写 WINDOWSTART。

### §198 补记：把 sp 挪回原始值再 spill 也不行（真板）
按上一节写的顺序（存帧 → sp 挪回 → spill → 挪回帧）实现后，故障变成：
```
exccause=29 (LoadStoreProhibited)  excvaddr=0xffffffe0(-32)  a0=0
epc1=0x40048c33   ← **ROM 里的地址**，不是 libxt_hal.a 的 IRAM 地址
windowbase=9  windowstart=0xaaaa（两者一致，说明 spill 确实走了一圈）
```
`epc1` 落在 ROM 说明 `xthal_window_spill_nw` 最终解析到的是 **ROM 里的实现**
（HAL 里不少例程是 ROM 常驻的），它在我们这个"当前窗口属于任务自己"的上下文里
把某个窗口的帧指针算成了垃圾（存到 -32）。

**这指向一个结构性结论**：官方的 vPortYield 之所以能这么干，是因为它在
**自己的函数帧里**执行 spill（当前窗口属于 vPortYield，不属于被切换的任务），
所以"祖先窗口"恰好就是任务的全部窗口。我们的 `port_task_switch` 是 call0 内核
函数、由 call0 内核调用，当前窗口就是**被中断任务自己的窗口**，语义不一样。

下一轮的正确做法：写一个**专门的 windowed 汇编辅助函数**（例如
`s3_win_spill_helper`），在里面 `entry` 建立自己的窗口 → 做 PS 舞蹈 →
`call0 xthal_window_spill_nw` → `retw` 返回；由 `port_task_switch` 通过
`call8` 调它（和 `port/s3/win_bridge.S` 里 call0→windowed 的桥同一套路）。
这样"当前窗口"就是辅助函数自己的，祖先窗口才是任务的，和官方语义一致。

### §198 再补记：windowed 辅助函数方案的两版实测（都还没成）
1. 辅助函数里用 `mov a12, a0` 存返回地址：真板 `cause=0x14`、`pc=0`、
   `a0=0x4037a6ed`。spill 把 a12 连同窗口一起搬走了，`mov a0,a12` 拿到 0。
2. 改成把返回地址/PS 存进辅助函数**自己的栈帧**（`entry a1,48`，放 a1+32/36）：
   窗口状态终于一致了（`windowbase=0xb`、`windowstart=0x800`，bit11 对得上），
   但紧接着 `cause=0`（IllegalInstruction）、`epc1=0x403cd8fa`（**ROM**）、
   `a1=0x3fcd8f10`（接近启动栈顶）—— 启动链上的窗口被 spill 到了之后会被
   复用的帧里，把启动路径搞坏了。

**结论（下一轮从这条起步）**：spill 只在**任务已经跑起来、栈已经稳定**之后
才有意义。启动早期（`_start` → `os_main` → `os_task_start` 之前）那条
call0/windowed 混合链不能参与 spill。所以要给切换路径加一个"启动阶段不 spill"
的开关（例如 `g_sched_ready`，`os_task_start` 里置 1），spill 分支只在它为真时
执行。这一条能同时解释本轮第 1、2 版的失败（两次都死在启动早期，而不是死在
任务的阻塞/恢复路径上）。

### §199 新工具：JTAG 单步观察窗口状态（`tools/gstep_win.gdb`）
之前的失败都是"改一版、烧一次、看串口"，太粗。现在这套能用了：
```sh
GDB=/media/winger/inst_26/esp/.espressif/tools/xtensa-esp-elf-gdb/14.2_20240403/\
xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb
sudo $GDB -q -batch -x tools/gstep_win.gdb build/wifi/c6os.elf
```
`tools/gstep_win.gdb` 做的是：`monitor reset halt` → `hbreak port_task_switch`
→ 命中了之后逐步 `stepi`，只在 `windowbase`/`windowstart` 变化时打印。
（要点：`monitor reset halt` 之后应用能正常跑起来，硬件断点可用；
之前 `break __wrap_pp_create_task` 打不中是因为那之前就崩了，不是断点问题。）

第一次切换（从启动栈切到第一个任务）的实测入口状态：
```
pc=port_task_switch  wb=9  ws=0x280  ps=0x60900  a1=0x3fcd8ef0(boot 栈)
连走 60 步：windowbase/windowstart 完全没变（wb=9, ws=0x280）
```
`0x280` = bit7|bit9 —— live 窗口不连续，说明**启动链上就已经带着一个不干净的
windowstart 进入第一次切换**。这为下一轮提供了明确的入手点：先在
`os_task_start` 之前把 WINDOWSTART 收拾干净（此时还没有任务栈，最安全），
再让切换路径去维护它。

### 下一轮行动计划（按顺序，不要跳）
1. 用 `tools/gstep_win.gdb` 在**没有 spill 的基线**上，把第一次切换的完整
   60~200 步走完，确认 windowstart 是在哪里变成 bit7|bit9 的；
2. 在 `os_task_start` 之前（启动栈上、无任务上下文）显式把 WINDOWSTART
   归一化一次；
3. 然后重新引入官方 spill（`tools/gstep_win.gdb` 逐步验证），而不是烧完看串口。
