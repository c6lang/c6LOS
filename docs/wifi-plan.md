

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

## §200 单步结论：spill 会改变 WINDOWBASE，所以它必须是"切换前最后一件事"

用 `tools/gstep_win.gdb` 单步（跳过第一次引导切换，停在真正执行 spill 的那次），
窗口状态序列如下：
```
第1次 port_task_switch（引导）: wb=9  ws=0x280        ← 门控生效，没 spill
第2次 port_task_switch:
  step  16  pc=0x4203307f  wb  9->11  ws 0x280->0xa80   ← call8 进辅助函数
  step  41  pc=0x4037e3dc  wb 11      ws 0xa80->0xa     ← HAL spill 开始动窗口
  step  45  pc=0x4037e3e8  wb 11->6                     ← **WINDOWBASE 被改**
  step  64  pc=0x4037e432  wb  6->8
  step  82  pc=0x4037e432  wb  8->10
  step  85  pc=0x4037e46f  wb 10->11
  step  90  pc=0x4037e47d  wb 11      ws 0xa->0x800    ← 结束在 base=11 且一致
末态 pc=0x4037bc77（s3_trap）  wb=11  ws=0x800
```
**两个关键事实**：
1. spill 结束时窗口状态是**自洽**的（`wb=11`、`ws=0x800` = bit11）—— 说明官方
   例程本身工作正常，我们调用的方式没问题。
2. 但 **`WINDOWBASE` 从 9 变成了 11**（`xthal_window_spill_nw` 靠一圈轮转实现，
   官方例程干完就 `s32i sp, TCB` 直接切走，**从不返回**到发起调用的那条链上）。
   我们却继续在 `port_task_switch` 里用 a1/a4 做后续计算 —— 此时这些寄存器
   已经属于**另一个窗口**，读到的是垃圾，于是撞进 s3_trap。

### 下一轮的实现方案（已定，照做即可）
spill 必须是切换前的最后一件事，之后的交接**只能通过内存**（内存访问与窗口无关）：
1. 存 80 字节陷阱帧（当前窗口内容落栈）；
2. 照旧做 ctx 记账，算好"目标任务的帧指针"和"目标 ctx"，**写进两个全局**
   （例如 `g_sw_next_sp` / `g_sw_next_ctx`）；
3. `a1` 挪回本任务的原始 sp，调用 windowed 辅助函数做 spill；
4. 之后**不再用任何寄存器里的旧值**：让 `RESTORE_FRAME` 一开头就从那两个全局
   把 `a1`/`a4` 重新 `l32r` + `l32i` 取回来（它本来就用 a1/a4）；
5. 继续 `rfe`。
这样"任务切走时祖先窗口全在栈上、恢复时靠欠载按需 fill"，就完全对齐官方语义了。

### §200 补记：内存交接版也失败了 —— 但失败点已经"物理上"明确

实现（交接信息落内存、spill 放最后、用 `l32r` 取回目标帧/ctx）后单步读数：
```
第2次 port_task_switch:
  step  58  wb  9->11  ws 0x280->0xa80    call8 进辅助函数
  step  83  wb 11      ws 0xa80->0xa      HAL spill 开始
  step  87  wb 11->6                      一圈轮转
  step 106  wb  6->8
  step 124  wb  8->10
  step 127  wb 10->11
  step 132  wb 11      ws 0xa->0x800      结束时只剩当前窗口(11) live
末态  pc=s3_trap  wb=11  ws=0x800
```
内存交接本身是有效的（后面不再用到旧寄存器），问题在**更底层**：

`xthal_window_spill_nw` 的设计是"溢出**调用者的调用者**的窗口，然后**不返回**"。
它结束时只有当前窗口 live（`ws=0x800`）。可是辅助函数还要 `retw` 回到
`port_task_switch` —— 那个返回目标在窗口 9，而窗口 9 既不 live、也**没有被
spill 出来**（`_nw` 只 spill 调用者的调用者），于是 `retw` 无法恢复，直接崩。

官方之所以没问题，是因为 `vPortYield` **spill 完就把 sp 存进 TCB 直接切走，
永远不 retw 回到发起调用的那条链**。我们的 `port_task_switch` 是"保存现场 +
切到目标任务"的普通函数，结构上和它不同。

**下一轮的正确结构**（这是最后一块拼图）：
把 `port_task_switch` 改成"发起调用的那条链永不被恢复"的形态 —— 即
spill 之后**不要 `retw` 回 port_task_switch**，而是直接从辅助函数内部完成
"写出 sp、装载目标任务帧、`rfe`"。也就是把整个切换动作搬到 windowed 侧：
```
s3_win_switch_helper:            (windowed)
    entry a1, 48
    <保存当前帧到任务 ctx —— 或者直接调用 call0 的保存例程>
    <PS 舞蹈 + xthal_window_spill_nw>
    <从内存取目标任务帧 -> a1, 恢复 a0..a15/EPC/PS -> rfe>
    （永不 retw）
```
这样"spill 后不返回"的语义与官方完全一致，也就不需要对抗 WINDOWBASE 了。

### §200 三补记：「spill 完直接 rfe、永不返回」版也挂在 ROM 例程内部
按上一节的正确结构实现（`s3_win_switch`：PS 舞蹈 → `xthal_window_spill_nw`
→ `jx` 到 `s3_restore_from_globals` 恢复目标任务并 `rfe`，永不 retw）后：
- **第一次切换（引导，门控不 spill）完全正常**：单步 400 步窗口状态不变、
  能正常 `rfe` 进第一个任务 ✓
- 第二次切换（真正 spill）挂在**ROM 的 spill 例程内部**：
  ```
  exccause=29 (LoadStoreProhibited)  excvaddr=0xffffffe0(-32)
  epc1=0x40048c33 (ROM)   a1=0x3fcba980(任务栈)
  windowbase=0xb  windowstart=0xaaaa   ← spill 进行到一半
  ```
  也就是说轮到某个窗口时，它算出的帧指针是坏的（往 -32 存）。

到这里可以确定：**问题不在"怎么调"，而在"我们这条任务栈上的窗口链本身不干净"**
（第一次切换前就已经观察到 `ws=0x280` = bit7|bit9 这种不连续的 live 集合）。
所以下一步不该再改切换代码，而应该**从源头保证任务栈上的窗口状态是干净的**：

1. 任务第一次被调度时，`port_task_ctx_create` 建的那个初始帧必须显式把
   `WINDOWSTART` 归成"只有当前窗口"（`wsr windowstart`，此时 WOE 是否已开
   要按 §197 的办法判断），让任务从一个干净的窗口状态起步；
2. 之后每次切换前 spill 才有意义（因为链上每个窗口都是本任务产生的）。
3. 验证手段：用 `tools/gstep_win.gdb` 在任务第一次运行前看 `ws` 是不是只有一个 bit。

这条是整条线上唯一还没做过的"源头清理"，也是三次切换改造都失败后剩下的
唯一解释。

## §201 代码级核对：前几轮的 spill 其实**一次都没执行过**；本轮把三处缺口补齐

这一轮没有再动真板，而是把 `port/` 里与窗口有关的代码和官方的对照读了一遍。
结论很硬：**§197~§200 三轮"方案不成立"的读数，全部建立在"spill 生效了"这个
错误前提上** —— 那三版代码在编出来的镜像里根本不存在。

### 三个决定性事实（都能用 objdump/nm 复核）

1. `port/s3/ctx.S` 里 `port_task_switch` / `port_task_yield` 开头那两段
   `call8 xthal_window_spill`，外面包着 `#ifndef __XTENSA_CALL0_ABI__`，
   而 Makefile 用 `-mabi=$(S3_ABI)` 编它、`S3_ABI ?= call0`（Makefile:12、
   218-220）。**所以这两段从来没被编进去过。** 同理 `RESTORE_FRAME` 末尾那个
   "windowed 才归一化 WINDOWSTART"的 `#else` 分支也是死的。
2. `port/s3/w2c.S` 里 `WIN_SPILL8`（正好就是官方的 rotw 整圈）**只定义、没人调**；
   而 `g_win_spilled = 1` 只写在它里面。于是 `ctx.S` 里那段"按标志做
   WINDOWSTART 归一化"的代码永远读到 0 —— 这解释了 §197 续那句
   "故障签名没变，说明归一化没执行"：不是归一化写错了，是它压根没被调用。
3. 真正跑在板上的那三版实验，用的是 `call8 xthal_window_spill`（ROM 的 HAL
   版本）。它的契约是"溢出**调用者的调用者**的窗口，然后**不返回**"，和
   `xt_asm_utils.h` 的 `SPILL_ALL_WINDOWS` 是两个东西 —— 前者靠内部轮转改变
   WINDOWBASE，后者转 16 个窗口正好一圈、**WINDOWBASE 回到原值**。
   所以 §200 推出的"spill 必然改 WINDOWBASE，所以必须是切换前最后一件事、
   之后的交接只能走内存、spill 完不能 retw"这一整套约束，是 HAL 版本的性质，
   不是窗口机制的性质。§197 里我们自己写的那句"转一圈 WINDOWBASE 回到原值，
   所以不需要写 WINDOWBASE"才是对的，后面三轮把它推翻了。

### 官方 `_xt_context_save` 里还有三件我们缺的事

`xt_asm_utils.h:51` 那个宏只是裸的 5 个 `rotw`。官方在它前后还做了：

1. **存/恢复 EPC1**：窗口溢出是 level-1 异常，向量末尾的 `rfwo` 会消费 EPC1；
2. **PS 舞蹈**：`PS = (PS & ~(INTLEVEL|EXCM)) | UM|WOE`，且 INTLEVEL 抬到
   `max(当前, XCHAL_EXCM_LEVEL=3)`。官方注释写得很直白：WOE=0 时整段 rotw 是
   "**a silent noop**"（静默空转）。我们先前两次"整圈 spill 集成"没做这一步，
   在 WOE 关掉的上下文里就等于什么都没干；
3. **归一化 WINDOWSTART**：转完只有当前窗口该是 live 的，必须显式写成
   `1 << windowbase`，否则就是 §197 那个签名（`windowbase=3 / windowstart=0x20`）。

### spill 该放在哪：不能放 `port_task_switch`，只能放 w2c 的壳里

这是本轮推出来的关键结构结论（比"拷官方代码"更重要）：

* 溢出槽位的地址 = **被 spill 窗口的调用者的栈指针减 16**（`_win_ovf4` 的
  `s32e a0, a5, -16`，a5 = callee 的 a1；`xtensa_vectors.S:2017` 与我们
  `port/s3/irq.S:52-125` 逐字节一致，这组向量没问题）。
  也就是说槽位落在"被 spill 的那个窗口的**被调方**的帧顶部"。
* `port_task_switch` 跑在我们自己的 **call0 内核栈**上（`kernel/task.c6:621`
  的主循环、以及 `port_task_yield`）。c6 生成的 call0 帧把 a0 存在 `[sp+0]`，
  根本没有 Xtensa ABI 那 16 字节保留区 —— 在那里 spill 就是拿窗口内容去踩
  内核自己的局部量。§197 猜的"`_win_ovf*/_win_unf*` 与 `entry a1,48` 组合时
  spill 区归属算错"、"spill 区越界"，就是这个。
* w2c 壳（`port/s3/w2c.S`）正好在另一头：它刚 `entry` 完、**还没有向下调用
  任何东西**，所以帧下方的 64 字节全是空闲栈，槽位天然落在自由区。祖先窗口
  则是闭源库那条链（gcc 编的 windowed 帧，保留区是规范做法）。

所以本轮的顺序是固定的：**先 spill，再把 sp 降 64 留死区，然后才进内核**。
反过来（先降 sp 再 spill）槽位就落进内核帧里了。

### 本轮改了四处

| 文件 | 改动 |
|---|---|
| `port/s3/w2c.S` | 新增 `WIN_SPILL` 宏（PS 舞蹈 + 整圈 rotw + EPC1 + 归一化，全部用 a8..a11，不碰 a4..a7/a12..a15 因为这些是 callee-saved 和 retw 的返回地址）；`W2C`/`W2C_TRACE`/`osi_queue_recv` 三个壳统一改成 `entry a1,128` + 局部量从 +16 起 + 64B 死区；删掉死掉的 `WIN_SPILL8` |
| `port/s3/ctx.S` | 删掉两处 `call8 xthal_window_spill`（反正没编进去）和 `g_win_spilled` → `ctx->r[0]` 的配对记账；`RESTORE_FRAME` 改成**无条件**（只看 PS.WOE）做 `WINDOWSTART = 1<<windowbase`，位置在恢复 a0..a15 之前 |
| `port/s3/ctx.c` | `g_win_spilled` → `g_s3_win_spill_en`，语义从"每任务一次性标志"变成**总开关**，默认 0 |
| `port/s3/wifi_glue.c` | `s3_wifi_osi_init()` 里打开开关；补 `net80211_printf`（闭源 .a 要、IDF 侧由开源组件提供、我们没人提供） |

为什么不再需要 per-task 配对标志：只要**每条** w2c 壳进门都 spill 一次，
"栈上有全部祖先窗口 + 寄存器文件里只有当前窗口"就是恒真状态，恢复时让欠载
向量按 a1 链 fill 回来即可 —— 这正是官方 FreeRTOS 移植层的语义。默认关掉是
为了保住已经跑通的 PHY/串口基线：只有闭源库那几条链需要窗口语义。

### 构建侧的真实状态（本轮踩到的环境缺口，别再浪费时间）

* HEAD 上 `make s3` **本来就链不过**（`w2c.o` 的 `os_queue_*` 与
  `wifi_glue.o` 的 `__real_*`）：默认 `S3_SKIP_C6` 把 `queue/sem` 排掉了。
  可用命令：
  ```sh
  make C6_ROOT=/home/winger/c6 BUILD=build/base S3_SKIP_C6="" \
    S3_EXTRA_C6="kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 \
                 kernel/sem.c6 kernel/mutex.c6 kernel/queue.c6 kernel/event.c6" \
    S3_EXTRA_LIBS="/tmp/wstub.o" s3        # /tmp/wstub.o 只补 __real_*/g_log_level/esp_wifi_start
  ```
  这样能出 elf（本轮用它验证了汇编与链接，`text 56021`）。
* 真 WiFi 镜像还差的**不是 ABI，是 IDF 开源层**：把
  `~/.c6os/toolchain` 之外那批原始 `.a` 直接拿来链
  （`esp_wifi/lib/esp32s3/lib{net80211,pp,core,phy,mesh,wapi,espnow,smartconfig}.a`
  + `objcopy --redefine-sym memcpy=win_memcpy ...`），现在缺的是
  `WIFI_EVENT`、`regdomain_table`、`regulatory_data`、`pp_printf`、
  `esp_wifi_disconnect`、`esp_mesh_send_event_internal` —— 这些在 IDF 的
  **开源**组件里（`components/esp_wifi/src/*.c`、`esp_wifi_regdomain.c` 等）。
  下一轮要么把这些编译进来，要么按需补桩。

### 下一轮（真板）判据，按顺序

1. 镜像能链接 + 烧进去后，先用 `tools/gstep_win.gdb` 在**第一次**进
   `port_task_switch` 时看 `ws`：应该是 `1<<wb`（不再是 §199 那个 `0x280`）。
2. 再 `hbreak osi_queue_recv`，命中断点后单步看**整圈 rotw 之后**
   `wb` 是否回到原值、`ws == 1<<wb`、`a1` 是否仍等于进壳时的帧基址。
3. 然后看 ppTask 阻塞→唤醒→`retw` 这一趟：`retw` 必须因欠载而 fill（说明
   祖先窗口真的在栈上），而不是直接跳走。
4. 串口判据：`g_osi_calls` 总数越过 §192 的 30 次、且 `_log_write` 仍然为 0
   但 `_queue_recv` 不再只在 ppTask 那一条路上出现（说明 init 走得更远）。

## §202 根因找到：flash 段的 LMA ≠ 镜像文件偏移，ROM 就按 LMA 建 MMU 映射

这一节是本项目"从 flash 执行必炸"的**终结**。前面 §15~§201 里所有
`IllegalInstruction`、`PC = 函数首地址 + 2`、`cause=28/29`、`pc = 0x00000001`
之类的怪签名，全是同一个原因：**我们让 `0x42xxxxxx` 指向了错误的 flash 字节**。

### 证据链（一次串起来就能对上）

1. 真板读到 `MMU[0x300] = 3`（= flash 0x30000），而 `objdump` 里
   `.flash.text` 的 VMA 也是 `0x42030000`、`_flash_text_lma = 0x30000` ——
   看起来"完全一致"，但：
2. 在 `build/wifi/c6os.bin` 里按 `ELF` 的头几个字节搜
   （`c6 01 00 00 92 42 00 1b`），**实际文件偏移是 0x10000**，
   `esptool image-info` 也写着 `Segment 2: Load addr 0x42030000, File offs 0x0000fff8`
   （+8 就是数据起点 = 0x10000）。
3. 于是 `0x42030bd0`（进程 PC）命中的 flash 字节是
   `ad 01 0c 02 29 11 …`，而 ELF 里那里应该是 `c6 01 00 00 92 42 …` ——
   完全不同的代码。GDB `x/i` 和 `objdump -d` 对同一地址给出不同指令，
   就是这个原因（GDB 读的是**目标机实际的 flash 内容**，objdump 读的是 ELF）。
4. 进一步：`JP` 到 `0x42030bd0 + 0xbd0` 那一带（LMA 页 3 之后）拿到的字节
   与 ELF 相差整整 `0x20000`（`0x30000 - 0x10000`）。

### 谁在用 LMA？

我们**没有二级 bootloader**（镜像直接由 ROM 引导，日志里
`load:0x3c000020 / load:0x3fca0000 / load:0x42030000 / …` 是 ROM 打的）。
ROM 给 flash 段（DROM/IROM）建 MMU 映射时，用的是**段头里的 load addr**
（也就是链接脚本给这段的 LMA）>> 16 当物理页号。而 esptool 排布镜像文件时
对 flash 段自己按 0x10000 对齐（从第 3 段开始就落在文件偏移 0x10000）。
**两者必须相等**，否则映射到的是别的字节。

IDF 之所以没这个问题：它的 `.flash.text`/IROM 段固定 `VMA = 0x42010000`、
`LMA = 0x10000`，正好和文件偏移撞上（前面 DROM 0x10c + DRAM 0xfec4 之后
刚好对齐到 0x10000）。

### 修法

| 文件 | 改动 |
| --- | --- |
| `port/s3/link.ld` | `.flash.text` 从 `0x42030000 : AT(0x30000)` 改成 **`0x42010000 : AT(0x10000)`**（VMA/LMA 必须 mod 0x10000 同余，MMU 页是 64KB） |
| `tools/chkimg.py` | **新增**：构建后校验"每个 flash 段（VMA 落在 DROM 0x3C000000..0x3E000000 / IROM 0x42000000..0x44000000）的字节，必须原封不动出现在 `.bin` 的 LMA 偏移处"。`Makefile` 的 `$(BUILD)/c6os.bin` 规则里调用它，不一致直接**构建失败** |
| `tools/flashmon.sh` | 新增可选 `SHADOW_BIN` / `SHADOW_OFF`：把一段原始字节额外补写到 flash 指定偏移（备用的兜底手段；`esptool` 拒绝同一次 write-flash 区间重叠，所以分开写两次，并在每次写之前重新探测 `ttyACM*`，因为 USB-JTAG 会重新枚举） |

回归验证：把 `link.ld` 临时改回 `AT(0x30000)` 重跑 `make wifi`，
`chkimg.py` 报 `chkimg: 失败（1 处问题）` 并让 make 失败 —— 说明这道闸门有效。

### 同时修掉的两处窗口簿记（也都会炸）

1. `port/s3/start.S`：**无条件**做一次窗口归一（原来这段被
   `#ifndef __XTENSA_CALL0_ABI__` 包着，call0 构建里根本没编进去）：
   `WINDOWSTART = 1 << WINDOWBASE`、清 `PS.OWB`。ROM 跳到我们这里时
   WINDOWSTART 是"有空洞的活动窗口集合"（实测 `ws = 0x28`），call0 内核自己
   从不轮转窗口，这份脏状态会一直挂到第一次 `entry`，让它去 spill 一个
   早已不存在的老窗口 → `StoreProhibited`（`excvaddr = 0x40` 这种小地址）。
2. `port/s3/win_bridge.S` 的 `s3_asm_flash_42`：手写的 windowed 函数必须
   **成对** `entry` / 宽 `retw`。原来只有 `movi a2,42; retw.n`：
   `call8` 把 `CALLINC=2` 写进 a0，`retw.n` 按 CALLINC 去"退窗口"，
   但根本没进过窗口 → 非法指令/欠载（真板签名：`cause=0x1c`，PC 落在 `retw.n`）。
   该文件现在用 `-Wa,--no-transform` 编译，防止 `retw` 被收窄。

### IRAM 预算（WiFi 全量镜像）

IRAM 又一次溢出（这次 -184 字节）。把 **`task.o`（11KB）整体挪进
`.flash.text`**：`port_task_switch/yield` 在 `ctx.S`（仍在 IRAM），
中断入口在 `irq.S`，切换路径本身不经过 flash，所以是安全的。
（`kprintf.o` 上一轮已经挪过。）

### 真板实测（修好之后的第一次跑通）

`tests/t_s3_wifi3.c6`（新增：先打 flash 诊断，再建 windowed 任务跑 PHY +
`esp_wifi_init` + `esp_wifi_start`），串口原文：

```
c6os s3 boot
flash: text va=0x42010000 lma=0x00010000 end=0x4207e8df size=0x0006e8df
flash: rodata va=0x3c000020 lma=0x00000020
flash: mmu(text)=0x00000001 mmu(rodata)=0x00000000
flash: icache_ctrl=0x0000000b ctrl1=0x00000000
flash: after mmu(text)=0x00000001 mmu(rodata)=0x00000000
flash: probe(42)=0x0000002a          <-- **从 flash 取指并 retw 回来成功**
[wifimain] id=1 tick=0               <-- 闭源库 printf 出口（我们的 mini 格式化）也通了
W0 phy start
```

`probe(42)=0x2a` 是本项目的分水岭：IROM 取指 + windowed `entry`/`retw`
终于都正常了。`[wifimain] id=1 tick=0` 说明 `net80211_printf` 这类
"闭源库调我们"的回调也已经跑起来。

### 新问题：PHY 初始化把 USB-Serial-JTAG 弄没了（下一轮第一件事）

`W0 phy start` 之后**再无输出**，并且板上 USB 设备**整个消失**
（`lsusb` 里没有 `303a:`，`/dev/ttyACM*` 也不存在，`usbreset.py` 找不到设备），
只能物理断电重插。两次抓取都是这样，第一次还抓到了 15s 的完整输出，
第二次连串口都枚举不出来。
下一个要看的就是 `s3_win_call_phy_cal()` / `s3_win_call_phy_enable()`
（libphy 的 `phy_init`/`phy_enable` 路径）：
* 最可能是在某个 `while` 忙等里死转（`register_chipv7_phy_init_param` 之类
  需要 RTC 时钟/校准数据），CPU 卡死且看门狗被关掉 → 永不复位；
* 也可能是 PHY 改 PLL/时钟把 USB-Serial-JTAG 的 48MHz 时钟源掐了。
下一轮的诊断顺序（都尽量**不依赖 USB**）：
1. 在这两个调用前后各打一颗 RTC beacon（`0x50000xxx`），复位后用
   `tools/rd.py` 读回 —— 不需要串口在线也能知道死在哪一步；
2. 先用 `S3_KEEP_SWD=1`（默认已开）保住 super watchdog，别让它被关掉；
3. 如果确认是 PHY 忙等，再考虑走 IDF 的完整 `esp_phy_init` 序列
   （`esp_phy_init.c` 里 `register_chipv7_phy_init_param` 前后的时钟/校准步骤），
   而不是直接裸调 `phy_*`；
4. 另外准备一路"普通 UART"日志（GPIO43/44 之外），这样 CPU 卡死时至少还能看到
   最后一条日志。

### 本轮其余代码改动

* `port/s3/wifi_glue.c`：`esp_wifi_connect/disconnect`、`MESH_EVENT`、
  `esp_mesh_send_event_internal` 这几个薄封装整段包进 `#ifdef S3_WITH_WIFI`
  （它们引用闭源库的 `*_internal`，普通构建里会 undefined reference ——
  别再把基线 `make s3` 弄挂）。
* `tests/t_s3_wifi3.c6`：新的 WiFi 启动用例（flash 诊断 + windowed 任务）。
  `Makefile` 的 `wifi` 目标默认改用它（`S3_WIFI_MAIN`）。
* 构建状态：`make wifi` → `text 512845 / data 94816 / bss 113792`，
  `chkimg: OK（2 个 flash 段）`；
  基线 `make ... s3`（§201 那条命令）依旧通过。

### 新增调试装置：RWDT 自愈复位 + 阶段信标（本轮最后一步，未上板验证）

`W0 phy start` 之后板子从 USB 上彻底消失这件事，让"每次实验都要人工断电"
成为最大的迭代障碍。为此新增：

| 位置 | 内容 |
| --- | --- |
| `port/s3/flash.c` | `os_wdt_start(secs)` / `os_wdt_feed()` / `os_wdt_stop()`：直接把 ESP32-S3 的 RTC 看门狗（RWDT，`0x60008000` 区，寄存器位定义核对过 IDF 的 `rtc_cntl_struct.h`）配成"超时复位整片"。**只显式调用才生效**，不影响其它构建 |
| `tests/t_s3_wifi3.c6` | `os_main` 一进来 `os_wdt_start(8)`：8s 不喂狗 → 整片复位 → USB 重新枚举 → 板子自己回来 |
| `port/s3/win_island.c` | `s3_win_wifi_main()` 里每个危险调用前写阶段信标 `0x50000A10` 并 `os_wdt_feed()`：`0x1001` 进 PHY 前、`0x1002` 调 `phy_cal` 前、`0x1003` 调 `phy_enable` 前、`0x1004/0x1005` 进 `s3_wifi_bringup`、`0x1006` 之后 |

RTC 慢存跨 CPU 复位不清，所以下一轮只要**复位后读一次 `0x50000A10`**就知道
上一轮挂在哪个调用里（`tools/rd.py` / `monitor rtc`）。判据：
- 如果 8s 后板子自己复位、串口重新出现 → 说明是**忙等死转**（CPU 还活着），
  按信标定位到具体调用；
- 如果连复位都没救回来 → 说明是 PHY 改时钟把 USB-Serial-JTAG 的时钟源掐了
  （那种情况必须走 IDF 的 `esp_phy_common_clock_enable` 顺序，或者换一路普通
  UART 日志）。

---

## §203 闭源库到底用了 OSI 表的哪些槽位（静态统计）+ 两个真实隐患修正

§192 里我们是靠"运行期在 glue 里记计数器"猜哪些回调被调用了。板子不在手上时，
更直接的办法是**反汇编闭源 .a**：WiFi 库调用 OSI 回调的固定形态是
"取 `g_osi_funcs_p` → `l32i aN, base, 4*slot` → `callx8 aN`"，把每个
`callx8` 往回追到最近一条写该寄存器的 `l32i`，就能得到**确凿**的槽位使用清单。

工具已经固化：`tools/osislots.py`（槽位名字直接读 `include/c6os/wifi_osi.h`
的字段声明顺序，所以永远和 ABI 同步；默认库目录是 IDF 的
`components/esp_wifi/lib/esp32s3`）。

```
python3 tools/osislots.py            # 全部库
python3 tools/osislots.py --limit 5  # 每个 .a 只扫前 5 个 .o，快点看
```

### 结论一：这些槽位根本没被闭源库读过（= 空实现完全无害）

`_phy_enable`、`_phy_disable`、`_phy_update_country_info`、`_wifi_clock_enable/disable`、
`_wifi_reset_mac`、`_timer_arm*`、`_dport_access_*`、`_wifi_apb80m_*`、`_coex_init/_coex_deinit`。

其中 **`_phy_enable` 没被读过**这条很重要：说明"PHY 必须由我们自己按 IDF 的
`esp_phy` 顺序显式初始化"是**对的**，不用指望库反过来叫我们，也不存在
"我们没实现 `_phy_enable` 导致 PHY 没开"这种问题 —— 现在 `W0 phy start` 之后
的挂死只能是我们自己那段 `s3_win_phy_*` 顺序/寄存器写错。

### 结论二：`_nvs_*` 是**真会被调用**的（原先全是返回 0 的空实现 → 真 bug）

统计到 `_nvs_open`(x8)、`_nvs_get_blob`(x11)、`_nvs_set_blob`(x21)、`_nvs_get_i8/u8/u16`、
`_nvs_commit`(x8)、`_nvs_erase_key`，调用点集中在
`libnet80211:wifi_nvs_load / wifi_nvs_compare_cfg_diff / wifi_nvs_commit`、
`libcore:misc_nvs_load`、`libmesh:esp_mesh_nvs_init/operate`。

危险点在于**返回 0 在 IDF 的 `esp_err_t` 里就是 `ESP_OK`**：

1. `_nvs_open()` 返回 0 → 库认为打开成功，拿到一个我们随手写 0 的 handle；
2. 接着 `_nvs_get_blob(handle, key, buf, &len)` 也返回 0 → 库认为"读到了上次保存的
   数据"，于是把 `buf` 里的**未初始化内容**当校准数据/配置用下去。

`tests/t_s3_wifi3.c6` 里那份 8KB 任务栈、以及 `g_wifi_cfg_blob` 附近的静态区，
内容都是随机的 —— 一旦被当成"已保存配置"解读，轻则行为诡异，重则按里面的
长度/索引越界。修正（`port/s3/wifi_glue.c`）：

| 槽位 | 现在的返回 |
| --- | --- |
| `_nvs_open` | `0x1101` = `ESP_ERR_NVS_NOT_INITIALIZED` |
| `_nvs_get_i8/u8/u16/blob` | `0x1102` = `ESP_ERR_NVS_NOT_FOUND`（`*len` 清 0） |
| `_nvs_set_*` / `_nvs_commit` | `0x1101` |
| `_nvs_erase_key` | `0x1102` |
| `_nvs_close` | void，什么也不做 |

这样库走的是"没存过配置 → 用默认值"的正常分支（也就是 IDF 第一次上电的行为）。

### 结论三：`_read_mac` 也必须返回真 MAC

同一个"返回 0 就是成功"的坑：`_read_mac` 原来把 MAC 全填 0 并返回 0（成功）。
真机上这会让 `esp_wifi` 认为网卡 MAC 是 `00:00:00:00:00:00` —— DHCP 客户端身份
不合法、部分 AP 直接拒绝关联，而我们后面**正是要靠 DHCP 才能访问 qq.com**。
改成返回这块板 efuse 里的 base MAC `cc:8d:a2:ec:99:c4`，并按 IDF `esp_read_mac()`
语义按 `type` 递增最后一字节（0=STA、1=AP、2=BT）。

> 待办：改成运行期从 efuse 读（`efuse_hal_get_mac()` 那套 ROM 接口），
> 现在先写死一块板的 MAC。

### 结论四：低编号槽位（sem/queue/task/malloc/intr）——统计里"没直接读到"，但确实在用

工具输出里 `_semphr_*`/`_queue_*`/`_task_*`/`_malloc`/`_set_isr`/`_ints_on` 等
都落在"没被直接调过"的列表里，但 §192 的运行期计数器 **证实** 槽位 13/18/29/37/43
都被真实调用过。原因：这些调用走 libcore 的 `OsSem_*`/`OsQueue_*` 一层薄封装
（先把自己缓存的函数指针或表指针转一手，`l32i` 和 `callx8` 之间隔得远，
静态追不上）。所以：

- 这个工具的结论**只能用于"确实被直接读过"的槽位**（那一侧是硬证据）；
- "没被直接读过"只意味着静态追不到，不能当成"没被调用" —— 需要运行期计数器兜底。

`_set_isr`/`_ints_on` 这一组仍然**没有定论**（静态追不到、运行期也没在计数器里
单独核对过）。如果下一轮上板出现"库卡在等中断"这类症状，第一个要查的就是：
WiFi MAC 中断（`ETS_WIFI_MAC_INTR_SOURCE`）到底有没有被装进我们自己的中断表。

### 构建状态

- `make C6_ROOT=/home/winger/c6 wifi` → `Successfully created ESP32-S3 image`、
  `chkimg: OK（2 个 flash 段，文件偏移 == LMA）`
- 基线 `make C6_ROOT=/home/winger/c6 BUILD=build/base S3_SKIP_C6="" S3_EXTRA_C6="…" s3`
  依旧通过（`chkimg: OK`）。

### 下一步（板子恢复供电后按这个顺序做）

1. 上电 → `lsusb | grep 303a` 确认 USB-Serial-JTAG 回来；
2. 刷 `build/wifi/c6os.bin`，观察串口：8s 内没输出就等**板子自己复位**
   （`os_wdt_start(8)` 的自愈狗），复位后用 `tools/rd.py` 读 `0x50000A10`
   看挂在哪个阶段信标；
3. 若信标停在 `0x1003`（`s3_win_phy_enable` 里 `register_chipv7_phy`）而 RWDT
   又能把片子救回来 → 说明 PHY 内部在忙等某个时钟/中断，下一步对比 IDF
   `esp_phy_common_clock_enable()` 里我们**漏掉的**寄存器
   （`MODEM_CLK`/`MODEM_RST`/`MODEM_ISO` 那一组，见 §202 末尾的清单）；
4. PHY 过了之后才轮到"扫描 → 关联 → DHCP → 取 https://qq.com"，
   MAC 现在已经是真 MAC，DHCP 这条路才成立。

---

## §204 把 IDF 的 S3 PHY 上电序列逐行对上 + 跨复位自动二分装置

### 一、IDF 到底做了什么（`components/esp_phy/src/phy_init.c` + `esp_hw_support/periph_ctrl.c`）

S3 上 `esp_phy_enable()` 的完整链条（`SOC_MODEM_CLOCK_IS_INDEPENDENT` 为假，
所以走的是 `periph_ll_*` 那套 ref_count 版本）：

| 步骤 | IDF 代码 | 实际写哪些寄存器（S3） |
| --- | --- | --- |
| ① | `esp_phy_common_clock_enable()` → `wifi_bt_common_module_enable()` | `SYSTEM_WIFI_CLK_EN(0x60026014) \|= SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M(0x78078F)` |
| ② | `phy_module_enable()` | `SYSTEM_WIFI_CLK_EN \|= SYSTEM_WIFI_CLK_PHY_EN_M(0x400000)`；顺带 RNG/WiFi 模块时钟；再 `DPORT_CLEAR_PERI_REG_MASK(SYSTEM_CORE_RST_EN_REG, 0)` |
| ③ | `esp_phy_load_cal_and_init()`：`phy_bbpll_en_usb(true)` | 见下面"反汇编发现" |
| ④ | 读 efuse MAC → `cal_data->mac`，`register_chipv7_phy(init_data, cal_data, mode)` | PHY 库内部 |
| ⑤ | `phy_module_disable()` | 关 WiFi/BT 模块时钟（PHY 和 RNG 保留） |

对照我们的 `s3_win_phy_clock_enable` / `s3_win_phy_power_on` / `s3_win_phy_enable`：
`0x78078F` 和 `0x400000` 两组位都写了，顺序也对，`phy_bbpll_en_usb` 也在
`register_chipv7_phy` 之前 —— **这一层没有漏**。所以"PHY 一初始化就整片失联"
只可能出在 ④ 里面，或者出在我们多做的那些动作上。

两个新发现：

1. **`phy_bbpll_en_usb()` 反汇编出来只是写一个标志字节**（`phy_api.o`）：
   ```
   00000004 <phy_bbpll_en_usb>:
       entry a1, 32
       l32r  a8, <libphy 的 .bss 基址>
       addmi a8, a8, 0x200
       s8i   a2, a8, 166        ; 就是 "keep_usb_phy = en" 这样一个 byte
       retw.n
   ```
   也就是说它本身不会挂；真正的动作在 `register_chipv7_phy` 里根据这个 byte
   决定**要不要把内部 USB PHY 关掉**。IDF 的 Kconfig `ESP_PHY_ENABLE_USB` 说明
   写得很直白："on those affected ESP targets, the ESP PHY library's
   initialization will automatically disable the USB PHY" —— 这和我们的现象
   （初始化后 USB-Serial-JTAG 连 `303a:` 都不见了）完全吻合。我们已经调了它，
   但要验证它**真的生效**（即我们调的是同一份 libphy 实例里的那个 byte）。
2. **IDF 的"清复位"其实是个空操作**：
   `DPORT_CLEAR_PERI_REG_MASK(SYSTEM_CORE_RST_EN_REG, 0)` 的 mask 是 0，
   一位也不清。而我们 `s3_win_phy_power_on()` 自己补了一次
   `*rst |= 0x2A1F; *rst &= ~0x2A1F;`（assert + release）—— **比 IDF 多做了**。
   这一步在时钟刚打开之后把 modem 各模块复位脉冲打了一遍，属于可疑动作：
   如果 ③④ 都排除了，下一个要试的就是去掉它。

### 二、装置：跨复位自动二分（`make S3_PHY_BISECT=1 wifi`）

"每试一次都要人工断电"是现在最大的迭代瓶颈，所以做了个不依赖人的实验器：

`port/s3/win_island.c` 里 `s3_win_phy_bisect()` 把 PHY 前置动作切成 7 个**累积**
子步骤，每轮复位只跑 `prefix[0..k]`，跑完**故意不喂狗** → RWDT(8s) 复位整片 →
下一轮 `k+1`。RTC 慢存跨复位不清，所以用三个字记账：

| RTC 慢存地址 | 含义 |
| --- | --- |
| `0x50000A20` | 本轮的 k（下一轮自增） |
| `0x50000A24` | bitmask：进了这一步、直到复位都没清掉 = **挂死在这一步**（同时当"以后跳过"标记） |
| `0x50000A28` | bitmask：这一步活着跑完了 |
| `0x50000A2C` | `0x600DD0E0` = 二分已全部跑完 |

子步骤（对应上表）：1 `WIFI_CLK_EN`、2 `power_on`(PWC/ISO/RST 脉冲)、
3 `phy_bbpll_en_usb(1)`、4 `phy_get_romfunc_addr()` + 检查 `g_phyFuns`、
5 `register_chipv7_phy_init_param()`、6 `register_chipv7_phy()`、
（7 预留给 `s3_win_usb_reclaim()` 之后要加的东西）。

**怎么用**：`make C6_ROOT=/home/winger/c6 S3_PHY_BISECT=1 wifi` 刷进去，
然后**什么都不用做**：约 7 轮 × ≤8s 自己跑完，最后一遍串口打出
`[bisect] done hung=0x...... ok=0x......`，正常 WiFi 流程接着往下走。

读法：
- `hung` 里置位的就是**加了这一步就活不下来**的子步骤 → 下一步就是针对它改；
- 如果某步只是**把 USB 掐了但 CPU 还活着**，它会被记成 OK —— 这种只能看
  主机端串口日志：最后一条是 `[bisect] step N enter` 而其后的 `ok` 没出现，
  就是第 N 步把 USB 弄没了。
- 这个装置顺便还验证了 RWDT 自愈狗到底能不能把片子从"USB 已死"的状态救回来
  （如果能，以后所有实验都不用人工断电）。

### 三、构建状态

| 命令 | 结果 |
| --- | --- |
| `make C6_ROOT=/home/winger/c6 S3_PHY_BISECT=1 wifi` | `Successfully created ESP32-S3 image` + `chkimg: OK` |
| `make C6_ROOT=/home/winger/c6 wifi`（不带开关，走 `#else` 桩） | 同上 |
| 基线 `make ... s3`（§201 那条） | 同上 |

### 四、下一步（板子一回来就按这个走）

1. 刷 `S3_PHY_BISECT=1` 的镜像，等它自己跑完 7 轮，抄下 `hung`/`ok` 两个 mask；
2. 按 `hung` 定位：
   - 挂在 step 4（`g_phyFuns` 没填上）→ ROM 函数表这条线从没通，回头查
     `phy_get_romfunc_addr` 的调用约定；
   - 挂在 step 6（真 `register_chipv7_phy`）→ 先试去掉 `power_on` 里那对
     `0x2A1F` 复位脉冲，再试 `S3_PHY_CAL_MODE=1/2`，再试把 `s3_phy_init_data`
     换成"全 0 的 128 字节"看是不是 init data 的问题（排除法）；
3. PHY 过了以后才轮到 `esp_wifi_init_internal`（§192~§203 那套 OSI 表已经铺好，
   NVS/MAC 两个隐患也修了），然后是扫描/关联/DHCP，最后才是取 https://qq.com。

---

## §205 找到"PHY 初始化把 USB 干掉"的机器码证据：`i2c_bbpll_set` 里的那个 byte

### 一、证据链（全部来自我们自己 `build/wifi/c6os.elf` 的反汇编，可复现）

1. `phy_bbpll_en_usb()` 在 `phy_api.o` 里只有 4 条指令（写在 `phy_param` 里）：

   ```
   4204d4c7: l32r  a8, <lit>   (3fcb60a0 <phy_param>)
   4204d4ca: addmi a8, a8, 0x200
   4204d4cd: s8i   a2, a8, 166      ; phy_param[0x2A6] = en
   ```
   `phy_param` 符号大小 `0x2e4`（`nm -S`），所以 0x2A6 确实是这个结构里的一个
   普通标志字节，写到 DRAM，不会碰只读的 IROM —— 这一步本身绝对安全。

2. 读这个 byte 的地方只有一个：`i2c_bbpll_set()`（我们镜像里 `0x42056658`）：

   ```
   420566ad: l32i  a8, a2, 0        ; a8 = I2C 函数表
   420566b0: l32r  a3, <phy_param>
   420566b6: s8i   a10, a3, 209     ; 存 I2C 读回值
   420566c4: s8i   a10, a3, 210
   420566c7: addmi a3, a3, 0x200
   420566ca: l8ui  a11, a3, 166     ; ← 就是 phy_param[0x2A6]
   420566cd: bnez.n a11, 420566e2   ; 非 0 → 跳过下面那一次 I2C 写
   420566cf: ... l32i a8, a8, 0x198 ; I2C 写回调
   420566d1: movi a14, 7
   420566da: movi a12, 5            ; reg 102 / sub 5  ← 关内部 USB PHY 的那一笔
   420566df: callx8 a8
   ```

   这正好是 IDF Kconfig `ESP_PHY_ENABLE_USB` 说明文字的机器码版本：
   *"the ESP PHY library's initialization will automatically disable the USB PHY"*。
   `phy_bbpll_en_usb(true)` 唯一的作用就是让这一笔 I2C 写被**跳过**。

### 二、于是发现一个真正的代码缺陷：两条 PHY 路径不一致

`port/s3/win_island.c` 里有两套 PHY 初始化代码：

| | 诊断用 `s3_win_phy_step()` | 正常路径 `s3_win_phy_enable()`（**在用的那个**） |
| --- | --- | --- |
| `s3_win_phy_power_on()`（PWC/ISO/复位脉冲） | 有 | **没有** |
| `phy_bbpll_en_usb(1)`（留住 USB PHY） | 有 | **没有** ← 板子从 USB 上消失的直接原因 |
| 填 `g_phyFuns` | `phy_get_romfunc_addr()` + 检查 | ROM 的 `phy_get_romfuncs()`，**没检查** |
| `register_chipv7_phy_init_param()` | 有 | **没有** |
| 事后 `s3_win_usb_reclaim()` | 无 | 有（但已经晚了） |

也就是说 §202 末尾那次"`W0 phy start` 之后整片失联"，**不是 PHY 库的锅，是我们
自己那条正常路径漏了三步**。现在把 `s3_win_phy_enable()` 改成和诊断路径
同一套顺序（并且每一步都 `ets_printf` 出来，方便串口直接看到进度）：

```
s3_win_phy_power_on()                ① 掉电/时钟/复位/隔离
phy_bbpll_en_usb(1)                  ② 留住 USB PHY（关键！）
phy_get_romfunc_addr(); 若 g_phyFuns==0 再 phy_get_romfuncs()   ③ 函数表
ets_printf("[phy] g_phyFuns=0x%08x")                             可观测
register_chipv7_phy_init_param(...)  ④
register_chipv7_phy(...)             ⑤  + ets_printf rc
s3_win_usb_reclaim()                 ⑥ 双保险
```

### 三、这和 §204 的自动二分是什么关系

现在有两条互补的观测手段：
- **§204 的自动二分**（`make S3_PHY_BISECT=1 wifi`）：万一 ② 之后还是挂，
  它会告诉我们到底卡在 1~6 的哪一步；
- **§205 的串口进度行**：`[phy] g_phyFuns=...` / `[phy] register_chipv7_phy rc=...`
  直接说明函数表有没有填上、主入口有没有返回。

判据组合：
| 串口最后一行 | 结论 |
| --- | --- |
| 只有 `W0 phy start`，没有 `[phy] g_phyFuns=` | 卡在 ①②（时钟/复位/隔离这一层） |
| `[phy] g_phyFuns=0x00000000` | ROM/libphy 的函数表入口没起作用 → ③ 的调用约定要查 |
| `g_phyFuns` 正常但没有 `rc=` | 卡在 ④⑤ 里（PHY 内部忙等） |
| `rc=0` 之后 | PHY 起来了，问题往下走到 `s3_wifi_bringup` |

### 四、构建状态

`make … wifi`、`make … S3_PHY_BISECT=1 wifi`、基线 `make … s3` 三者都通过，
`chkimg: OK（2 个 flash 段，文件偏移 == LMA）`。

---

## §206 第二个真 bug：`g_phyFuns` 不是 ROM 那个入口填的

§202~§205 一直以为"`g_phyFuns` 由 ROM 的 `phy_get_romfuncs()`（`0x40005c58`）填"。
反汇编我们自己的镜像看清楚了：**不是**。

```
4204da14 <phy_get_romfunc_addr>:            ; libphy 的入口
  4204da17: l32r a8, (40005c58 <phy_get_romfuncs>)   ; ROM 的
  4204da1a: callx8 a8                                  ; → 返回表指针（a10）
  4204da1d: l32r a8, (3fccf938 <g_phyFuns>)
  4204da20: s32i.n a10, a8, 0                          ; ★ 只有这里才写 g_phyFuns
  4204da22: ...                                         然后往表里补 libphy 自己的实现：
  4204da25: s32i a8, a10, 0x144   ; ram_temp_to_power
  4204da2b: s32i a8, a10, 0x208   ; ram_pll_vol_cal
  4204da31: s32i a8, a10, 0x264   ; ram_wifi_set_tx_gain
  4204da37: s32i a8, a10, 0x228   ; ram_wifi_get_tx_gain
  4204da3d: s32i a8, a10, 0x218   ; ram_bt_get_tx_gain
  4204da43: s32i a8, a10, 0x15c   ; ram_get_i2c_hostid
  ... （还有 0x268/0x224/0x008/0x00c/0x128/0x22c/0x20c/0x288/0x28c/0x1e4/0x258 …）
```

也就是说：

* ROM 的 `phy_get_romfuncs()` **只返回**表指针，不会写我们的 `g_phyFuns`；
* 真正"把表接上 + 打补丁"的是 libphy 的 `phy_get_romfunc_addr()`。

而我们原来的正常路径 `s3_win_phy_enable()` 只调了 ROM 那个 —— 于是
`g_phyFuns == 0`，而 `register_chipv7_phy()` 的头两条指令就是

```
4204e2b8: l32i.n a5, a7, 0    ; a5 = g_phyFuns   (= 0!)
4204e2ba: l32i   a5, a5, 0x204 ; 从 0x204 读 → 未映射，LoadStoreError
4204e2bd: callx8 a5
```

这正是 §202 那串"PC 落在奇怪地址 / cause=0x1c/0x1d"签名的一个来源。

### 顺带清掉一个"自己发明的调用"

`register_chipv7_phy_init_param()` 在整个 IDF 源码里只出现在
`components/esp_rom/esp32c2/ld/esp32c2.rom.ld`（C2 的 ROM 符号表）里，
S3 的 `esp_phy_load_cal_and_init()` **从不调它**。我们原来按"先 init_param 再主入口"
的顺序调了它 —— 现在已经从正常路径和诊断路径里都删掉，保持和 IDF 一字不差：

```
s3_win_phy_power_on()     ① PWC/时钟/复位脉冲/隔离   (IDF: common_clock_enable + phy_module_enable)
phy_bbpll_en_usb(1)       ② 留住 USB PHY             (IDF: CONFIG_ESP_PHY_ENABLE_USB)
phy_get_romfunc_addr()    ③ 填 g_phyFuns + 打补丁      (IDF: esp_phy_enable 内部生效)
register_chipv7_phy()     ④ 主入口                    (IDF: esp_phy_load_cal_and_init)
s3_win_usb_reclaim()      ⑤ 双保险把 USB 抢回来
```

（唯一和 IDF 不同的地方：`cal_data.mac` 我们是写死的板子 MAC，IDF 从 efuse 读。）

### 所以到目前为止，"PHY 一初始化就失联"的两个成因都已定位并修掉

1. **USB PHY 被库自己关掉** → 缺 `phy_bbpll_en_usb(1)`（§205，机器码级证据
   `i2c_bbpll_set` 里那个 byte）；
2. **`g_phyFuns` 是空表** → 缺 `phy_get_romfunc_addr()`（本节，机器码级证据
   `s32i a10, g_phyFuns`）。

两个都是我们自己的代码缺陷，不是闭源库或 windowed ABI 的问题。下一步上板如果
还有问题，`[phy] g_phyFuns=0x…` / `[phy] register_chipv7_phy rc=…` 两行输出
会直接指向位置；`S3_PHY_BISECT=1` 的自动二分（6 步）作为兜底。

---

## §207 IP 栈可行性：lwIP 能编进我们的工具链（已验证），L2 钩子在闭源库里

PHY 通了之后还差一整套 IP/TCP/TLS 才能取到 `https://qq.com`。这一节把"IP 栈
怎么来"这个最大的未知项**用编译验证**settle 掉，而不是继续靠估计。

### 一、结论先说

| 问题 | 答案 | 证据 |
| --- | --- | --- |
| 仓库里有 IP 栈吗？ | 没有（`grep lwip` 全仓库为空） | — |
| IDF 的 lwIP 能用我们的工具链编吗？ | **能**，32 个对象全过 | `make lwip` → `build/lwip/liblwip.a`，`text 49407 / data 12 / bss 1688` |
| lwIP 需要 FreeRTOS 吗？ | **不需要**（`NO_SYS=1` 时零依赖） | 我们只补了 `arch/cc.h` + `lwipopts.h` 两个头 |
| 闭源 WiFi 库给不给 L2 收发的钩子？ | **给** | `esp_wifi_internal_tx`（libnet80211）、`esp_wifi_internal_reg_rxcb`（libnet80211）、`esp_wifi_internal_free_rx_buffer`（libpp） |
| c6 标准库那套能不能直接用？ | 现在不行，需要 socket 层 | `/home/winger/c6/std/net/tcp.c6` 是 POSIX 薄封装（`socket/connect/poll/getaddrinfo`），所以最终要 `NO_SYS=0` + 自己的 `sys_arch` |

也就是分工很清楚：**L2（802.3 帧收发）用闭源 WiFi 库，ARP/IP/TCP/UDP/DHCP/DNS 用
lwIP，application 层用 c6 标准库**。不需要 IDF 的 `esp_netif`/`esp_event`/FreeRTOS。

### 二、新增文件

| 文件 | 作用 |
| --- | --- |
| `port/s3/lwip/lwipopts.h` | 我们自己的 lwIP 配置（IDF 那份第一行就 `#include "sdkconfig.h"`，我们没有 sdkconfig 生成流程）。当前 `NO_SYS=1`；`MEM_LIBC_MALLOC=1` + `MEMP_MEM_MALLOC=1` 让 lwIP 用内核堆，静态 RAM 只占 1.7KB |
| `port/s3/lwip/arch/cc.h` | lwIP 要的平台适配头：定宽类型、`PACK_STRUCT_*`、`LWIP_PLATFORM_DIAG/ASSERT`、`LWIP_RAND` |
| `port/s3/lwip/c6_lwip_port.c` | 平台钩子（`sys_now`/`c6lwip_diag/assert/rand`）+ netif 粘合（`linkoutput` → `esp_wifi_internal_tx`；RX 回调 → `netif->input` + 归还 eb；`c6_lwip_start(mac)` 起 lwIP + `dhcp_start`） |
| `Makefile` 的 `lwip` 目标 | 把上面的东西编成 `build/lwip/liblwip.a`（**还没接进 c6os.elf**，见第五节） |

### 三、踩到的两个编译坑（都会复现，记下来省时间）

1. **不能给 lwIP 加 `-Wa,--no-transform`**。这个开关是给手写汇编（`win_bridge.S`
   防 `retw` 被缩成 `retw.n`）用的；加到编译器生成的代码上之后，gas 不再做分支
   松弛，`-Os` 下生成的窄分支一旦超范围就直接报
   ```
   Error: operand 2 of 'bnez.n' has out of range value '4294967280'   (core/def.c)
   ```
2. **`LWIP_ACD=0` 时必须同时写 `LWIP_DHCP_DOES_ACD_CHECK=0`**，否则 lwIP 的
   `init.c` 直接 `#error`（它只检查"想要 ACD 就要三件套"这一侧）。
3. `arch/cc.h` 不能用 IDF 那份：它 `#include "sdkconfig.h"` + `esp_log.h` +
   `arch/sys_arch.h`，三个我们都没有。

### 四、集成顺序（等 PHY 在真板上跑通之后）

1. `lwipopts.h` 改 `NO_SYS=0`，写 `port/s3/lwip/sys_arch.c`（我们内核已经有
   sem/mutex/queue/task/tick/event，正好对上 `sys_sem_*`/`sys_mutex_*`/`sys_mbox_*`/
   `sys_thread_new`/`sys_now`），开 `LWIP_SOCKET=1` + `LWIP_NETCONN=1`；
2. `port/s3/link.ld` 里把 `liblwip.a` 的 `.text` **塞进 `.flash.text`**（IROM），
   千万别掉进 IRAM —— IRAM 只剩几 KB（§202 的预算表）；
3. 提供 POSIX 薄封装（`socket/connect/recv/send/poll/close` → `lwip_*`），
   这样 `std/net/tcp.c6`、`std/net/https.c6` 就能直接用；
4. 先 `ping`（ICMP）→ 再 DHCP 拿到地址 → 再 DNS 解析 `qq.com` → 再 TCP 443 →
   最后才是 TLS。

### 五、为什么现在不接进镜像

WiFi 关联（`esp_wifi_start` → 扫描 → 4 次握手）在真板上还没跑通，PHY 才刚把
两个真 bug 修完（§205/§206）。这时候把 48KB 的 lwIP 链进 `c6os.elf`，只会让
一个**本来就无法验证**的镜像再叠一层不可验证的代码，排障时更难分清是谁的问题。
所以 `liblwip.a` 先独立编出来放着，`make lwip` 可以单独验证；
`make wifi` / 基线 `make s3` 的行为完全不变。

### §207 补记：自动二分里的两个自坑（已修，都在上板前发现）

1. **RTC 慢存上电是随机值**。`BISECT_CUR` 没有校验字，第一次上电很可能读到
   `0x7fffffff` → `k >= BISECT_N` → 第一轮就打印 `done` 把整个二分跳过。
   现在加 `BISECT_INIT(0x50000A30) = 0x600DD0E1` 作账本有效性标记：
   不是这个值就把 CUR/HUNG/OK/DONE 全部清零并写标记（只有真掉电才会清掉标记，
   而 RWDT 的 "reset system" 属数字域复位，RTC 慢存保留 —— 这正是装置的前提假设；
   如果上板看到日志反复出现 `round k=0`，那就说明这个假设不成立，要在日志里记一笔）。
2. **每一步都要从满 8s 开始计时**。原来只在 `s3_win_wifi_main` 里喂狗，
   于是第 3 步以后实际只剩"上一步用剩的时间"，可能把"慢"误判成"挂死"。
   现在 `bisect_run()` 之前先 `os_wdt_feed()`。

### §208 lwIP 升级到 NO_SYS=0：自带 tcpip 线程 + BSD socket（编译验证）

§207 先把 lwIP 以 `NO_SYS=1` 编通了；这一步把它升到**最终形态**：`NO_SYS=0`，
lwIP 自己起一个 tcpip 线程、提供 BSD socket。这样 `/home/winger/c6/std/net/tcp.c6`
那种 POSIX 薄封装才有落脚点。

新增/改动：

| 文件 | 内容 |
| --- | --- |
| `port/s3/lwip/arch/sys_arch.h` | 句柄类型（`sys_sem_t`/`sys_mutex_t`/`sys_mbox_t` 用 unix 端口那种"指针的指针"语义）+ `sys_*_valid/set_invalid` 宏。**函数原型一个都不写** —— 全部由 lwIP 自己的 `lwip/sys.h` 声明（它先 include 本文件、之后才 `typedef lwip_thread_fn`，写原型会 "unknown type name lwip_thread_fn"） |
| `port/s3/lwip/sys_arch.c` | 静态池实现的 sys 层：sem→`os_sem_*`、mutex→`os_mutex_*`、mbox→内核队列、`sys_thread_new`→`os_task_create`（并置 `g_next_task_windowed=1`，因为 lwIP 线程体是 windowed 编译的）。全部经 `port/s3/w2c.S` 的 `s3_w2c_*` 壳调用 |
| `port/s3/lwip/lwipopts.h` | `NO_SYS=0`、`LWIP_SOCKET=1`、`LWIP_NETCONN=1`、`MEMP_NUM_NETCONN=4`、打开 `SO_RCVTIMEO/SNDTIMEO/RCVBUF` |
| `Makefile` | `lwip` 目标加入 `api/*.c` 与 `sys_arch.c` |

lwIP 2.x 的 sys_arch 是**指针传递**风格，和我们最初照 unix 老端口写的签名不一样，
改对之后才编过：
```c
err_t sys_sem_new(sys_sem_t *sem, u8_t count);
err_t sys_mutex_new(sys_mutex_t *mutex);
err_t sys_mbox_new(sys_mbox_t *mbox, int size);
```

结构体尺寸不 include 内核头，直接按 `include/c6os/{sem,mutex,queue}.h` 抄字节数
（`OsSem` 16 / `OsMutex` 12 / `OsQueue` 40）；容器用静态池
（16 sem / 8 mutex / 8 mbox × 16 槽），既避免 malloc 时序问题，也让"池满"
比"堆碎片"好定位。

规模（`make lwip`）：

| 配置 | 对象数 | text | data | bss |
| --- | --- | --- | --- | --- |
| `NO_SYS=1`（§207） | 32 | 49407 | 12 | 1688 |
| `NO_SYS=0` + socket（本节） | 42 | **71554** | 12 | **3384** |

也就是整套 IPv4+TCP+UDP+DHCP+DNS+ARP+BSD socket 只花 **~70KB flash、3.4KB 静态 RAM**
（PBUF/连接控制块都走内核堆）。IROM 现在只用了 452KB/16MB，完全放得下；
关键是接进来的时候**必须把 lwIP 的 `.text` 放进 `.flash.text`**（IROM），
不能让它掉进 IRAM（§202 的 IRAM 预算只剩几 KB）。

到"能取 https://qq.com"还差的软件件（都还没做，因为要等真板把 WiFi 关联跑通）：

1. POSIX 名字的薄封装：`socket/connect/recv/send/poll/close/setsockopt/getaddrinfo`
   → `lwip_*`（c6 标准库只认这些名字），外加 `errno` 映射；
2. 把 `liblwip.a` 接进 `c6os.elf` + `link.ld` 里路由 `.text` 到 IROM；
3. TLS：`std/net/https.c6` 只做记录层，握手要用 `std/net` 里的 `tlskdf`/`x25519`/`x509`
   或者把 mbedTLS 也照 lwIP 的办法编进来 —— 这是最后一段路，也是最不确定的一段。

### §209 socket 头：c6 的 libc 根本没有 `sys/socket.h`（我们自己补上）

查 `/home/winger/c6/c6c/include/`（c6 自带 libc 头）发现一件之前没意识到的事：

| 头 | 有吗 |
| --- | --- |
| `string.h` / `stdlib.h` / `stdio.h` / `unistd.h` / `errno.h` / `fcntl.h` | 有 |
| `sys/` 下 | **只有 `stat.h`** |
| `sys/socket.h`、`netinet/in.h`、`netinet/tcp.h`、`netdb.h`、`poll.h`、`arpa/inet.h` | **全都没有** |

而 `/home/winger/c6/std/net/tcp.c6` 一上来就 include 这一整套（它连 `<unistd.h>`
都用上了，`close()` 也没人声明）。也就是说：**要把 c6 标准库的 net 模块拿到
S3 上用，先得把这 6 个头写出来** —— 这跟 lwIP 编不编得过是两件独立的事。

做法（都在新目录 `port/s3/lwip/posix/` 里）：

| 文件 | 内容 |
| --- | --- |
| `posix/sys/socket.h` | `struct sockaddr/sockaddr_storage/sockaddr_in`、`AF_*/SOCK_*/SOL_SOCKET/SO_*/MSG_*/SHUT_*` 常量、函数原型，**逐字节照抄 lwIP 的 `lwip/sockets.h`**（含 BSD 风格的 `sa_len` 首字段），这样包装层是纯转发、不用搬字段 |
| `posix/netinet/in.h` | `INADDR_ANY`、`inet_addr/aton/ntoa/pton/ntop`、`htonl/htons/ntohl/ntohs` |
| `posix/netinet/tcp.h` | `TCP_NODELAY` 等（数值同 lwIP） |
| `posix/netdb.h` | `struct addrinfo`/`hostent` + `getaddrinfo/freeaddrinfo/gethostbyname` |
| `posix/poll.h` | `struct pollfd` + `poll()`（`tcp_connect(timeout)` 靠它实现超时） |
| `posix/arpa/inet.h` | 转发到 `netinet/in.h` |

**为什么不放进仓库根的 `include/`**：宿主（x86）构建也用 `-Iinclude`，
放进去会遮蔽 glibc 的同名头，把宿主构建搞坏。所以它们单独放，只在 S3 + lwIP
的构建里用 `S3_C6FLAGS=-Iport/s3/lwip/posix` 加进来（`S3_C6FLAGS` 已经存在于
`.c6` 编译规则里）。

配套：`port/s3/lwip/c6_lwip_posix.c` 提供**真符号**
（`socket/connect/poll/send/recv/getaddrinfo/...` → `lwip_*`），因为我们的
`lwipopts.h` 里 `LWIP_COMPAT_SOCKETS=0`（宏式兼容层只在 include 了 lwIP 头的
TU 里生效，而 c6 侧不 include lwIP 的头）。`close/read/write` 也一并提供
（freestanding 目标上没有 libc 的同名符号；可用 `C6_LWIP_NO_POSIX_IO` 关掉）。

离线验证：新增 `make netcheck` —— 用 c6c 编 `tests/t_s3_net.c6`（一个把
`getaddrinfo → socket → setsockopt → connect → poll → send → recv → getsockname
→ shutdown → close` 和 `hton*/inet_*` 全用一遍的编译期冒烟用例），再用 gcc 把
c6c 的产物按 windowed 编一遍，两侧都过才算 OK。

踩到两个 c6 语言层面的事（顺手记下）：
1. **注释里不能出现 `*/`**：我在 `arpa/inet.h` 的注释里写了 `hton*/ntoh*`，
   注释在那里就被提前闭合了，后面 `ntoh*` 被当成变量声明，
   报 `error: variable 'ntoh' must be initialized`。
2. c6 要求**局部变量必须带初始化**（`struct addrinfo hints = { 0 };`），
   裸的 `int fd;` 都过不了。

### 主线还差什么（更新版）

| 环节 | 状态 |
| --- | --- |
| WiFi 关联（PHY → start → scan → 4 次握手） | **卡在这**：PHY 两个真 bug 已修（§205/§206），等上板验证 |
| L2 收发 | 已确认闭源库给 `esp_wifi_internal_tx/reg_rxcb` |
| ARP/IP/TCP/UDP/DHCP/DNS + BSD socket | lwIP 编过（42 对象，text 71.5KB），`sys_arch` 已写 |
| POSIX 名字/头 | 本节完成（`make netcheck` 过） |
| 接进 `c6os.elf` | 没做（等 PHY 通；还要把 lwIP `.text` 路由进 `.flash.text`） |
| TLS 1.3 握手 + 证书链校验 | 没做（c6 std 只有记录层/KDF/x25519/x509，缺握手驱动与签名校验） |

---

## §210 把 lwIP 真正链进镜像：`make wifip` 通过（含内存预算实测）

§207~§209 都是"单独编"，这一步把它**链进 c6os.elf**，第一次拿到真实的
内存占用。新增目标：

```
make C6_ROOT=/home/winger/c6 wifip      # = wifi + -DS3_WITH_LWIP + build/lwip/liblwip.a
```

和 `wifi` 的差别只有两处：`-DS3_WITH_LWIP`（bringup 成功后接 IP 栈），
以及链接时把 `build/lwip/liblwip.a` 塞进 `--start-group`。

### 为链接做的四处改动

| 位置 | 改动 | 原因 |
| --- | --- | --- |
| `port/s3/link.ld` | `.flash.text` 里加 `*liblwip.a:*(.literal .literal.* .text .text.*)` 以及 `c6_lwip_port.o`/`c6_lwip_posix.o`/`sys_arch.o` 三个粘合目标文件 | lwIP 有 **71KB** 代码；不显式收进 flash 文本段就会掉进 `.text`（IRAM），直接把 67KB 的 IRAM 撑爆 |
| `port/s3/win_island.c` | `#ifdef S3_WITH_LWIP`：`s3_wifi_bringup()` 成功后调 `c6_lwip_start(mac)`，然后跑 2000 次 `c6_lwip_poll()` + 让出 CPU | 这是"关联之后"的路径；先接上、留出阶段信标 `0x1010/0x1011` 便于以后定位 |
| `port/s3/wifi_glue.c` | 补 `malloc/calloc/realloc/atoi/_ctype_[257]` | lwIP 的 `MEM_LIBC_MALLOC`/`MEMP_MEM_MALLOC` 直接引用 `malloc/calloc/realloc`；`netif.c` 用 `atoi`；`ip4_addr.c` 的 `isdigit` 查 newlib 的 `_ctype_`。**关键：malloc 必须和已有的 `free()` 落回同一个堆**（`free` 已经指向 `osi_free`），所以 `malloc = osi_malloc` |
| `port/s3/w2c.S` | 试着加过 `s3_w2c_sys_alloc/free/realloc`，**最后删掉** | 内核堆 `os_sys_*` 在 `port/s3/fast.c`，LEAN 构建里根本不编进来 —— 加了只会 undefined reference。结论：lwIP 的分配就走 OSI 那套 windowed 堆 |

### 实测内存（`make wifip`，全部真数字）

| 段 | VMA | 大小 | 说明 |
| --- | --- | --- | --- |
| `.flash.text`（IROM） | `0x42010000` | **490191 B (479KB)** | WiFi/PHY 闭源库 + 内核原语 + lwIP 全在这里，宽松 |
| `.vector` + `.text`（IRAM） | `0x40378000` | 1146 + 60120 = **61266 B (59.8KB)** | 预算 67328 B，**余 ≈6KB**（没爆） |
| `.data`（DRAM） | `0x3FCA0000` | 104192 B | |
| `.bss`（DRAM） | `0x3FCB9700` | 116816 B，`_bss_end = 0x3FCD5F42` | `_stack_limit = 0x3FCD6742 ≤ _stack_top = 0x3FCD9000`，链接期 ASSERT 过 → 栈区约 10.5KB |

`size` 口径：`text 551725 / data 104192 / bss 116816`，
`chkimg: OK（2 个 flash 段，文件偏移 == LMA）`。同一个构建里
`make wifi`（不带 lwIP）和基线 `make s3` 也都仍然通过。

### 一个必须先记住的约束：DRAM 只剩约 10KB

`_bss_end` 已经到 `0x3FCD5F42`，离 `_stack_top(0x3FCD9000)` 只剩约 12KB，
其中 2KB 是护栏。也就是说 **lwIP 的动态内存和闭源 WiFi 库共用同一个 16KB 的
`g_wifi_heap_mem`**（`S3_WIFI_HEAP_SIZE`），而 DRAM 已经没有空间把这个堆放大。

到时候（等关联跑通、真正收发数据时）要按这个顺序调：
1. 先量：`g_osi_*` 的 peak 会打在 RTC/串口诊断里，先看 WiFi 侧到底吃多少；
2. 再缩 lwIP：`TCP_MSS 536`、`TCP_WND 4KB`、`TCP_SND_BUF 4KB`、`MEMP_NUM_TCP_PCB 4`
   —— 只为取一次 `https://qq.com` 的话足够；
3. 还不够就把 `.bss` 里的大户挪走（音频/图像缓冲在 WiFi 构建里本来就不该在）。

### 下一步

板子一回来，优先级顺序不变：
1. `S3_PHY_BISECT=1` 的 `wifi` 镜像 → 看 `[phy] g_phyFuns=` / `rc=` / bisect mask（§204~§206）；
2. PHY 通了再看 `esp_wifi_start` → 扫描 → 关联；
3. 关联成功后才轮到 `wifip` 镜像里的 `[lwip] start rc=` 和 DHCP。

---

## §211 **决定性发现：qq.com 不支持 TLS 1.3**，方案必须改成 TLS 1.2

前面几节一直按"TLS 1.3 + c6 标准库的 tlskdf/tlsrec"做规划。这一节把这条假设
直接推到真服务器上验证，结论是**假设不成立**。

### 一、实测证据（`openssl s_client`，宿主上就能做）

```
$ openssl s_client -connect qq.com:443 -servername qq.com -tls1_3 -brief
error:0A00042E:SSL routines:ssl3_read_bytes:tlsv1 alert protocol version
   ← 连 OpenSSL 自己都被服务端回 "protocol version" alert（alert 70）

$ openssl s_client -connect qq.com:443 -servername qq.com -brief
CONNECTION ESTABLISHED
Protocol version: TLSv1.2
Ciphersuite: ECDHE-RSA-AES128-GCM-SHA256
Peer certificate: C=CN, ... CN=qq.com
Hash used: SHA256
Signature type: RSA-PSS
Verification: OK
```

也就是说 `qq.com:443` **只谈 TLS 1.2**，套件是 `ECDHE-RSA-AES128-GCM-SHA256`。

再确认两件决定实现量的事：

```
$ openssl s_client ... -tls1_2 -groups X25519 -brief          → 成功（组 = X25519 ✓）
$ openssl s_client ... -tls1_2 -sigalgs rsa_pkcs1_sha256 ...  → 成功（签名 = RSA PKCS#1 v1.5 ✓）
```

证书链（`-showcerts`，三级，Verify return code: 0）：

| # | subject | 公钥/签名 |
| --- | --- | --- |
| 0 | `C=CN, O=Shenzhen Tencent Computer Systems, CN=qq.com` | RSA-2048，RSA-SHA256 |
| 1 | `C=US, O=DigiCert, CN=DigiCert Secure Site OV G5 TLS CN RSA4096 SHA384 2022 CA1` | RSA-4096，RSA-SHA384 |
| 2 | `C=US, O=DigiCert, CN=DigiCert TLS RSA4096 Root G5`（← 由 `DigiCert Global Root G2` 交叉签名） | RSA-4096 |

### 二、这对已有规划的影响

| 原规划 | 结论 |
| --- | --- |
| 用 `std/net/tlskdf.c6`（TLS 1.3 密钥调度）+ `std/net/tlsrec.c6`（TLS 1.3 记录层，ChaCha20-Poly1305） | **用不上**。TLS 1.2 需要 P_SHA256 PRF + AES-128-GCM，完全是另一套 |
| 缺的只是"握手驱动" | 不止：还缺 **AES-128-GCM**（`std/encoding/aes.c6` 只有 ECB 和 CTR，**没有 GHASH**）、**TLS 1.2 PRF**、**RSA 验签**（`std/encoding/rsa.c6` 根本不存在，`x509.c6` 的注释只是指了个还没写的模块）、**X.509 链校验** |

### 三、修订后的 TLS 1.2 客户端清单（全部用 `.c6` 写，宿主可验证）

| 模块 | 内容 | 估算 |
| --- | --- | --- |
| `aes128_gcm` | GHASH（GF(2^128) 乘）+ CTR 加密 + 16 字节 tag；`std/encoding/aes.c6` 已有 `ctr_xor` 和块加密，只缺 GHASH | ~150 行 |
| `tls12_prf` | `P_SHA256`（`hmac_sha256` 已存在于 `std/encoding/sha256.c6`）→ master secret / key expansion / finished | ~40 行 |
| `rsa_pkcs1_verify` | 从 SPKI 取 (n,e) → `bignum` 模幂 → 解 PKCS#1 v1.5 填充 → 比对 `DigestInfo(SHA256)` | ~120 行 |
| `tls12_hs` | ClientHello/ServerHello/Certificate/ServerKeyExchange/ClientKeyExchange/CCS/Finished 的状态机 + 记录层 | ~700~900 行 |
| 链校验（可选但推荐） | 叶子证书 CN=qq.com + 用 #1 的公钥验叶子签名 + 用内嵌根验 #1；根可以内嵌 `DigiCert Global Root G2` 的 DER | ~150 行 |

关键简化（都由实测确认可行）：
- **只提供 X25519**（`std/net/x25519.c6` 直接可用，不用写 P-256）；
- **只提供 `rsa_pkcs1_sha256`** 作为 `signature_algorithms`，服务端就会用 PKCS#1 v1.5，
  **不用写 RSA-PSS/MGF1**；
- **只提供 `ECDHE-RSA-AES128-GCM-SHA256`**（0xC02F）一个套件；
- ALPN 只提供 `http/1.1`（不提供 h2，省掉 HTTP/2）。

### 四、宿主上的开发闭环（重要）

c6 的编译器可以直接在宿主上编+跑（`tcc/c6`，多文件 + 模块源一起给就行）：

```
/home/winger/c6/tcc/c6 -B /home/winger/c6/tcc -I/home/winger/c6 -o probe \
    /home/winger/c6/std/core/types.c6 /home/winger/c6/std/net/tcp.c6 probe.c6
```

本轮就是用这条路**手工构造了一个 TLS ClientHello 发给真 `qq.com:443`**：
记录层/扩展长度全部正确（服务端解析后回的是 `protocol version` 这种"协议层"告警，
而不是 `decode_error`），拿到这个结论只花了几分钟。也就是说：
**TLS 1.2 客户端可以在宿主上对着真服务器开发、调试到通，再搬到板子上**——
这比"上板盲试"高效得多，也把最后这段路的风险从"未知"变成"已知工作量"。

板子上唯一要多做的是把 `Tcp` 换成 lwIP 的 socket 实现（§210 已经链进来了）。

---

## §212 TLS 1.2 基础件：AES-128-GCM / PRF / RSA 验签（宿主上已验证通过）

§211 定下了 TLS 1.2 方案，这一节把**三个最底层、最容易写错**的件写完并在宿主上
对着公开测试向量验证通过。新增 `net/` 目录（跨宿主/板子复用的协议实现）：

| 文件 | 内容 | 验证方式 | 结果 |
| --- | --- | --- | --- |
| `net/aes_gcm.c6` | AES-128-GCM（GHASH + CTR + tag；`std/encoding/aes` 提供块运算和 key schedule） | NIST SP 800-38D 的两个用例（全零用例 + Test Case 3，带 20 字节 AAD/60 字节明文） | 密文、tag 全对；改 tag 必须拒绝 |
| `net/tls12_prf.c6` | TLS 1.2 PRF（`P_SHA256`）+ `master secret` / `key expansion` / `finished` 三个便捷入口 | 用宿主 python 的 `hmac`/`hashlib` **独立算出**参考值（不是抄自己） | 三个向量全对 |
| `net/rsa_verify.c6` | RSA PKCS#1 v1.5 + SHA-256 验签：自写 4096 位上限定长模运算（平方-乘 + 逐位长除法）+ SPKI(DER) 解析 | 宿主 `openssl genrsa` 现场生成 RSA-2048 / RSA-4096 密钥与签名 | 两种模长都验过；改消息/改签名/改长度一律拒绝 |

一键跑：`make tls12check`（= `tools/tls12_selftest.sh`，**不需要板子**）。

### 为什么不用 `std/core/bignum.c6`

它是**定长 1024 位**（`BIG_LIMBS=32`），而 qq.com 的叶子证书 RSA-2048、
中间/根证书 RSA-4096 —— 差一档就废。所以 `net/rsa_verify.c6` 自己带一套
128 limb（4096 位）的模运算，只做 RSA 需要的那几件事。代价是 `bn_mod` 里要一个
129 limb 的临时余数（左移会临时多一位）。

### 三个真踩到的坑（都记在代码注释里，避免以后再犯）

1. **c6/tcc 里对 32 位值移 ≥32 位是按 `&31` 取模的**，不是 0。GCM 的长度块
   `len(A)*8 || len(C)*8` 一开始就是 32 位写的，结果低字节被重复写进高位 →
   tag 一直不对（密文却是对的，很迷惑）。改成 `u64` 移位后立刻对。
2. **HMAC 永远吐 32 字节**：PRF 的最后一块往往只要几个字节，直接写 `out+off`
   会越过调用方栈上的其它局部量（表现为"前面的用例过了，后面的莫名失败"）。
   改成先写临时 32 字节再 `memcpy` n 个。
3. **模运算的余数要 n+1 limb**：`bn_mod` 直接拿调用方的 `r[n]` 当 129 limb 用，
   RSA-2048 侥幸没炸、RSA-4096 直接验签失败。改成内部缓冲、最后拷回 n limb。
   另外 DER 的 INTEGER 为了让最高位不当负数会在前面补 `0x00`，RSA-4096 的 `n`
   因此是 **513 字节** —— 不先跳前导零会被误判成"超出 128 limb"。

### 栈占用要注意

`rsa_pkcs1_sha256_verify` 的调用链（`bn_modexp` + `bn_modmul` + `bn_mod` 的本地
数组）峰值约 **3.5KB**。板子上跑 TLS 时这个任务要给足栈（建议 ≥6KB），
别用默认小栈。

### 下一步

TLS 1.2 握手状态机（`net/tls12_hs.c6`）：ClientHello → ServerHello → Certificate
→ ServerKeyExchange（用上面的 RSA 验签）→ ClientKeyExchange → CCS → Finished，
记录层用 `aes_gcm`，密钥用 `tls12_prf`。仍然先在宿主上对着真 `qq.com:443`
跑到通，再搬板子。

---

## §213 **协议层打通：宿主上真取到了 https://qq.com 的页面**（TLS 1.2 客户端完成）

这一节是全局目标里"协议层"那半边的终点：`net/tls12_hs.c6` 写完并在宿主上对着
真服务器跑通了 TLS 1.2 握手 + HTTP GET（含跟随 302）。

```
$ ./tools/tls12_live.sh          # 或 make tls12live
== 连接 qq.com:443，请求 /
== 握手完成（阶段 21，套件 0xc02f）
---- 第一跳 HTTP 状态 302，共 318 字节 ----
---- 跟随重定向到 www.qq.com/ ----
== 连接 www.qq.com:443，请求 /
== 握手完成（阶段 21，套件 0xc02f）
---- 第二跳 HTTP 状态 200，共 7020 字节 ----
  <title>腾讯网</title>
[证据] 响应里含 <title>，即拿到了 HTML 页面内容
```

也就是说：**除了"WiFi 关联 + lwIP"这半边（板上），剩下整条协议栈已经验证可用。**

### 新增/改动

| 文件 | 内容 |
| --- | --- |
| `net/tls12_hs.c6` | TLS 1.2 客户端：ClientHello/ServerHello/Certificate/ServerKeyExchange/ClientKeyExchange/CCS/Finished 状态机 + 记录层（AES-GCM）+ 应用数据读写。I/O 只用 `read()/write()`，宿主和板子（lwIP POSIX 层）通用 |
| `tests/t_tls12_live.c6` | 真连 qq.com → 302 → www.qq.com，打印状态行与页面开头 |
| `tools/tls12_live.sh` / `make tls12live` | 一条命令跑上面这个用例 |
| `tools/tls12_selftest.sh` / `make tls12check` | §212 的三个基础件自测 |

### 调试过程中踩到的 5 个坑（都写进了代码注释，值得记牢）

1. **ALPN 扩展长度写错**（写成 13，实际 11）→ 服务端直接 `decode_error(50)`。
   扩展长度是手写二进制协议最容易错的地方。
2. **CCS 被当成"已加密"发出去**：原先用单个 `enc_on` 标志，装好密钥就置 1，
   结果 CCS（必须是明文）被加密，服务端回 `unexpected_message(10)`。
   现在收发两个方向分别有 `enc_out` / `enc_in`，各自在自己方向的 CCS 之后打开。
3. **AES-GCM 的 AAD 长度用错**：RFC 5288 里 AAD 的 length 是**明文**长度，不是记录体
   长度（含 8 字节 explicit nonce + 16 字节 tag）。发方向写成记录体长度 →
   服务端 `bad_record_mac(20)`；收方向同样错 → 我们解不开服务端的 Finished。
4. **NewSessionTicket 不在 transcript 里**：服务端在 CCS 之前会先发一条
   `NewSessionTicket`（qq.com 上 170~218 字节），它**也计入握手哈希**，漏掉就会导致
   "客户端 Finished 通过、服务端 Finished 验不过"这种很像密钥出错的症状。
5. **加密的 alert**：CCS 之后的 alert 是密文（我们一开始当明文读，报出 level=182
   desc=78 这种鬼数字）。现在 alert 也走记录层解密，`close_notify` 当 EOF 处理。

另外两个工程约束：
- 记录缓冲（8KB）和握手缓冲（12KB）都放在**模块静态区**，不占栈；代价是这个模块
  不可重入（一次一条 TLS 连接）。板子上 DRAM 紧，这是有意的取舍。
- `tls12_read_app()` 要求调用方缓冲 ≥ 一条记录的大小（qq.com 见过 2948 字节的记录，
  现在用例里给 8KB）。
- RSA 验签调用链峰值栈约 3.5KB（§212），TLS 任务栈要 ≥6KB。

### 现在距离目标只差"板上那半边"

| 环节 | 状态 |
| --- | --- |
| TLS 1.2 handshake / HTTP GET / 跟随 302 | ✅ 宿主上对着真 qq.com 验证通过 |
| AES-128-GCM / PRF / RSA 验签 | ✅ 向量验证（§212） |
| lwIP（ARP/IP/TCP/DHCP/DNS + BSD socket） | ✅ 编进镜像并实测内存（§210） |
| 板子的 socket 层 = lwIP POSIX 薄封装 | ✅ 已提供（§209），只是还没在真板上跑过 |
| **WiFi PHY/关联** | ⛔ 仍是唯一未通的一环：PHY 两个真 bug 已修（§205/§206），等板子回来验证 |

---

## §214 TLS 1.2 客户端上板：`make https` 链接通过（WiFi + lwIP + TLS 在一个镜像里）

上一节把协议层在宿主上跑通了；这一节把**同一份 `.c6` 代码**编成 windowed 静态库
链进 ESP32-S3 镜像，得到一个"bringup 成功后自动去取 https://qq.com"的完整镜像。

```
make C6_ROOT=/home/winger/c6 https
```

### 一、新增/改动

| 位置 | 内容 |
| --- | --- |
| `Makefile` 的 `https` 目标 | 在 `wifip` 基础上加三样：`-DS3_WITH_HTTPS`、`build/wifi/tls/libtls.a`、`build/wifi/obj/c6_https_demo.o` |
| `Makefile` 的 TLS 规则 | 三步：`tools/c6_strip_tests.py` 剥自测块 → c6c 出 `.c99` → gcc 按 `-mabi=windowed` 编成对象 → `ar` 成 `libtls.a`（11 个对象） |
| `tools/c6_strip_tests.py` | 去掉 c6 标准库模块里的 `test "…" { … }` 块（见下面"坑 1"） |
| `net/cert_parse.c6` | 自己写的最小 DER 证书解析：只取叶子证书的 SPKI（替代 `std/data/x509`，见"坑 2"） |
| `net/tls12_hs.c6` | 改成**调用方提供缓冲**：`tls12_set_buffers(rec, enc, out, hsbuf)`；`tls12_state_size()` 导出结构体大小；`tls12_trace` 默认关 |
| `port/s3/c6_https_demo.c` | 板级演示（windowed C）：`getaddrinfo("qq.com")` → `connect(443)` → TLS 1.2 握手 → `GET /` → 打印状态行与页面开头。缓冲全部从 OSI 堆里要 |
| `port/s3/wifi_glue.c` | 补 windowed 的 `printf()` 和 `int errno;` |
| `port/s3/win_island.c` | `S3_WITH_HTTPS` 时在 lwIP 起来之后调 `c6_https_demo()`，阶段信标 `0x1020` |
| `port/s3/link.ld` | `libtls.a` 和 `c6_https_demo.o` 的 `.text` 也收进 `.flash.text`（否则 IRAM 溢出 22048 字节） |

### 二、内存实测（`make https`）

| 段 | 大小 | 说明 |
| --- | --- | --- |
| `.flash.text`（IROM） | **535 KB** | WiFi/PHY 闭源库 + lwIP + TLS + 内核原语，flash 富余得很 |
| `.vector + .text`（IRAM） | 60.4 KB / 67.3 KB | 仍有余量 |
| `.data` | 109.8 KB | |
| `.bss` | 117.3 KB | `_bss_end = 0x3FCD76D6`，`_stack_limit = 0x3FCD7ED6 ≤ _stack_top = 0x3FCD9000`（链接期 ASSERT 过；栈区约 4.4 KB，很紧但够用） |

TLS 的缓冲（收记录 6.4KB + 发送 2KB + 握手消息 1KB + 握手累积 6.4KB + 状态结构
约 0.7KB ≈ 16.5KB）**不再占 .bss**，改成运行期从 OSI 堆（`g_wifi_heap_mem` 64KB）
申请 —— 这是这一节能链进镜像的关键：静态放不下（DRAM 只剩约 10KB）。

### 三、上板前踩到的四个坑

1. **c6 标准库的 `test "…" { … }` 块会被编进 `.c99`**，于是引入 c6 语言运行时的
   `__tcc_err_set_copy()` / `__tcc_slice_index()` 之类符号 —— 我们这套
   `-ffreestanding` 镜像里没有那套运行时，链接直接失败。所以加了
   `tools/c6_strip_tests.py` 在编译前剥掉（`test` 是顶层块，按大括号配对删）。
2. **`std/data/x509.c6` 依赖 `std/text/str.c6`**，而 str 里有切片操作 → 又拖回
   坑 1 的运行时依赖。TLS 1.2 其实只需要"叶子证书里那段 SPKI"，所以改成
   `net/cert_parse.c6` 自己走 DER（按 tag 跳过 6 个字段就到了），依赖清零。
3. **c6 模块里的 `extern void *malloc(...)` 会被修饰成 `net__tls12_hs__malloc`**
   （模块作用域），链不到 C 侧的 `malloc`。所以缓冲改成由调用方传进来
   （`tls12_set_buffers`），模块自己不碰分配器。
4. **`snprintf`/`printf`/`errno`/`strstr` 都得自己给**：freestanding 镜像里没有
   libc；`snprintf` 本来就有（wifi_glue.c），又补了 `printf`（用 `wp_vfmt`）和
   `int errno`；`strstr` 干脆在演示里用本地小函数替代。

### 四、现在的状态

| 环节 | 状态 |
| --- | --- |
| TLS 1.2 握手 + HTTP GET + 跟随 302 | ✅ 宿主上对着真 qq.com 验证到通（§213） |
| 同一份 TLS 代码编成 windowed 库链进镜像 | ✅ `make https` 通过，chkimg OK |
| lwIP（ARP/IP/TCP/DHCP/DNS + BSD socket） | ✅ 链进镜像（§210） |
| 其他镜像不受影响 | ✅ `wifi` / `wifip` / 基线 `s3` 三种构建都仍然通过 |
| **WiFi PHY/关联** | ⛔ 仍是唯一没在真板上通过的一环（§205/§206 已修，等板子） |

板子回来后一步到位的验证顺序：
1. `make S3_PHY_BISECT=1 wifi` 刷进去，确认 PHY 过（看 `[phy] g_phyFuns=` / `rc=` / bisect mask）；
2. PHY 过了就刷 `make https` 的镜像：串口应出现 `[https] === 开始取 https://qq.com/ ===`
   → `[https] TLS 握手完成（阶段=21）` → 状态行 + 页面开头；
3. 如果卡在 DHCP/DNS，会看到 `[https] DNS 解析 qq.com 失败`；
   卡在 TLS 会看到 `[https] TLS 握手失败，阶段=N`（阶段含义见 §213）。

### 五、上板前必须留意的两个余量（都还没在真板上量过）

1. **栈只剩约 4.4KB**。RSA 验签那条链的峰值栈约 3.5KB（§212），也就是说 TLS 握手
   时栈几乎是贴着上限跑的。上板如果出现莫名复位/数据错乱，第一个怀疑对象就是栈。
   调整手段（按代价从低到高）：
   - 把 `bn_modmul`/`bn_mod` 的大临时数组（约 2KB）改成模块静态 —— 但 .bss 也满了，
     需要先腾出空间；
   - 把 `g_wifi_heap_mem`（现在是 **64KB**）调小，把 DRAM 让给栈：`S3_WIFI_HEAP_SIZE`
     目前是 65536，WiFi 库 + lwIP + TLS 缓冲到底用多少要靠真板量（`os_sys_heap_peak`
     类似口径的诊断还没接到 OSI 堆上）。
   - 把 TLS 任务单独建一个更大栈的任务（现在跑在 bringup 那个任务里）。
2. **16.5KB 的 TLS 缓冲全在 64KB 的 OSI 堆里**，和闭源 WiFi 库、lwIP 抢同一块。
   如果真板上出现 `[https] TLS 缓冲申请失败`，说明堆不够，优先把
   `TCP_WND`/`TCP_SND_BUF`（现在各 4KB）再调小，或者把 TLS 缓冲降到
   4KB 记录 + 4KB 握手累积。

---

## §215 补上"关联"这一环：init 之后其实从来没连过 AP

§210~§214 把 IP 栈和 TLS 都铺好了，但回头看 `s3_wifi_bringup()` 会发现一个
致命缺口：它只调了 `esp_wifi_init_internal()` —— **从来没 set_mode / set_config /
start / connect**。也就是说即使 PHY 起来了、`esp_wifi_init` 成功了，板子也永远
不会去关联 AP。

### 一、补上的调用（`port/s3/wifi_glue.c`）

按 IDF 的顺序（每个调用都打一行结果，失败原因看串口）：

```c
esp_wifi_set_storage(1);       /* 1 = WIFI_STORAGE_RAM：我们没接 NVS，别落盘 */
esp_wifi_set_mode(1);          /* 1 = WIFI_MODE_STA */
esp_wifi_set_config(0, cfg);   /* 0 = WIFI_IF_STA，cfg 见下 */
esp_wifi_start();
esp_wifi_connect_internal();   /* 公共 API esp_wifi_connect() 在闭源库里没有，
                                * 只有 *_internal 版本（和 esp_wifi_init 一样） */
```

`cfg` 是一块 192 字节全零缓冲，只填 `wifi_sta_config_t` 的头部两个字段：
`ssid[32]` @0、`password[64]` @32 —— 这两个字段在 IDF 各版本里位置都稳定，
其余全零表示"用默认值"（任意 authmode、无 PMF、快速扫描）。

### 二、怎么知道连上了 / 拿到地址了

我们的 OSI 里 `_event_post` 是空实现（事件系统没接），所以**不靠事件**：

| 判断 | 手段 |
| --- | --- |
| 关联成功 | 轮询 `esp_wifi_sta_get_ap_info()` 返回 0（`s3_wifi_wait_connected(ms)`，默认等 15s） |
| 拿到 IP | 轮询 lwIP netif 的地址（`c6_lwip_wait_ip(ms)`，默认等 20s），期间不停调 `sys_check_timeouts()` 让 DHCP 自己跑 |

阶段信标（RTC 慢存 `0x50000A10`）：`0x1021` 开始关联、`0x1022` 关联成功、
`0x1023` 拿到 IP、`0x1020` HTTPS 演示结束。DHCP 超时会打印 `[lwip] DHCP 超时`。

### 三、账号怎么给

`port/s3/wifi_creds.h` 是默认值（空串 = 跳过关联），构建时可以用 make 变量覆盖：

```
make C6_ROOT=/home/winger/c6 https \
     S3_WIFI_SSID='"你家的AP名字"' S3_WIFI_PASS='"口令"'
```

（额外那层单引号是给 shell 的：`'"…"'` 让 C 看到带双引号的字符串字面量。）

### 四、板上串口应该看到的完整链路（板子回来后的验收清单）

```
W0 phy start
[phy] g_phyFuns=0x........            ← §206
[phy] register_chipv7_phy rc=0        ← §205/§206
W1 bringup
[wifi] associate ssid=...
[wifi] set_storage(RAM)=0 / set_mode(STA)=0 / set_config=0 / start=0 / connect=0
[wifi] 已关联，ssid=... channel=N      ← 关联成功
[lwip] ip=192.168.x.y mask=... gw=...  ← DHCP 成功
[https] TLS 握手完成（阶段=21）
[https] 第一跳 HTTP 状态 302 ...        ← 协议层跑通（宿主上已验证，§213）
```

### 五、构建状态

`make https`（含关联 + lwIP + TLS）与 `wifi` / `wifip` / 基线 `s3` 四种构建都通过，
`chkimg OK`。基线构建里新增的两个函数被 `#ifdef S3_WITH_WIFI` 包住（否则会引用
不存在的 `esp_wifi_set_storage` 等符号 —— 这次就踩到了，已修）。

### §215 补记：加一步"扫描"——**不需要 WiFi 密码**就能验证射频链路

关联需要用户给 SSID/密码，但"射频通不通"不该等密码。所以 `s3_wifi_associate()`
在 `esp_wifi_start()` 之后、`esp_wifi_connect` 之前先做一次**阻塞扫描**
（`esp_wifi_scan_start(cfg, 1)`，cfg 全零 = 全信道扫所有 SSID），并打印：

```
[wifi] scan start（阻塞）
[wifi] scan_start=0
[wifi] scan_get_ap_num=0 num=7
[wifi] AP0 ssid=<第一个 AP 的名字> ch=6 rssi=-46
```

- 扫到 `num > 0` 就是**PHY + MAC + 收发链路全部活着的硬证据**；
- 扫不到（`num=0`）说明还是射频/时钟层的问题，和密码无关；
- 没给 SSID 时也会走 set_mode + start + scan，所以板子回来第一件事就能验证射频，
  不必等账号到位。

只打印第一个 AP 的 SSID/信道/RSSI：`wifi_ap_record_t` 的结构体大小依赖 IDF 版本，
只取第一个记录不需要知道 stride（按 `bssid[6]@0, ssid[33]@6, primary@39, rssi@41`
读前几个字段，这几个偏移在各版本都稳定）。

---

## §216 把 OSI 的中断/事件槽位做成"记账实现"，用来判断库到底用不用

### 一、为什么

关联/扫描要用到 MAC 中断（RX 帧靠它），而我们的 OSI 里 `_set_isr`/`_ints_on`/
`_set_intr` 一直是 `glue_ret0`（空实现）。静态统计（`tools/osislots.py`，以及把
窗口收窄到 6 条指令的二次扫描）显示 **libnet80211/libpp/libcore 从来没有直接读过
这几个槽位** —— 按 IDF 的实现（`esp_adapter.c`：`_set_isr → xt_set_interrupt_handler`、
`_ints_on → esp_cpu_intr_enable`、`_set_intr → esp_rom_route_intr_matrix`），
这意味着 WiFi 库大概是自带中断安装逻辑（它们能直接摸 CPU 的向量表），不经过 OSI。

但"大概率不用"和"确实不用"在真板上一试就知道，而接内核的中断表是个大改动
（handler 表 + INTENABLE 操作 + 中断矩阵路由，还得小心翼翼别碰坏现在的
timer/调度路径）。所以这一步先做**零风险、但能给出答案**的版本：

| 槽位 | 现在的实现 | RTC 慢存记账位置 |
| --- | --- | --- |
| `_set_isr(n, fn, arg)` | 记录后返回 0 | `0x50000B00`：8 项 ×（中断号, 函数），`[16]` 是次数 |
| `_set_intr(cpu, src, num, prio)` | 记录后返回 0 | `0x50000B50`：4 项 × 4 字，`[16]` 是次数 |
| `_ints_on(mask)` / `_ints_off(mask)` | 记录（次数 + 最后 mask） | `0x50000B40` / `0x50000B48` |
| `_wifi_int_disable()` / `_restore()` | 记录次数 | `0x50000B40[2]` / `[3]` |
| `_is_from_isr()` | 恒返回 0（不在中断里） | — |
| `_event_post(base, id, ...)` | 记录后返回 0 | `0x50000B90`：16 项 ×（event_base, event_id），`[32]` 是次数 |

### 二、上板后怎么读（**不要用 esptool read-mem**）

`esptool read-mem` 会先复位芯片再读，读到的是新一轮启动的值，上一轮的账本已经
被覆盖。要用 JTAG 附着后直接读（`tools/dbg.sh` 里的 gdb 脚本模板）：

```
x/20xw 0x50000B00     # set_isr 环 + 次数
x/4xw  0x50000B40     # ints_on/off、wifi_int_disable/restore 的次数与最后 mask
x/16xw 0x50000B50     # set_intr 环
x/36xw 0x50000B90     # event_post 环 + 次数
x/1xw  0x50000A10     # 阶段信标（§204/§215）
```

判读：

| 看到什么 | 结论 |
| --- | --- |
| `0x50000B00[16]`（set_isr 次数）= 0 且 `0x50000B40[0]`（ints_on 次数）= 0 | 库确实不用 OSI 装中断 → 中断这条路不用管，问题在别处 |
| 任一计数 > 0 | 库在用，必须把内核的 IRQ 表和 INTENABLE 接上（下一轮的大改动） |
| `0x50000B90[32]`（event_post 次数）> 0 | 库在发 WiFi/IP 事件 → 可以把事件 id 解出来做状态机（比轮询更准） |

### 三、构建状态

`wifi` / `wifip` / `https` / 基线 `s3` 四种构建都通过，`chkimg OK`。
这一节的改动只在 `port/s3/wifi_glue.c`，不影响任何已有行为（空实现 → 记账 + 空实现）。

---

## §217 一键上板脚本：`tools/s3run.sh` + `make bringup` / `make httpsflash`

板子动不动就从 USB 上消失（§202/§205），每轮实验都是"等设备回来 → 刷 → 抓串口 →
翻日志找关键字"。这一步把这些合成一条命令，并且把原始串口日志落到
`build/logs/s3run-<时间戳>.log`，事后能翻。

```
# 第一步（板子回来先跑这个）：自动二分 + PHY 进度打印
make C6_ROOT=/home/winger/c6 bringup

# 第二步：完整镜像（WiFi + lwIP + TLS），记得给账号
make C6_ROOT=/home/winger/c6 httpsflash \
     S3_WIFI_SSID='"你家的AP"' S3_WIFI_PASS='"口令"'
```

脚本会等串口最多 120s（板子可能正在自己复位），刷完抓 60/120s，然后按启动顺序
逐项打印是否出现关键标记：

| 标记 | 含义 |
| --- | --- |
| `[phy] g_phyFuns=…` / `[phy] register_chipv7_phy rc=…` | PHY 走到哪一步（§205/§206） |
| `[bisect]` | 自动二分进度/故障图（§204） |
| `[wifi] scan_start` / `[wifi] AP0 …` | 射频体检：扫描是否工作（§215） |
| `[wifi] 已关联` | 关联成功 |
| `[lwip] start rc=` / `[lwip] ip=` | lwIP 起来 / DHCP 拿到地址（§210） |
| `[https] TLS 握手完成` / `第一跳` / `第二跳` | TLS 与 HTTP（§213/§214） |
| `panic` / `Guru` / `IllegalInstruction` | 崩了（本轮之前的老朋友） |

---

## §218 WPA 加密回调表：没有它，WPA2 关联必挂（PBKDF2 已用 IEEE 向量验证）

### 一、问题

`esp_wifi_init_internal()` 的配置块里，偏移 4 起是 `wpa_crypto_funcs`（44 字节，
9 个函数指针）。我们之前只填了 `size=44` / `version=1`，**九个指针全是 NULL**。
WPA2-PSK 关联时，闭源库要回调它们做两件核心的事：

1. `pbkdf2_sha1`：口令 + SSID → PSK（`iterations=4096`、输出 32 字节）；
2. `hmac_sha256_vector` / `sha256_vector`：EAPOL 的 MIC 与各种 KDF 的哈希。

NULL 指针 → 关联阶段直接跑飞。这是 §215 之后发现的最致命的一个坑。

### 二、做法（`port/s3/wpa_crypto.c`）

| 回调 | 状态 |
| --- | --- |
| `pbkdf2_sha1` | ✅ 真实现（PBKDF2-HMAC-SHA1），**IEEE 802.11i 测试向量验证通过** |
| `sha256_vector` | ✅ 真实现（向量拼接 + SHA-256），**NIST "abc" 向量验证通过** |
| `hmac_sha256_vector` | ✅ 真实现，**RFC 4231 case 2 向量验证通过**（顺便验证多段拼接） |
| `aes_128_encrypt` / `aes_128_decrypt` / `omac1_aes_128` / `ccmp_decrypt` / `ccmp_encrypt` / `aes_gmac` | ⏳ 先"记账 + 返回 -1"：ESP32-S3 的 MAC 有 CCMP/TKIP 硬件卸载，普通 WPA2-PSK 大概率不回调；真上板如果回调了，RTC `0x50000BC0` 起的计数会显示是哪一项 |

SHA-1 / SHA-256 / HMAC **自带 C 实现**（约 200 行，不依赖 c6 模块）。这样
`wpa_crypto.c` 是自包含的普通 C：既不拖累不链 libtls 的 `wifi` 构建，
也能在宿主上用 gcc 直接验证（`tests/wpa_crypto_selftest.c`）。

`wifi_glue.c` 里 `s3_wifi_bringup()` 现在把这张表**整块拷进 init config 的
偏移 4**（`c6_wpa_crypto_init()` + `c6_wpa_crypto_table()`），不再是只写
size/version。

### 三、验证

```
make tls12check
  ========== aes_gcm_test ==========      AES-128-GCM 自测通过
  ========== tls12_prf_test ==========    TLS1.2 PRF 自测通过
  ========== rsa_verify_test ==========   RSA 验签自测通过
  ========== pbkdf2_test ==========       PBKDF2 自测通过（IEEE 802.11i 向量）
  ========== wpa_crypto_selftest =====    wpa_crypto C 实现自测通过（gcc 直接编）
  全部 TLS 1.2 基础件 + WPA 密钥派生自测通过。
```

### 四、上板后怎么判断这一层对不对

关联成功（`[wifi] 已关联，ssid=…`）本身就说明 PSK 派生与 EAPOL MIC 都对
（PSK 错的话 4 次握手必然失败，表现是 `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT`，
只是我们没接事件系统，看不��具体原因 —— 所以串口只会有"关联超时"）。

如果 RTC `0x50000BC0[3..7]` 非 0，说明库回调了 AES/CCMP 那几项（也就是说硬件
卸载这条路没走通），那时就要在 `wpa_crypto.c` 里补真正的 AES-128（CBC/CMAC/CCM）。

---

## §219 任务上限只有 3 个、每个栈固定 8KB —— 又一个会挡住关联的结构性限制

### 一、问题

`port/s3/ctx.c` 里写死了：

```c
#define S3_MAX_CTX   3          /* 最多 3 个任务上下文 */
#define S3_STACK_SZ  8192       /* 每个任务固定 8KB 栈 */
static u8 g_stacks[S3_MAX_CTX][S3_STACK_SZ];   /* 共 24KB */
```

数一下我们**实际需要**的任务：

| # | 任务 | 谁创建 | 栈 |
| --- | --- | --- | --- |
| 1 | bringup / wifi 主任务 | `tests/t_s3_wifi3.c6` | 8KB |
| 2 | 监视任务 | 同上 | 4KB |
| 3 | libpp 的 `ppTask`（TX/RX 的包处理，**必需**） | 闭源库经 OSI `_task_create` | 3.5KB（我们抬到 6KB） |
| 4 | lwIP 的 `tcpip` 线程（DHCP/TCP 都靠它） | `sys_thread_new` → `os_task_create` | 4KB |

也就是 `wifi` 镜像刚好卡在 3 个（能跑），而 `https` 镜像的 lwIP tcpip 线程
**建不起来** —— 没有 tcpip 线程就没有 DHCP、没有 TCP，取页面无从谈起。
而且 `port_task_ctx_create` 失败时是**静默返回 0**，只表现为"某个任务凭空没起来"。

### 二、改法：栈池 + 放宽任务数

```c
#define S3_MAX_CTX     8          /* 任务数上限 3 -> 8 */
#define S3_STACK_POOL  24576u     /* 所有任务的栈从这一块里按需切 */
static u8 g_stack_pool[S3_STACK_POOL];
static u32 g_pool_off;            /* 单调分配，任务不会被删所以不回收 */
```

- 总内存**和原来一样是 24KB**（DRAM 一点没多花，§210 的余量还是几 KB）；
- 但"最多 3 个任务 × 8KB"变成了"最多 8 个任务、按请求大小切"；
- 每个任务最小 1KB（原来 512B，对闭源库/ lwIP 的线程太小）；
- 池不够时**不再静默**：打印 `[ctx] 栈池不够：需要 X，已用 Y/Z`，并把
  （需要, 已用, 总大小, 次数）写进 RTC 慢存 `0x50000BE0`。

预算：bringup/wifi 8KB + ppTask 6KB + tcpip 4KB + 监视 2KB ≈ 20KB ≤ 24KB ✓。

配套两处：
1. `port/s3/wifi_glue.c` 的 OSI `_task_create` 不再把库请求的栈一律抬到 8192，
   改成抬到 **6144**（否则"bringup 8KB + ppTask 8KB + tcpip 4KB"正好把池吃满）；
2. `port/s3/lwip/lwipopts.h` 显式给 `TCPIP_THREAD_STACKSIZE = 4096`
   （lwIP 在 `NO_SYS=0` 下这个默认值是 **0**，传下去会被内核当成最小值），
   `sys_arch.c` 的 `sys_thread_new` 再兜一个下限 4096。

### 三、构建状态

`https` / `wifi` / `wifip` / 基线 `s3` 四种构建都通过，chkimg OK。
`https` 镜像的余量：`_bss_end = 0x3FCD7C56`、`_stack_limit = 0x3FCD8456`、
`_stack_top = 0x3FCD9000`（主栈余量约 3KB；TLS 那些活都在任务栈上跑，不在主栈）。

---

## §220 收包路径必须走 `tcpip_input`（NO_SYS=0 下直接在 RX 回调里跑协议栈是错的）

`c6_lwip_port.c` 里原来的 netif 注册是：

```c
netif_add(&s_netif, 0,0,0,0, c6_lwip_netif_init_cb, ethernet_input);   // ← 错
...
if (nif->input(p, nif) != ERR_OK) { ... }                               // ← 错
```

在 `NO_SYS=0`（我们有 tcpip 线程）下，收包应该交给 **`tcpip_input`**：它把 pbuf
塞进 tcpip 的邮箱，由 tcpip 线程去跑 ARP/IP/TCP。直接调 `ethernet_input` 等于在
**WiFi 的 RX 回调上下文**里并行跑 lwIP 的全部协议处理，和 tcpip 线程同时摸同一批
全局状态（pcb 链表、memp 池）—— 轻则丢包乱序，重则把协议控制块踩坏。

现在两处都改成 `tcpip_input`（`netif_add` 的输入回调和 RX 回调里），
`TCPIP_MBOX_SIZE=16` 保证邮箱够放。

这条和 §219（任务上限/栈池）是同一类问题：**单线程假设下写的代码，接了 tcpip
线程之后必须重新检查每个"回调在哪个上下文跑"**。

---

## §221 两个"库会读多少字节 / 会不会去睡觉"的防御性修正

### 一、配置缓冲要按**库编译时的 sizeof** 给足

闭源库是按它自己编译时的 `sizeof(wifi_sta_config_t)` / `sizeof(wifi_scan_config_t)`
读我们传进去的指针的。我们原来分别给 192 / 16 字节：

- `wifi_sta_config_t` 现在约 184 字节 —— 192 只是"刚好"，一旦 IDF 版本再长一点，
  库就会读进我们 **bss 里后面的垃圾**：`bssid_set` 变成 1（只连指定 BSSID）、
  `channel` 变成随便一个信道，表现就是"配置明明对却连不上/一直超时"；
- `wifi_scan_config_t` 约 20~24 字节 —— 16 字节**现在就已经不够**了，
  `scan_type` / `home_chan_dwell_time` 会吃垃圾。

改成 **320 字节**（sta config）和 **64 字节**（scan config）的零缓冲。
多出来的零是最便宜的保险：任何"库读得比我们给的长"的字段都会落成 0（= 默认）。

### 二、显式关掉省电

`esp_wifi_start()` 之后加一句 `esp_wifi_set_ps(WIFI_PS_NONE=0)`。理由：modem sleep
依赖 OSI 的两个回调 —— `_wifi_rtc_enable_iso` / `_wifi_rtc_disable_iso` —— 而我们
那两个是空实现（RTC_CNTL_DIG_ISO 的 WIFI_FORCE_ISO 位），睡眠一开 RX 就可能
收不到包。取一个页面完全不需要省电，所以直接关掉。

串口会多一行 `[wifi] set_ps(NONE)=0`。

### 三、构建状态

`https` / `wifi` / `wifip` 三种构建都通过，chkimg OK。

---

## §222 WPA 回调补齐 AES 家族：CBC / CMAC(OMAC1) / GMAC（向量验证）

§218 里 AES 那六项还是"记账 + 返回 -1"。这一节把它们补成真实现：

| 回调 | 状态 | 验证向量 |
| --- | --- | --- |
| `aes_128_encrypt`（AES-128-CBC 原地加密） | ✅ | NIST SP 800-38A F.2.1 |
| `aes_128_decrypt`（AES-128-CBC 原地解密） | ✅ | 同上（还原原文） |
| `omac1_aes_128`（AES-CMAC） | ✅ | RFC 4493 四个用例（空/16/40/64 字节，覆盖填充分支） |
| `aes_gmac`（GCM 的空明文形式） | ✅ | GCM 规范的"全空"用例 |
| `ccmp_encrypt` / `ccmp_decrypt` | ⏳ 记账 + 返回失败 | 语义复杂（要按 802.11 头解析 PN/keyid 做 CCM），而 S3 的 MAC 有 CCMP 硬件卸载；真上板若 `0x50000BC0[6]/[7]` 非 0 再补（AES 块、CBC-MAC、CTR 都已就位） |

实现是自包含的普通 C（AES-128 加/解密 + CBC + CMAC + GHASH/GMAC，约 350 行），
`tests/wpa_crypto_selftest.c` 用宿主 gcc 直接编、直接跑向量：

```
make tls12check
  ========== wpa_crypto_selftest ==========
   ok   PSK 与 IEEE 802.11i 向量一致
   ok   SHA-256(abc) 与 NIST 向量一致
   ok   HMAC-SHA256 与 RFC 4231 向量一致
   ok   CBC 密文与 NIST F.2.1 一致 / CBC 解密还原
   ok   CMAC(空消息/16/40/64 字节) 与 RFC 4493 一致
   ok   GMAC 与 GCM 规范一致
  wpa_crypto C 实现自测通过（失败 0 项）
```

**坑**：加完 AES 之后 `https` 镜像 `IRAM overflowed by 392 bytes` —— 新代码默认
掉进 IRAM。`link.ld` 里把 `*wpa_crypto.o(.text…)` 也收进 `.flash.text`（它不在
中断路径上，放 flash 完全没问题），然后 IRAM 就回去了。

`https` / `wifi` / `wifip` / 基线 `s3` 四种构建都通过，chkimg OK。

---

## §223 分阶段打印 OSI 调用统计和事件环：把"关联卡在哪"变成一行日志

板子上没有断点，之前关联失败只会看到一句 `[wifi] 关联超时（15000 ms）`，
无法判断是"根本没扫到 AP"、"扫描后没去连"、"连了但 4 次握手失败"还是
"连上了但 DHCP 没拿到地址"。这一节把两个已有的记账装置接到串口上：

| 新增输出 | 内容 |
| --- | --- |
| `[osi] <阶段>: 13=1 18=1 23=2 29=5 37=2 43=12 …` | `g_osi_calls[]` 里**非零的槽位**（最多 16 项）。槽位含义见 `include/c6os/wifi_osi.h`：13 `_semphr_create`、18 `_mutex_create`、23 `_queue_create`、29 `_queue_recv`、37 `_task_create`、43 `_malloc`、83 `_log_write`… |
| `[evt] <阶段>: 共 N 条 [wifi:2] [wifi:1] [wifi:4] …` | RTC `0x50000B90` 里记的事件：`1=SCAN_DONE 2=STA_START 4=STA_CONNECTED 5=STA_DISCONNECTED`（§216 的记账） |

打印时机：`esp_wifi_init_internal` 之后、`start+scan` 之后、关联成功之后。

判读方法（上板直接用）：

| 现象 | 结论 |
| --- | --- |
| `[evt]` 里有 `[wifi:2]`（STA_START）但没有 `[wifi:1]`（SCAN_DONE） | 扫描没完成 → 射频/时钟层还没通 |
| 有 `[wifi:1]` 但没有 `[wifi:4]`（STA_CONNECTED），之后出现 `[wifi:5]` | 关联被拒 → 看 `_log_write` 的内容（`g_log_level=4` 已经打开）或考虑 PSK/认证方式 |
| 有 `[wifi:4]` 但没有 `[lwip] ip=` | 关联成功但 DHCP 没成 → 看 `[osi]` 里有没有 `_queue_send`/`_task_create` 失败、以及 lwIP 的 netif 状态 |
| `[osi]` 里出现我们没实现的槽位（如 120 以上的兜底项）| 说明还有 OSI 回调要补 |

这一节不改任何行为，只是把已有的两个记账装置（`g_osi_calls`、事件环）暴露到串口。

`https` / `wifi` / `wifip` / 基线 `s3` 构建都通过，chkimg OK。

---

## §224 上板运行手册（板子回来照着做，一步一条命令）

### 0. 准备

- 板子插上 USB（供电 + USB-Serial-JTAG），`lsusb | grep 303a` 能看到设备；
- 给 WiFi 账号（**只有这两项需要人给**，见 `port/s3/wifi_creds.h`）。

### 1. 先验 PHY（不需要 WiFi 账号）

```
make C6_ROOT=/home/winger/c6 bringup
```

这一条会构建 `S3_PHY_BISECT=1` 的镜像、刷进去、抓 60s 串口、把日志落到
`build/logs/s3run-*.log`，最后按顺序打印关键标记出现了没有。

期望看到（依次）：

```
[bisect] done hung=0x00000000 ok=0x0000007e     ← 6 个子步骤全活着
[phy] g_phyFuns=0x3fcxxxxx                       ← 函数表填上了（§206）
[phy] register_chipv7_phy rc=0                   ← PHY 起来了
W1 bringup / W2 bringup returned
[wifi] scan start（阻塞） / scan_start=0
[wifi] AP0 ssid=<你家 AP> ch=N rssi=-xx          ← 射频链路 OK（§215）
```

- 如果 `hung` 非 0：按 §204 的位定义定位是哪一步；
- 如果只看到 `W0 phy start` 就没有下文：看 RTC `0x50000A10` 的阶段信标（§204）；
- 如果扫描 `num=0`：射频还没通，先别管关联。

### 2. 再验整条链（需要 WiFi 账号）

```
make C6_ROOT=/home/winger/c6 httpsflash \
     S3_WIFI_SSID='"你家的AP"' S3_WIFI_PASS='"口令"'
```

期望看到：

```
[wifi] set_storage(RAM)=0 / set_mode(STA)=0 / set_config=0 / start=0
[wifi] set_ps(NONE)=0
[wifi] 已关联，ssid=... channel=N                ← 4 次握手过了（§215/§218）
[evt] 关联成功: [wifi:2] [wifi:1] [wifi:4] ...    ← STA_START/SCAN_DONE/CONNECTED（§223）
[lwip] ip=192.168.x.y mask=255.255.255.0 gw=...   ← DHCP 成功（§210）
[https] TLS 握手完成（阶段=21）                    ← TLS 1.2（§213）
[https] 第一跳 HTTP 状态 302 …                     ← 取到 https://qq.com（§214）
```

跑到最后一行就等于目标达成（真板上取到 qq.com 的页面数据）。

### 3. 卡住时的对照表

| 最后一行 | 看哪里 |
| --- | --- |
| `[ctx] 栈池不够` | §219：调 `S3_STACK_POOL` 或压小某个任务的栈 |
| `[wifi] 关联超时` + `[evt]` 里没有 `[wifi:4]` | §223：关联被拒，看 `_log_write` 内容（`g_log_level=4` 已开） |
| `[lwip] DHCP 超时` | §220：收包路径；`[osi]` 里看 `_queue_send`/`_task_create` 是否正常 |
| `[https] TLS 缓冲申请失败` | §214：OSI 堆（64KB）被 WiFi+lwIP 占满，调小 `TCP_WND`/`TCP_SND_BUF` |
| `[https] DNS 解析 qq.com 失败` | DNS 服务器（DHCP 下发）或 lwIP DNS 配置 |
| `[https] TLS 握手失败，阶段=N` | 阶段含义见 §213（1/2/11/12/20/21） |
| 栈相关崩溃 / 莫名复位 | §214 第五节：RSA 验签峰值栈约 3.5KB，任务栈要够 |

---

## §225 OSI 表覆盖审计：82/120 已接实现，剩下 38 个逐个说明为什么可以空着

用新工具 `tools/osi_audit.py` 把 `s3_wifi_osi_init()` 的 `OSI_SET(...)` 与
`include/c6os/wifi_osi.h` 的字段顺序对齐（§203 的 `osislots.py` 是"库调了哪些"，
这个是"我们接了哪些"，正好互补）：

```
$ tools/osi_audit.py
OSI 槽位 120 个：已接实现 82，兜底空实现 38
```

### 这一轮补上的两个（原来确实是漏洞）

| 槽位 | 现在 | 为什么必须补 |
| --- | --- | --- |
| 39 `_task_delay` | → `s3_w2c_task_delay` | 实现函数一直存在，**但从来没 `OSI_SET` 过** —— 库调它拿到的是"立即返回"，所有"等一会儿再重试"都变成空转（我们调度是抢占的所以不会彻底卡死，但时序全乱） |
| 64/65 `_wifi_rtc_enable_iso` / `_disable_iso` | 置/清 `RTC_CNTL_DIG_ISO(0x60008094)` 的 `WIFI_FORCE_ISO(bit28)` | 和 IDF 的 `esp_wifi_rtc_enable_iso()` 完全一致；`wifi_hw_start/stop`、MAC sleep/wake 都会调 |

### 剩下 38 个空实现，分四类（都有理由）

| 类别 | 槽位 | 理由 |
| --- | --- | --- |
| 静态统计证明**库从不直接调用** | 52~54 `_phy_*`、56~60 `_timer_*`、61 `_wifi_reset_mac`、62/63 `_wifi_clock_*` | `tools/osislots.py` 全库反汇编统计里一次都没出现；PHY 由我们自己按 IDF 顺序显式初始化（§205/§206） |
| 返回 0 就是**正确答案** | 96~118 `_coex_*` | 我们的 BT 是关的，"没有 BT 活动 / 无共存需求" 就是 0 |
| 纯性能/时钟优化，空实现无害 | 48~51 `_dport_access_stall_*`、`_wifi_apb80m_*` | IDF 用它们优化 APB 访问与 80MHz 时钟，缺了只是慢一点 |
| 事件系统没接（有意） | 45 已接（记账）、其余事件相关走队列/信号量 | 我们不注册事件处理函数，"关联成功"靠轮询 `esp_wifi_sta_get_ap_info()`（§215） |

以后任何新镜像上板前都可以先跑一次 `tools/osi_audit.py`，确认没有"本该接却漏接"
的槽位 —— §218 那个 `wpa_crypto_funcs` 全 NULL 的教训就是漏接的代价。

`https` / `wifi` / `wifip` / 基线 `s3` 四种构建都通过，chkimg OK。

---

## §226 `_wifi_int_disable/_restore` 改成真的关中断

IDF 的 OSI 表里这两个槽位就是：

```c
._wifi_int_disable = (void *)portENTER_CRITICAL,   /* 返回中断状态 */
._wifi_int_restore = (void *)portEXIT_CRITICAL,
```

我们之前是"记账 + 什么也不做"：库以为自己在临界区里，实际上并发完全没保护 ——
队列/链表可能被中断和任务同时改（这类 bug 在真板上表现为随机的丢包、
`_queue_recv` 返回垃圾、甚至断言失败，极难查）。

现在接上内核的：

```
port_irq_save   : rsil a2, 15     （关中断，返回原 PS）
port_irq_restore: wsr ps, a2      （恢复）
```

经 `port/s3/w2c.S` 的 `s3_w2c_irq_save` / `s3_w2c_irq_restore` 两个壳调用。

**风险与判据**：如果库里在"临界区"内阻塞（等信号量），关中断会让 tick 进不来、
直接死锁。IDF 用同一套原语跑同一个库，所以按设计不会这样用；万真板上出现
"关联时卡死不返回"，把这两个槽位改回空实现即可验证是不是这个原因
（RTC `0x50000B40[2]/[3]` 记录调用次数，能看出库到底用不用）。

`https` / `wifi` / `wifip` / 基线 `s3` 四种构建都通过，chkimg OK。

---

## §227 宿主上验证 lwIP 的线程层（sys_arch）：`make lwipcheck`

板子上真正"没被验证过"的软件层只剩两块：闭源 WiFi/PHY（要硬件）和 **lwIP 的
线程层**。后者其实不需要硬件 —— 把 `s3_w2c_*` 这套内核原语用 pthread 实现出来，
`port/s3/lwip/sys_arch.c`（板子上跑的那份，一行不改）就能在宿主上跑通。

### 新增

| 文件 | 内容 |
| --- | --- |
| `port/s3/lwip/host/arch_stub.c` | 用 pthread 实现 `s3_w2c_sem_*` / `s3_w2c_mutex_*` / `s3_w2c_queue_*` / `s3_w2c_task_*` / `s3_w2c_tick_*`（timeout 单位是内核 tick，这里按 10ms 折算），外加 `sys_now`、`c6lwip_diag/assert/rand` |
| `tests/sys_arch_host_test.c` | 18 项检查：邮箱按序投递/`tryfetch`/超时（空邮箱 fetch(200ms) 必须返回 `SYS_ARCH_TIMEOUT` 且耗时约 200ms）、信号量超时与跨线程 signal、`sys_thread_new` 真起线程、互斥量、`sys_now`/`sys_msleep` |
| `make lwipcheck` | 用宿主 `cc` 编 lwIP 全量源码 + `sys_arch.c` + stub + 用例，直接跑 |

实测：

```
$ make lwipcheck
== 邮箱：投递 3 条、按序取出、tryfetch
   ok   sys_mbox_new / 第一条是 1 / tryfetch 拿到 2 / 第三条是 3
== 邮箱超时：空邮箱 fetch(200ms)
   ok   返回 SYS_ARCH_TIMEOUT / 耗时约 200ms（允许调度误差）
== 信号量：wait 超时 / 跨线程 signal
   ok   空信号量 wait 超时 / 耗时约 200ms
== 跨线程：sys_thread_new 起的线程投递邮箱并 signal
   ok   sys_thread_new / 从工作线程收到消息 / 被工作线程唤醒 / 工作线程确实跑了
== 互斥量  ok   lock/unlock 两次没死锁
== sys_now  ok   不回退 / sys_msleep(30) 后前进约 30ms
sys_arch 自测通过（失败 0 项）
```

### 踩到的一个配置坑（记下来）

宿主编译 lwIP 时不能用 `#define LWIP_PROVIDE_ERRNO 0` —— lwIP 的 `lwip/errno.h`
用的是 `#ifdef`（只看有没有定义），定义成 0 一样会走"自己提供 errno"分支，
于是和 glibc 的 TLS `errno` 打架（链接期 `TLS definition ... mismatches`）。
正确做法是宿主上**不定义**它，并且定义 `LWIP_ERRNO_INCLUDE <errno.h>` 让 lwIP
把系统的 errno 头拉进来；`struct timeval` 用 `LWIP_TIMEVAL_PRIVATE 0` 避免重复定义。

### 现在的验证矩阵

| 层 | 验证方式 | 状态 |
| --- | --- | --- |
| AES-GCM / PRF / RSA / WPA crypto | 宿主向量（NIST/RFC/IEEE/openssl） | ✅ |
| TLS 1.2 + HTTP（含跟随 302） | 宿主真连 qq.com / www.qq.com | ✅ |
| lwIP 线程层（sys_arch + tcpip 邮箱/信号量/超时） | `make lwipcheck` | ✅ |
| 内核原语（sem/mutex/queue/task/tick） | 内核自带测试 + 基线构建 | ✅ |
| **WiFi PHY / 关联 / DHCP（闭源库）** | **必须真板** | ⛔ |

## §228 真板恢复供电后的第一轮实测（2026-11-16）

板子重新上电后（之前是没接电源），USB 枚举一开始失败（`dmesg`: `device descriptor
read/64, error -71` / `unable to enumerate`），按住 BOOT 再复位进下载模式后正常
（`303a:1001`，SerialNumber `CC:8D:A2:EC:99:C4`）。**以后刷机前先确认能枚举。**

### 一、这一轮修掉的三个真 bug

| # | 现象 | 根因 | 修法 |
| --- | --- | --- | --- |
| 1 | 只打印 `c6os s3 boot` 就没了，USB 也跟着消失 | `os_main` 里先调 `os_wdt_start(8)` 再 `os_icache_enable()`，装完自愈狗就卡死 | 调换顺序：先 cache + flash 诊断，再装自愈狗；加 `-DS3_NO_WDT` 开关 |
| 2 | `[wifimain] id=1` 之后立刻 `TRAP cause=0 pc=0x8037a7e9`（a0 带窗口标签） | windowed 岛（`s3_win_wifi_main`）里直接调 **call0** 的 `os_wdt_feed()` | 新增 w2c 壳 `s3_w2c_wdt_feed/start`，7 处调用全改走壳 |
| 3 | `esp_wifi_init_internal` 返回 **258 (ESP_ERR_INVALID_ARG)**，库日志：`wpa crypto funcs expected size=0 version=0, actual size=0 version=0` | `wcfg()` 收的是**字节偏移**，我把 11 个字写成了 `wcfg(4u + k, …)`，整张表挤进 4..14 字节 | 改成 `wcfg(4u + k * 4u, …)` |

修完 1、2 之后串口能看到：`W0 phy start` → `[phy] g_phyFuns=0x3fcef3d8` → 但
`register_chipv7_phy` 会卡死（`set_chan_freq_hw_init` 里 RF 等待不返回，一次还
`TRAP cause=0` 在它的 `retw.n` 上）。

### 二、PHY 使能改回 IDF 的语义（关键认知修正）

IDF 的 `esp_adapter.c` 里：`._phy_enable = esp_phy_enable_wrapper`（→
`esp_phy_enable(PHY_MODEM_WIFI)`）。也就是说 **PHY 使能是 WiFi 库在合适时机回调
我们做的**，不是我们提前手动做。现在：

- OSI 的 `_phy_enable` / `_phy_disable` 接到 `s3_win_phy_enable_hook()`；
- `make S3_PHY_VIA_OSI=1` 时 bringup 之前不再手动初始化 PHY。

改完之后 `esp_wifi_init_internal` 才第一次真正跑起来（之前它根本没被调到就死在
PHY 里了）。

### 三、当前进展与卡点（下一步从这里接）

现在串口能看到：

```
c6os s3 boot
M1 icache ok / flash: ... / M2 flash_diag ok / M3 wdt skipped
M5 tasks ok, starting scheduler
[wifimain] id=1 tick=0
W0 phy start
[phy] 跳过手动初始化：交给 OSI _phy_enable 回调
W1 bringup
B1 slot13=… magic=deadbeaf ver=00000008
B2 cfg@… osi=… magic@144=1f2f3f4f
[pp] pp rom version: e7ae62f
[net80211] net80211 rom version: e7ae62f
[task] 库建任务 fn=403813fc id=2 stack=6144 prio=29(->2)     ← ppTask 起来了
[take] t=1 sem=3fcc4964 tk=ffffffff                          ← init 等 ppTask 握手（拿到）
[recv] t=2 qh=3fcc42d4 tk=ffffffff                           ← ppTask 正常进收包循环
[take] t=1 sem=3fcc4994 tk=ffffffff                          ← **init 卡在这里**
```

**卡点**：`esp_wifi_init_internal` 里的 init 任务（t=1）在等第二个信号量
（3fcc4994），而没有任何人去 signal 它，且从日志看库只创建了**一个**任务（ppTask）。
下一步很可能是：那个"事件/协议栈任务"没被创建出来（我们的 `_task_create` 只被调了
一次），要查库为什么没建第二个任务 —— 大概率还是某个 OSI 返回值让它以为失败。
`[task]` / `[take]` / `[evwait]` / `[recv]` 这些打印是这一轮加的，会一直留在代码里
（它们是定位这类"卡在某个同步点"的唯一手段）。

### §229 同步点追踪：init 卡在 ppTask 握手之后（两个任务同时失声）

给 OSI 的信号量/互斥/队列加了序号化追踪（`[semnew] / [mtxnew] / [take] / [give] /
[lock] / [unlock] / [send] / [recv]`，只在内核 bringup 期间打开，`g_dbg_sync`）。
真板串口现在的完整流程：

```
W1 bringup
B1 … magic=deadbeaf ver=00000008
B2 cfg@… osi=… magic@144=1f2f3f4f
[lock]   t=1 m#… p=…                  ← esp_wifi_init_internal 拿锁
[pp] pp rom version: e7ae62f
[net80211] net80211 rom version: e7ae62f
[unlock] t=1 m#… p=…
[mtxnew] #1 / [mtxnew] #2             ← 建两个互斥
[semnew] #1 p=… max=1 init=0 t=1      ← 建握手信号量
[task]   库建任务 fn=… id=2 stack=6144 prio=29(->2)   ← ppTask 起来了
[take]   t=1 sem#1 tk=ffffffff        ← init 等 ppTask 就绪
[give]   t=2 sem#1                    ← ppTask 给了 ✓
（之后**两个任务都不再有动静**）
```

也就是：`pm_funcs_init / wdev_funcs_init / net80211_funcs_init` 都过了，ppTask 建
起来并完成了启动握手；**紧接着 init 任务（t=1）和 ppTask（t=2）同时失去进展**，
而且没有 TRAP、没有任何后续 OSI 调用（`[send]/[recv]/[lock]` 都没出现）。

这几乎可以断定问题在**上下文切换 / 窗口保存恢复**这一层（之前几次 `TRAP cause=0`
且 `pc` 落在 `retw.n`、`a0` 是垃圾值，正是窗口被子被踩的症状）：
- 下一步①在 `port_switch` 里加打印（切换前后任务 id + WINDOWSTART），确认 give 之后
  是否发生了任务切换、切换后窗口状态是否正确；
- 下一步②检查 `ctx.S` 的任务切换对 **windowed 任务**是否正确地保存/恢复了
  a0-a15 与 WINDOWSTART（我们只验证过 call0 任务，windowed 任务是实验性的）。

### §230 JTAG 现在能用了 + 当前故障的精确定位

**重要工具进展**：板子正常运行（USB CDC 工作）时，`tools/dbg.sh` 的 OpenOCD+JTAG
能附着（之前失败是因为当时 USB 控制传输坏了）。于是可以直接：

```
./tools/dbg.sh build/wifi/c6os.elf /tmp/halt.gdb     # monitor halt + info registers pc
```

`_vec_default` 会把未处理异常的现场写进 RTC（`+0x100`=0xBAD0 标记、`+0x104`=EXCCAUSE、
`+0x108`=EXCVADDR、`+0x10C`=EPC1），用 gdb `x/4xw 0x50000100` 直接读，不用复位。

本次实测读到：

```
EXCCAUSE = 3          (LoadStoreError)
EXCVADDR = 0x40043ca4 (ROM 里的地址 —— 就是说"往 ROM 写"或"用 ROM 当地址"了)
EPC1     = 0x42014415 (s3_w2c_current_task 中间，w2c 壳里)
```

结合 halt 时 `a1=0x3fcc0430`（落在 ppTask 的 6KB 栈内）、`windowbase=13`、
`windowstart=0xa002`（明显是坏的窗口状态），可以确认：

**故障点 = w2c 壳（windowed → call0 的跳板）在 ppTask 上下文里用了被踩坏的
栈指针/窗口状态**。这和之前 PHY 里 `set_chan_freq_hw_init` 的 `retw.n` TRAP 是同一
类问题（windowed ABI 与 call0 内核交界处的窗口簿记）。

已经做的两个对照实验（都还没解决）：

| 实验 | 结果 |
| --- | --- |
| `S3_NO_WIN_SPILL=1`（壳里不 spill 祖先窗口） | 比开着 spill 多走一步（ppTask 会回到 `_queue_recv`），但随后 init 任务（t=1）无声卡死 |
| `ctx.S` 里改成**每个任务保存/恢复 WINDOWSTART**（帧偏移 68，替代"恢复时归一化"） | 没有改变症状（说明归一化不是这次的主因） |

下一步方向（按代价从低到高）：

1. **把 w2c 壳改成不含任何窗口操作的版本**：`entry` → 存 a0 到帧 → `callx0` →
   从帧取回 a0 → `retw`（把 WIN_SPILL 与 128 字节大帧都去掉），再用 JTAG 看是否还炸。
   如果好了，就说明 WIN_SPILL 的偏移/假设有问题（那套东西从没在真板上验证过）。
2. 若 1 不行：把 OSI 回调里"需要调内核"的那几个（sem/mutex/queue/task/tick）
   改成**纯汇编手写**的窗口安全版本，逐个替换、逐个用 JTAG 验证。
3. 或者换思路：让内核原语也编成 **windowed**（同一份 .c6 用 `-mabi=windowed` 编到
   一个单独的库里给 windowed 侧用），彻底消除 windowed→call0 的边界。

### §231 两个方向性实验（都做了，结论明确）

#### 实验 A：极简 W2C 壳（`S3_W2C_MIN=1`）

把 w2c 壳从"entry 128 + WIN_SPILL + 64 字节死区 + callx0"简化成
"entry 32 + 存 a0 + callx0 + 取回 a0 + retw"，不做任何窗口操作。

- 结果：比带 spill 的版本**多走一步**（ppTask 能回到 `_queue_recv` 循环），
  但 init 任务（t=1）随后仍然无声卡住；JTAG 读到的异常现场是
  `EXCCAUSE=29 (StoreProhibited)`、`EXCVADDR=0x56`（近似 NULL 解引用）、
  `EPC1=0x420179f3`（在 `os_task_start_core` 里）。
- 说明：w2c 壳不是唯一问题；**windowed 库和 call0 内核这个拼接方式本身**在深调用
  下会互相踩。这条路可能永远需要"逐个壳调参"，风险高。

#### 实验 B：整个镜像用 windowed ABI 编译（`make ... S3_ABI=windowed`）

既然内核/闭源库一个是 call0、一个是 windowed，最彻底的办法是**让内核也用
windowed**（我们的 port 本来就写了 `#ifdef __XTENSA_CALL0_ABI__` 的两条路）。

- **编译链接一次通过**：`Successfully created ESP32-S3 image` + `chkimg OK`
  （IRAM/IROM/DRAM 都还在预算内）。
- 但真板上只打印 `entry 0x4037853c` 就没了 —— 连 `c6os s3 boot` 都没到，说明死在
  `start.S` 的 windowed 入口（窗口初始化/WINDOWSTART 归一化那段、
  以及 `_start` 之后第一条 `entry` 的窗口状态）。
- 这不是坏消息：说明这条路**有希望但要改 port 的启动代码**（把 windowed 路径补完整：
  正确的 `entry` 帧、WINDOWSTART 初始化、以及 ctx.S/irq.S 在 windowed 下的
  上下文保存）。

#### 结论与建议的下一步优先级

| 优先级 | 方向 | 理由 |
| --- | --- | --- |
| 1 | **把 windowed ABI 这条路打通**（改 start.S 的 windowed 入口 + ctx.S 的上下文保存） | 一次投入换来"没有 ABI 边界"，之后再也不用调 w2c 壳/窗口 spill；而且内核与闭源库同 ABI 正是 IDF 的做法（FreeRTOS 也是 windowed） |
| 2 | 继续调 call0+壳 这条路 | 现在能走到 `esp_wifi_init_internal` 的 ppTask 握手之后，但每次都用真板 + JTAG 定位，成本高、且每次只前进一点点 |

无论走哪条，**工具已经齐了**：`make ... https` 出镜像、`tools/flashmon.sh` 刷+抓、
`tools/dbg.sh` + gdb 做 JTAG 断点/读内存、`_vec_default` 的 RTC 现场、以及
`[task]/[take]/[give]/[recv]/[lock]` 同步点追踪。

### §232 windowed 构建到底死在 port_init 的哪儿（真板 + JTAG 实测）

`S3_ABI=windowed` 的镜像刷进去后：

- RTC `0x50000000 = 0xa1` → `_start` 跑完、**.bss 清完了**；
- RTC STORE7 `0x600080CC = 0xb0b0b0b0` → **进了 `port_init`**（那是它的第一个信标）；
- 然后 TRAP 进 `_vec_default`（halt 时 pc 在 `_vec_default` 的自旋里，
  a0=0x80379c15（带窗口标签），windowbase=9 / windowstart=0x2a0）。

也就是说 windowed 路并不像表面那样"死在 _start"：`_start → call8 port_init` 是通的，
死在 `port_init` 的第一批动作里（`os_flash_init()` / `os_icache_enable()` 一带 ——
那里要经 `s3_win_call_config_icache_mode` 进 ROM 的 windowed 函数）。

要把这条路走通，需要补的恰好是"windowed 内核"的几处基础设施：

1. `port_init` 里各个 `call8` 进来的函数，其 `entry` 帧与 WINDOWSTART 状态要自洽
   （我们的 `_start` 已把 WINDOWSTART 归一化成 1<<windowbase，但 `call8` 之后
   window 轮转由硬件做，溢出/欠载向量要不要在 windowed 内核里也参与，需要确认）；
2. `ctx.S` 的任务上下文在新 ABI 下的保存/恢复（a0..a15 + WINDOWSTART + EPC/PS）；
3. `irq.S` 的异常入口：windowed 下 `_vec_level1` 的现场保存/`rfe` 前要不要做
   `rotw`/`wsr windowstart`。

工作量不小（相当于把 port 的窗口语义整体捋一遍），但收益是"内核与闭源库同 ABI"，
后面所有 OSI 回调都不再有边界问题。

**决策（本轮）**：先守在 call0 这条能跑的线上（已经推进到
`esp_wifi_init_internal` 内部、ppTask 握手成功），把 windowed 重构留作专项。

### §233 用 JTAG 看两个任务和"正在跑的那段代码"：

命令：`./tools/dbg.sh build/wifi/c6os.elf <脚本>`（脚本里 `monitor halt` + `x/…`）。
本轮拿到的关键事实：

1. **两个任务都是正常阻塞的，不是崩的**：读 `g_ctx[i].sp` 得到各任务的陷阱帧，
   `+60` 的 EPC 都是 `0x420166e4`（就是 `port_task_switch` 里的切换点），
   PS 分别是 `0x00060900` / `0x00060100`。也就是说
   - 任务1（init，8KB 栈）：在等某个同步对象；
   - 任务2（ppTask，6KB 栈）：在 `_queue_recv` 里等消息（正常）。
2. **但"正在跑的那段内核代码"的栈指针是坏的**：halt 时 `pc` 在 `_vec_default`
   的自旋里，`a1 = 0x3fcb95f2`——**奇数**，而且落在 `g_stack_pool`（0x3fcbcd20 起）
   **下方**的 .bss/heap 区里，不可能是合法栈。两个任务帧里也都能看到
   `0x3fcb95f2 / 0x3fcb9582` 这串值。
3. 异常现场：`cause=29 (StoreProhibited)`、`excvaddr=0x56`、
   `EPC1=0x420179f3`（在 `os_task_start_core` 的调度循环里）。

**结论**：两个 windowed 任务之间切换之后，**内核自己的栈指针被写坏了**
（a1 变成 0x3fcb95f2 这种值），于是调度/空转路径下一次压栈就炸。
问题还在上下文切换/中断入口这一层（`ctx.S` + `irq.S`），而不是 WiFi 库。

下一步的具体做法（按顺序）：

1. 在 `port_task_switch` 里加"RTC 环 + 校验"：切换前后把 a1、WINDOWSTART、
   正在切换的 from/to 任务 id 写进 RTC（0x50001D00 起），并检查 a1 是否落在
   合法栈范围内（`g_stack_pool .. g_stack_pool+24576`），一旦越界就把
   "越界的那次切换"记下来（from/to/旧a1/新a1）——这是唯一能把"谁写坏了 a1"
   钉死的办法；
2. 重点看 `_vec_level1`（定时器中断）在 windowed 任务里嵌套时的帧：
   中断入口保存 a0..a15/EPC/PS 用的是 80 字节帧，而任务切换也用同一种帧，
   两者嵌套时 `S3Ctx.sp` 指向的应该是中断帧；如果中断帧和任务帧混了，
   `a1` 就会被写成帧里的某个值（正好能解释 0x3fcb95f2 这种"像栈又不像栈"的数）；
3. 另一个可疑点：`RESTORE_FRAME` 里恢复 PS 时用 `ps | 0x10`（清 EXCM?），
   以及 `_vec_level1` 是否在 `rfe` 前恢复 `windowstart` —— windowed 任务被
   中断打断再恢复，窗口状态必须逐个核对。

### §234 根因判断：**windowed 任务被抢占切换时，活着的祖先窗口没有 spill**

把 §233 的观测和 Xtensa 的窗口机制对上，根因基本可以确定：

- Xtensa 只有 **4 个窗口**（a0-a15 的物理寄存器组按 WINDOWBASE 轮转）。
  一个 windowed 调用链处在深度 N（N≤3）时，它的**祖先窗口仍在寄存器里**，
  只有当溢出向量被触发过才会落到栈上。
- 我们的 `port_task_switch` 只保存**当前窗口**的 a0..a15（80 字节帧），
  祖先窗口一律不管 —— 注释里写的是"切走前 w2c 的壳一定已经把祖先 spill 了"，
  但那只对**主动**经壳调内核的路径成立；对**定时器抢占**这条路径不成立。
- 于是：windowed 任务在深度 ≥1 时被 tick 抢占 → 切走 → 另一个任务/内核代码
  轮转窗口 → 回来时它的祖先窗口已经被覆盖 ⇒ `retw`/`retw.n` 用错窗口的 a0
  ⇒ 就是我们反复看到的 `cause=0` / `cause=29` / `a0` 是垃圾 / `a1` 变成
  `0x3fcb95f2` 这种值的来源。

标准做法（Espressif 的 FreeRTOS 移植层同样如此）：
**在切走之前（还在被打断任务的栈上）把活着的窗口整圈 spill 到栈上，并把
WINDOWSTART 归一化成 `1<<WINDOWBASE`**，这样寄存器文件里不留任何任务私有状态。

在我们这套 port 里要落地，需要：

1. `_vec_level1`（trap/中断入口）在建立陷阱帧之后、调用 C 处理函数之前，
   用**被打断现场的 a1**（陷阱帧里保存的那个）做一次 spill；
   注意不能直接用当前 a1（那时已经是内核 C 帧了）；
2. spill 完把归一化后的 WINDOWSTART 也存进陷阱帧，`RESTORE_FRAME` 恢复时用它；
3. 对 windowed 任务，`port_task_switch` 里的"保存 a0..a15"必须在 spill 之后做
   （否则保存的是被覆盖前的当前窗口，而祖先已经不在寄存器里了）。

实现要点（备忘）：`w2c.S` 里已经有 `WIN_SPILL` 宏（按 a1 链把祖先窗口写到
各自帧的下方）。可以把它抽成可复用的宏，参数化成"用哪个 a1 作为链头"。
真板验证顺序：只跑一个 windowed 任务（应当已经能跑）→ 两个 windowed 任务互相
抢占（`s3_win_stress`）→ 再跑 WiFi。

### §235 关掉抢占也没用；故障点钉在 `WIN_SPILL` 的窗口轮转上

新增开关 `S3_NO_PREEMPT=1`（`s3_timer_irq` 不再做定时抢占切换，只在显式阻塞点
切换）。真板实测：**还是同样的故障**：

```
[task] 库建任务 fn=… id=2 stack=6144 prio=29(->2)
[take] t=1 sem#1 tk=ffffffff
[give] t=2 sem#1                        ← ppTask 给了信号量
（之后无进展）
```

JTAG 现场（这次的 ppTask 栈在 0x3fcc0430）：

```
EXCCAUSE = 3   (LoadStoreError)
EXCVADDR = 0x40043ca4   ← ROM 里的地址（把 ROM 当地址写了）
EPC1     = 0x42014415   ← s3_w2c_current_task 里，也就是 w2c 壳的 WIN_SPILL 段
```

也就是说：**故障发生在 `WIN_SPILL` 里**。看 `port/s3/w2c.S` 的实现就明白了：
它并不是"按 a1 链走"，而是走

```
rsr ps → 置 PS.WOE=1, EXCM=0, INTLEVEL=7
rotw 3 / rotw 3 / rotw 3 ……（转 16 个窗口一整圈）
```

靠**轮转触发硬件窗口溢出向量**把每个窗口 spill 到它自己的栈帧上（这正是
`xthal_window_spill` 的做法）。这条路径要求"寄存器文件里每个窗口的 a1 都是合法
栈指针"—— 而真板实测有窗口的 a1 是 `0x40043ca4` 这种 ROM 地址，于是溢出向量
按那个地址 spill 就 LoadStoreError。

`0x40043ca4` 是 ROM 里的地址、`a0=0x40043cc4` 也是 ROM —— 说明当时寄存器文件里
还残留着**进入我们代码之前（ROM/bootloader 阶段）的窗口状态**，或者某个 ROM
函数留下的窗口没有被正确"归位"。§202 里 `start.S` 已经做过一次 WINDOWSTART 归一，
但那只保证"哪个窗口算 live"，并不能清掉窗口**寄存器内容**（a0..a15 仍是 ROM 的
残值）。

下一步（明确、可执行）：

1. 在 `start.S` 归一化 WINDOWSTART 之后，**把寄存器文件清零**（连续 `rotw` 一圈，
   每个窗口把 a0..a15 写 0，最后转回原窗口），保证没有任何 ROM 残留进入内核；
2. 或者改成"不用 rotw 轮转"的 spill：直接按 a1 链手工 spill（`entry` 帧里
   调用者的 a1 在 `[frame+4]`，call8 窗口要写 4 个寄存器到 `caller_a1-16`，
   call12 写 12 个到 `caller_a1-48`），完全不依赖硬件溢出向量，也就不受
   寄存器文件里其它窗口内容的影响 —— 这条路更可控，建议优先。

### §236 试过"窗口预热"，无效（但保留，作为保险）

在 `start.S` 里加了一段 4 层嵌套 `call8`（每层 `entry a1,32`）的"窗口预热"：
目的是把 4 个窗口的 a1 都变成合法栈指针，这样 §235 里
`WIN_SPILL` 用 `rotw` 转一圈时就不会踩到 ROM 残留的垃圾窗口。

真板实测：**症状不变**（`[give] t=2 sem#1` 之后无进展）。说明：
- 要么垃圾窗口不是从 ROM 残留来的，而是运行期被写坏的；
- 要么故障不在 spill 的 rotw 上（`EPC1=0x42014415` 只是"最后一条被记录的" EPC）。

这段预热代码保留（无害、且对"ROM 残留"这类情况确实是必要的保险），
但问题显然更深：下一步建议直接用**手工 a1 链 spill** 替换 `rotw` 轮转
（不依赖硬件溢出向量，见 §235 第 2 条），并且给 w2c 壳加"spill 前校验 a1
是否落在 g_stack_pool 范围内，越界就只记录不写"的保护，避免一次坏 a1 直接把
系统打死 —— 先让它能继续跑，再看真正坏在哪里。

### §237 **决定性结论**：windowed 任务支持本身就是坏的（与 WiFi 库无关）

用现成的 windowed ABI 压力用例（`tests/t_winabi.c6`，两个 windowed 任务互相抢占 +
`_queue_recv` 最小复现）刷进真板：

```
make C6_ROOT=/home/winger/c6 S3_WIFI_MAIN=tests/t_winabi.c6 wifi
c6os s3 boot
[recv] t=2 qh=3fcc0360 tk=0
*** TRAP
```

JTAG 读异常现场：`EXCCAUSE=0x14 (20)`、`EXCVADDR=0x00000000`、**`EPC1=0x00000000`**，
halt 时 `windowbase=13`、`windowstart=0x2a80`、`a1=0xfffffde0`。

也就是：**跳到地址 0 去取指** + 窗口寄存器状态完全错乱。这个用例完全不碰 WiFi 库，
所以可以确定：

> 我们的 port 对 **windowed 任务**（`g_next_task_windowed=1` 建出来的任务）的
> 上下文切换/窗口簿记是坏的；之前所有"WiFi 初始化卡住/TRAP"都是它的下游症状。
> call0 任务一切正常（基线镜像、几十个内核测试都过）。

这也解释了为什么 `S3_NO_WIN_SPILL`、`S3_NO_PREEMPT`、窗口预热这些"外面打补丁"的
尝试都只是把故障点挪一挪、并不解决。

**下一步（明确且自包含）**：只盯 `t_winabi` 这一个用例，把"两个 windowed 任务
互相抢占"调通。要检查的点（按怀疑度）：

1. `ctx.S` 的 `port_task_switch`：切走时只存当前窗口的 a0..a15；**windowed 任务的
   祖先窗口必须在切走前 spill**（§234），否则另一个任务轮转窗口就把它覆盖；
2. `RESTORE_FRAME` 恢复 `windowstart` 的方式（现在改成按任务保存恢复，§230）是否
   和"spill 后归属"一致；
3. `_vec_level1`（trap/中断入口）在 windowed 任务里嵌套时的帧与 `S3Ctx.sp` 的关系；
4. 新建 windowed 任务的初始帧（`port_task_ctx_create` 里的 `g_stacks`/顶帧布局 +
   `g_next_task_windowed` 的 ABI 标志）是否让任务第一次 `retw` 就落在正确窗口。

判据：`t_winabi` 能连续跑（`d[1]` 里的轮数持续增长、`d[0]==0xC0DE1234`、
`d[4]==0xC0DE5678`）且不 TRAP，再回来跑 WiFi 初始化。

### §238 又抓到两个真 bug（但 windowed 任务首跑仍然 TRAP 到 0）

1. **`PS.WOE` 之前只在 windowed 构建里置 1**（`start.S` 的 `#ifndef __XTENSA_CALL0_ABI__`
   分支）。可我们是 call0 内核 + windowed 闭源库：WOE=0 时 `call8`/`entry`/`retw`/`rotw`
   都不做窗口轮转（官方注释：silent noop），windowed 代码的返回地址/帧指针必然错。
   现在改成**无条件置 WOE=1**（call0 代码不受影响）。
2. **`s3_task_tramp` 把 ABI 标志寄存器 a4 当计数器用了**：
   ```
   l32r a3, .Lts_cnt ; l32i a4, a3, 0 ; addi a4,a4,1 ...
   beqz a4, 2f        ← 这里本意是判断"入口是不是 windowed"，但 a4 已经变成启动计数
   ```
   于是**所有任务都被当成 windowed**（call0 任务也会被 `callx8` 调进去，返回地址带
   CALLINC 标记）。现在诊断计数改用 a8/a9，a4 的 ABI 标志保住。

两个都是真 bug（已修）。但 `tests/t_winabi.c6`（两个 windowed 任务 + `_queue_recv`
最小复现）在真板上**仍然 TRAP**：`EXCCAUSE=0x14`、`EXCVADDR=0`、**`EPC1=0`**
（跳到地址 0 取指）、`windowbase=13`、`windowstart=0x2a80`、`a1=0xfffffde0`；
`d[0]` 还是 `0xFFFFFFFF`（压力任务压根没跑起来）⇒ **新建 windowed 任务的第一次
进入就崩**。

也就是说下一个要查的是"新建 windowed 任务的初始帧"：
`port_task_ctx_create` 里 `f[1]=entry / f[2]=arg / f[3]=flag / f[15]=s3_task_tramp /
f[16]=PS|WOE`，然后 `ctx.S` 的 `RESTORE_FRAME` 恢复并 `rfe` 进 `s3_task_tramp`。
`EPC1=0` 说明 `rfe` 之后取指地址是 0 —— 高度怀疑 **`RESTORE_FRAME` 里
`wsr epc1` 用的是帧偏移 60，而 `port_task_ctx_create` 写的是 `f[15]`（=偏移 60）✓
一致**；那么更可能是 `s3_task_tramp` 里 `callx8 a12` 时 a12（入口地址）已经不对，
或者 trampoline 自己的帧没有 `entry`（它是被 `rfe` 直接跳进来的，**没有 entry**，
却用 `retw`/`callx8` —— 这本身就不合法）。

### §239 最小窗口用例把问题切干净了：**单 windowed 任务 OK，两个就崩**

新增 `tests/t_win1.c6` + `s3_win_hello_task`（只打印 + 周期 delay 的最小 windowed
任务）和 `s3_w2c_sp_probe`（读当前 sp）。真板结果：

| 用例 | 结果 |
| --- | --- |
| **一个** windowed 任务 | ✅ 完美：`[w0] hello … sp=3fcbb250` 然后 `tick 4/8/12/…` 一直跑（说明：新建窗口任务、首次进入、w2c 壳、与 idle 之间来回切换都正常） |
| **两个** windowed 任务（一个 8KB prio2、一个 4KB prio3） | 两个都打印了 hello（`sp=3fcbb250` / `sp=3fcbc250`，说明**第一次切换成功**），随后 TRAP：`windowbase=11`、`windowstart=0xa00`、`a1=0x303530b2` —— 又是窗口状态错乱 |

顺带确认了一件重要的事：§230 把 `RESTORE_FRAME` 改成"按任务恢复 windowstart"
**和壳里的 spill 不配对**（两个任务时第二个任务启动就炸）；改回原来的
"归一化成 `1<<WINDOWBASE`"（配合壳在切走前 spill 祖先窗口）之后，两个任务都能
启动起来。所以设计上还是"spill + 归一化"这一对，别再动它。

**现在的故障窗口非常小**：两个 windowed 任务来回切几次之后窗口状态就坏。
下一步就在这个最小用例上做（比 WiFi 好调无数倍）：

1. 在 `port_task_switch` 里把每次切换的 from/to 任务 id、WINDOWSTART、WINDOWBASE、
   a1 写进 RTC 环（0x50001D00 起），复位后用 JTAG 读，看**第几次切换开始坏**；
2. 重点怀疑 `s3_w2c_task_delay` 这条壳：它 `entry a1,128` → spill（转窗口）→
   `addi a1,-64` → `callx0` 内核 → 内核里 `port_task_switch` 保存现场。
   切回来时 `RESTORE_FRAME` 归一化 WINDOWSTART 后 `rfe` 回到壳里的 `addi a1,a1,64`，
   接着 `l32i a0` + `retw` —— 这里 `retw` 需要"当前窗口的 a0 帧槽还在"，
   而归一化 + 欠载 fill 的顺序如果错了，`retw` 就会用错 a0（正是我们看到的症状）。

### §240 "按任务恢复 WINDOWSTART"实测更糟（已回退）

在最小两任务用例上对比三种组合：

| 组合 | 结果 |
| --- | --- |
| `RESTORE_FRAME` 归一化成 `1<<WINDOWBASE` + 壳里 spill（默认） | 两个任务都能启动、各自打印 hello，随后 TRAP |
| 按任务恢复保存的 WINDOWSTART + 壳里 spill | **只有第一个任务启动**（第二个压根没跑）——更糟 |
| 归一化 + 不 spill（`S3_NO_WIN_SPILL=1`） | 和默认一样（两个启动，随后 TRAP） |

所以维持默认（归一化 + spill）。分析上剩下的关键问题是：
**WINDOWBASE 是全局的**，两个任务共用同一组物理窗口；切走时任务 A 的基线是 B_A，
切回来时"归一化成 1<<当前base"用的是当时的 base（可能就是别的任务的），
刚恢复的 a0..a15 会落到错的物理窗口。

要真正解决，需要按"切走前把 A 的全部活窗口 spill 到 A 的栈上 + 记录 A 的 base；
切回来时先把 base 恢复成 B_A、再由欠载向量按 a1 链 fill"这一套完整语义来做，
而且**框架必须是 FreeRTOS 那种"只在统一调度点切换"**（我们的 `port_task_switch`
会被任意深度的内核调用链调到，这本身就很难保证窗口语义）。

务实的两条路（下一轮选一条推进）：

1. **把切换点收敛**：所有内核阻塞/让出都改成"回到统一调度点再切"（类似
   FreeRTOS 的 `_xt_context_save` 只在 dispatch 里跑）——工作量中等，但一劳永逸；
2. **绕开 windowed 任务**：让 kernel 里只跑 call0 任务，闭源库需要的"任务"用
   **同一个 windowed 任务**（ppTask）+ 我们只做"被动回调"（不再自己建 windowed
   任务）。若库只需要一个 windowed 任务，则根本不需要在两个 windowed 任务之间切换。

### §241 架构调整：**只让库自己的 ppTask 是 windowed 任务**

按 §240 的第 2 条路走了：`s3_win_call_wifi_main` 跳板（call0 → `callx8` 进
`s3_win_wifi_main`；目标在 flash 里，`call8` 的 128KB 位移够不着，所以用
`l32r a10, .Lwmain` + `callx8 a10`），测试用例加 `-DS3_WIFI_TASK_CALL0`：

```
g_next_task_windowed = 0;
os_task_create("wifi", wifi_call0_task, 0, 3u, 8192u);   /* call0 任务 */
   └─ wifi_call0_task → s3_win_call_wifi_main() → s3_win_wifi_main()（windowed）
```

这样系统里**只有一个 windowed 任务**（闭源库自己建的 ppTask），我们自己的任务都是
call0（call0 代码不轮转窗口），"两个 windowed 任务互相切换"那个还没解决的难题就被
绕开了。

真板结果（`make ... S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON -DS3_WIFI_TASK_CALL0" S3_PHY_VIA_OSI=1 https`）：

```
T1 call0 任务进入 windowed 主流程
[wifimain] id=1 tick=0
W0 phy start / [phy] 跳过手动初始化
W1 bringup / B1 magic=deadbeaf ver=8 / B2 osi=…
[lock] / [pp] / [net80211] / [unlock] / [mtxnew] #1 #2 / [semnew] #1
[task] 库建任务 fn=403815fc id=2 stack=6144 prio=29(->2)     ← ppTask 起来了
[take] t=1 sem#1 tk=ffffffff
[give] t=2 sem#1                                             ← 握手完成
（之后仍无进展：ppTask 给完信号量后没进它的 _queue_recv 循环）
```

即：架构调整成功（跳板/单 windowed 任务都验证了），但 ppTask 在"给完启动信号量之后"
仍然卡住。下一步就在这个最小状态上用 JTAG 抓 ppTask 的 PC（它现在只有一个 windowed
上下文，窗口问题已经排除，剩下的应该是某个 OSI 返回值/初始化没做完）。

### §242 **目前最好的组合**：call0 任务 + 单 windowed 任务 + **壳里不 spill**

```
make C6_ROOT=/home/winger/c6 S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON -DS3_WIFI_TASK_CALL0" \
     S3_PHY_VIA_OSI=1 S3_NO_WIN_SPILL=1 https
```

道理：我们的内核是 call0，**从不轮转窗口**，所以窗口文件里那些"祖先窗口"不会被我们
弄坏，壳里根本不需要 spill；反过来 spill 的 `rotw` 转圈会去碰那些从未初始化的窗口
（真板上就是那个 a1=0x40043ca4 的 ROM 残留窗口）→ 直接 LoadStoreError。
§241 的架构（只有库的 ppTask 是 windowed）又保证了"只有一个窗口上下文"。

真板实测（这是目前走到最远的一次）：

```
[task] 库建任务 fn=403815fc id=2 stack=6144 prio=29(->2)
[take] t=1 sem#1 tk=ffffffff
[give] t=2 sem#1
[recv] t=2 qh=3fcc4420 tk=ffffffff        ← ppTask 进了收包循环（第一次看到！）
*** TRAP sp=3fcbeb90 cause=0 pc=4037c2f7 ps=00060f10 ws=00000006 a0=3fcbebc0
```

新的故障点很具体：`pc=0x4037c2f7` 是 **`osi_semphr_take` 结尾的 `retw.n`**
（`mov a2,a3` 之后），`a0=0x3fcbebc0` 是本任务栈上的地址（不是代码地址），
`ws=0x06`（两个窗口 live，是真实调用链的状态）。也就是说：

> 在 `osi_semphr_take`（windowed 的 OSI 回调）里，它自己 `call8 sem_trace` 转了一次
> 窗口；`sem_trace` 返回后 `retw.n` 用的 a0 已经不是自己的返回地址了 —— **窗口映射
> 在"windowed 函数 → windowed 函数"的普通调用里就已经错位**。

这条线索把问题从"任务切换"缩小到了"**windowed 函数之间的普通 call8/retw**"。
下一步（很值得做，范围小）：

1. 写一个最小 windowed 调用链用例（`a()` 里 `call8 b()` 几次再 `retw`），在真板上
   验证"windowed→windowed 普通调用"是否本身就坏；如果坏，问题就在
   `start.S` 的 WOE/WINDOWSTART 初始状态或 `_win_ovf*`/`_win_unf*` 向量本身；
2. 对照 `irq.S` 里的溢出/欠载向量实现与 ISA 手册（每个窗口 4/8/12 个寄存器、
   `-16/-32/-48` 的偏移、`rfwo`/`rfwu`），特别是 `_win_unf4/8/12` 里
   `l32e a0, a5, -16` 这种写法对应的 a5 语义；
3. 顺带确认 `osi_semphr_take`/`sem_trace` 这种小函数能不能让编译器别生成
   `call8 sem_trace`（比如把 sem_trace 改成 `static inline` / 直接内联），
   先用"减少一次窗口轮转"的方式绕过去验证假设。

### §243 sem_trace 诊断本身会在真板上炸（已停用）

把 `sem_trace` 内联后故障换了位置（`cause=0x1c LoadProhibited, vaddr=0`，
`l32i.n a8, a6, 0` 里 a6=0），说明这段"往 RTC 环写诊断"的代码本身在 windowed
上下文里会踩到寄存器/地址问题（它是被 `call8` 调的、又要轮转窗口）。
把 `sem_trace` 改成空函数之后，故障回到 `osi_semphr_take/give` 结尾的 `retw.n`
（`pc=0x4037c280, a0=0x3fcbebc0` 栈地址、`ws=0x06`）——
**即"windowed 回调函数返回时 a0 不对"这个核心问题依旧**。

结论：诊断代码要写得极少（只用 `ets_printf`，不要额外函数调用），
核心问题仍然是 windowed 函数之间的 `call8`/`retw` 窗口配对。

### §244 真板 WiFi 账号已配置（不进版本库）

用户给了路由器：**SSID `weforpay.com`，密码 `weforpay`**。别在命令行上跟引号搏斗
（`make S3_WIFI_SSID='"x"'` 经过 shell 会掉引号，编译期报 `'x' undeclared`），
改成走本地头文件：

```
port/s3/wifi_creds_local.h          # 已加进 .gitignore
    #define S3_WIFI_SSID "weforpay.com"
    #define S3_WIFI_PASS "weforpay"
```

`port/s3/wifi_creds.h` 里用 `__has_include("wifi_creds_local.h")` 自动包含。
验证：`grep -c weforpay build/wifi/c6os.bin` == 2。

### §245 试了"壳保存全部 a0..a15"（S3_W2C_SAVEALL=1）：更早炸，已回退

理论：壳是 windowed 函数，却 `callx0` 进 call0 内核；call0 不轮转窗口，被调方可以
把同一窗口里 a0..a15 的 caller-saved 全改掉，而 windowed 调用者没料到 → `retw`
用错 a0（§242 的症状）。所以试着把 a0..a15 全存全取（64 字节 entry 帧正好放得下）。

真板结果：**更早炸**（`pc=0x42013f54`、`a0=0x3fcb938c`、`ws=6`，第一个 windowed
调用就失败），所以这个改动本身有 bug（注意 `retw` 会用 [a1+4] 自动恢复调用者 a1，
不能自己再 `l32i a1, a1, 4`——已修但仍不行）。留作开关，默认关闭。

**当前最佳仍是**（§242）：

```
make C6_ROOT=/home/winger/c6 S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON -DS3_WIFI_TASK_CALL0" \
     S3_PHY_VIA_OSI=1 S3_NO_WIN_SPILL=1 https
```

ppTask 能进 `_queue_recv` 循环，随后在 `osi_semphr_take` 的 `retw.n`
（`pc=0x4037c280`、`a0` 是栈地址、`ws=0x06`）炸。下一步要么把 SAVEALL 调试正确
（它理论上是解），要么回到"归一化+spill"那一对语义上整体做对。

### §246 当前状态小结（第 24~25 轮结束时）

**能跑到的位置**（真板，最佳配置）：

```
make C6_ROOT=/home/winger/c6 \
     S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON -DS3_WIFI_TASK_CALL0" \
     S3_PHY_VIA_OSI=1 S3_NO_WIN_SPILL=1 https
```

```
c6os s3 boot
M1 icache ok / M2 flash_diag ok / M3 wdt skipped / M4 task wifi ok / M5 tasks ok
T1 call0 任务进入 windowed 主流程
[wifimain] id=1 tick=0
W0 phy start / [phy] 跳过手动初始化：交给 OSI _phy_enable 回调
W1 bringup
B1 slot13=… magic=deadbeaf ver=00000008
B2 cfg@… osi=… magic@144=1f2f3f4f
[lock] / [pp] pp rom version / [net80211] / [unlock] / [mtxnew] #1 #2 / [semnew] #1
[task] 库建任务 fn=… id=2 stack=6144 prio=29(->2)      ← ppTask 建起来
[take] t=1 sem#1 tk=ffffffff                          ← init 等 ppTask 就绪
[give] t=2 sem#1                                      ← 握手完成
[recv] t=2 qh=… tk=ffffffff                           ← ppTask 进了收包循环
*** TRAP cause=0 pc=<osi_semphr_take 的 retw.n> a0=<栈地址> ws=0x06
```

**唯一剩下的拦路虎**：windowed 代码在**多层嵌套**（库 → 我们的 windowed 胶水 →
w2c 壳 → call0 内核）时，`retw` 的窗口簿记出错（`retw.n` 直接 cause=0）。
已知：
- 单层（最小用例 t_win1：一个 windowed 任务 + 一次壳调用）**完全正常**；
- `ws=0x06 / windowbase=3` 说明只有 2 个窗口 live，而这条调用链深度 ≥3 ——
  窗口溢出/欠载向量本应参与，但看起来没有正确触发；
- 向量实现（`_win_ovf4/8/12`、`_win_unf4/8/12`）与官方 HAL 逐条一致 ✓ 已核对；
- `PS.WOE` 已无条件置 1 ✓；`WINDOWSTART` 恢复改成"新任务归一化 / 老任务原值" ✓。

**下一步（下次接着干）**：
1. 在 `port_task_switch` 里把 (windowstart, windowbase, a0带CALLINC位) 写进 RTC 环
   （0x50001D00 起），用 `./tools/dbg.sh` + JTAG 读，**看第几次切换开始对不上**；
2. 用 t_win1 扩一个"深度 3 嵌套"的最小用例（windowed a() → windowed b() →
   壳 → call0 c()），把 WiFi 剥掉，专攻这个；
3. 若确认是溢出向量没触发，检查 `start.S` 里 WOE/WINDOWSTART 的设置时序
   （硬件要求：写 WOE 之后要先经过一次 `rsync`，且 WINDOWSTART 的位要与
   `windowbase` 配套），以及 `_vec_exc`（level-1 异常入口）在 windowed 上下文里
   有没有把 `windowstart`/`windowbase` 存进陷阱帧。

### §247 c6 编译器一度消失 + 新 c6c 不能用于本构建

2026-11-16 中午发现 `/home/winger/c6/tcc/c6` 不见了（目录只剩源码），构建直接
报"没有那个文件或目录"。查了一下：

- 新项目 `/home/winger/c6/c6c/build/c6c` 是**另一套编译器**：`-c` 出的是 native
  ELF（x86-64/aarch64），**没有"输出 C99 源码"的模式**，而我们的流程是
  `c6 → C99 → xtensa-gcc`，所以用它跑不通（试过：`-c -o` 出来的是 ET_REL 目标文件，
  下一步 gcc 编 C 就失败了）；
- 老 c6 的源码还在，`make -C /home/winger/c6/tcc -j8 c6` 一分钟就重新编出来了。

已在 Makefile 里把 `C6CC` 改成 `?=`（可覆盖）并写清上面这些。**结论：继续用
`/home/winger/c6/tcc/c6`；c6c 不适合这套交叉编译流程。**

### §248 关掉同步点追踪（减少 windowed 里的 printf）：故障仍在同一处

把 `g_dbg_sync` 置 0（`[take]/[give]/[recv]/[lock]` 全不打），ppTask 仍能进
`_queue_recv` 循环，随后仍在同一位置 TRAP。说明**不是 printf 把窗口用光**，
问题是 windowed 调用链本身的 `retw` 簿记（§246 的结论不变）。

另外确认：把 `RESTORE_FRAME` 在"纯归一化"和"新任务归一化/老任务原值"两种策略间
切换，**故障签名完全一样**（`ws=0x06, sp=0x3fcbebb0, pc=0x4037c284, a0=0x3fcbebe0`），
所以目前的故障点**不经过任务切换**（是两个 windowed 上下文交替时寄存器文件被
互相覆盖，归一化策略救不了）。

### §249 **spill 打开 + 纯归一化 = 不再崩**；新卡点：队列里有消息但接收者没被唤醒

配置（本次实测）：

```
make C6_ROOT=/home/winger/c6 S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON -DS3_WIFI_TASK_CALL0" \
     S3_PHY_VIA_OSI=1 https         # 注意：**不**加 S3_NO_WIN_SPILL（spill 打开）
```

串口（这是目前为止最健康的一次，全程没有任何 TRAP）：

```
[task] 库建任务 fn=40381584 id=2 stack=6144 prio=29(->2)
[take] t=1 sem#1 tk=ffffffff
[give] t=2 sem#1
[recv] t=2 qh=3fcc443c tk=ffffffff        ← ppTask 在收包循环里等消息
[semnew] #2 p=3fcc4afc max=1 init=0 t=1   ← init 又建了一个信号量
[lock] t=1 m#…                            ← 拿锁
[send] t=1 qh=3fcc443c tk=1               ← **往 ppTask 的队列里发了一条消息**
[unlock] t=1
[take] t=1 sem#2 tk=ffffffff              ← init 等 sem#2（等 ppTask 干完活）
（之后就静止了）
```

JTAG 侧证据（halt 时）：

- CPU 在 `port_idle`（说明两个任务都阻塞着，**没有崩、没有 TRAP**）；
- 队列计数：`0x50001208`(put_ok) = 1、`0x5000120C`(put_try) = 1 → **消息确实入队了**；
- `0x50000320`（队列诊断区）全 0 → 句柄没坏。

所以现在的**唯一卡点**：消息进了队列，接收方（ppTask，阻塞在 `os_queue_get_item` 的
`Q_WAIT_GET`）却没有被唤醒/没有被调度。`os_queue_put_item` 里是：
```
slot_write/tail++/count++ → os_task_wake_first(q, Q_WAIT_GET, 0) → os_obj_unlock
```
`os_task_wake_first_id` 的匹配条件是 `state==BLOCKED && wait_obj==obj && wait_kind==kind`，
`best` 取优先级最高者 → `unblock(best, 0)`。

下一轮就从这里查（很具体、代价小）：
1. 在 `os_task_wake_first_id` 里加"找到/没找到"的计数（RTC 或 `ets_printf`），
   确认是**没匹配上**（wait_obj/kind 不一致）还是**匹配上了但没被调度**；
2. 特别注意：`osi_queue_recv` 传给内核的是 `&q->q`（WQ 内嵌的 OsQueue），
   而 `os_queue_put_item` 收到的是同一个指针 —— 需要确认 `s3_w2c_queue_put` 的
   w2c 壳没有把指针"按值复制"或偏移；
3. 如果匹配没问题，就查 idle 循环的调度：`port_idle` 里是否只看
   `os_sched_preempt_ctx()` 而漏掉了"被 unblock 的任务"（S3_NO_PREEMPT 已关，
   但 idle→任务的切换路径要单独确认）。

### §250 **关键 bug 修好了**：`g_waiters` 快速返回把真正的等待者跳过

真板串口加了内核级标记（`b`=任务阻塞计入、`W`=唤醒命中、`z`=唤醒时 g_waiters==0、
`u<id>`=被 unblock 的任务、`.`=没匹配上）之后，看到的顺序是：

```
[take] t=1 sem#1
b                                   ← init 阻塞，g_waiters=1
[give] t=2 sem#1  → W u1            ← ppTask 给信号量，唤醒 init，g_waiters=0
[recv] t=2 qh=…  → b                ← ppTask 阻塞在收包队列，g_waiters=1
u1                                  ← 又有人 unblock 任务 1（虚假唤醒）→ g_waiters=0 ✗
[send] t=1 qh=…  → z                ← 发消息时 g_waiters 已经是 0 ⇒ **唤醒被跳过**
```

根因：`os_task_wake_first_id` / `os_task_wake_event` / `os_task_wake_all` 开头都有

```c
if (g_waiters == 0u) { return 0; }   /* "没人等待"的快速返回 */
```

而 `g_waiters` 只是个**全局计数**，一旦因为虚假唤醒/超时解阻塞而失配，就会变成 0，
于是真正的等待者被跳过 —— 消息明明入队了（`put_ok=1`），接收方永远不被唤醒。
**修法：删掉这三个快速返回，直接扫任务表**（代价可忽略）。
真板效果立竿见影：

```
[send] t=1 qh=3fcc443c tk=1
Wu2                                 ← ppTask 被唤醒
[lock]/[unlock] t=2 …（连打二十多轮，ppTask 真的在干活）
[give] t=2 sem#2 → Wu1
[wifi] esp_wifi_init_internal -> 0  ← **esp_wifi_init_internal 第一次返回 0！**
[wifi] bringup rc=0
W2 bringup returned
```

这是从"卡在库初始化"到"库初始化成功"的转折点。

### §251 下一个卡点：bringup 返回后从栈地址取指

紧接着上面：

```
*** TRAP sp=3fcd8ea0 cause=00000002 pc=3fcb9d15 ps=00000b10 vaddr=3fcb9d14 a0=00000000 ws=0
```

`cause=2` = InstructionFetchError，`vaddr=0x3fcb9d14` 落在 **DRAM/heap** 区
（0x3fcb9xxx），`sp=0x3fcd8ea0` 是**主栈/idle 栈**（`_stack_top`=0x3FCD9000 附近），
`a0=0`、`ws=0`。→ 从主栈上下文里跳到了一个数据地址。

最可能的解释：wifi 任务在 `s3_win_wifi_main` 里走完 bringup 后继续做关联
（`s3_wifi_associate`），中途某处返回到了错误地址；也可能是任务退出路径
（`os_task_exit`）在 windowed/调用链上出了问题。下一步：在 bringup 返回点之后
加阶段标记（`W3 associate` 等），确认是在哪一步崩的。

### §251 补充：加了 lwIP 阶段打印后出现"有时能过握手、有时停在半路"

`c6_lwip_start` 里加了 `[lwip] 1..7` 七个阶段打印（lwip_init / netif_add /
netif_up / dhcp_start / reg_rxcb）。两次真板实测：

- 第一次：跑到 `[send]` → `Wu2` → ppTask 连打二十多轮 `[lock]/[unlock]` →
  `[give] sem#2` → `esp_wifi_init_internal -> 0` → `bringup rc=0` → 然后
  `cause=2` 从 `0x3fcb9d14`（数据地址）取指（§251 的现象）；
- 第二次、第三次：停在 `[lock] t=1`（semnew #2 之后），连 `[send]` 都没到。

⇒ 说明**握手路径上还有时序敏感的问题**（我们的 preempt 切换与库的信号量/队列
握手之间）。下一步先查这个不稳定性（它比 lwIP 的崩溃更靠前）：
可以在 `os_sched_preempt_ctx` / `port_idle` / `si3_timer_irq` 里加"每次切换的
from→to"记录，或者临时用 `S3_NO_PREEMPT=1` 对比一下（协作式）是否稳定得多。

### §252 剩余故障的本质：**windowed 函数调 call0 时，寄存器被改但编译器不知道**

真板上两类崩溃签名（都是 windowed 胶水函数的 `retw`/`retw.n` 出事）：

| 配置 | 症状 |
| --- | --- |
| spill 打开（默认） | 有时能一路跑到 `esp_wifi_init_internal -> 0` + `bringup rc=0`；有时在 `osi_wifi_int_disable` 的 `retw.n` 炸（`cause=0`，`a0=0x8037bbba` 这种"带 CALLINC 的假地址"） |
| `S3_NO_WIN_SPILL=1` | 稳定在 `osi_semphr_take` 的 `retw.n` 炸（`a0` = 栈地址，`ws=0x06`） |
| `S3_W2C_SAVEALL=1`（我加的全寄存器版壳） | 第一个 windowed 调用就炸 —— 说明那版壳本身有 bug，还没调对 |

**根因（现在可以确定）**：我们的胶水层/壳是 windowed 函数，却要 `callx0` 进
**call0** 的内核。call0 的调用约定里 a0..a15 大量是 caller-saved，被调方可以随手改；
而 **GCC 在为 windowed 函数生成代码时并不知道这一点** —— 它按 windowed 的规则认为
`call8` 之后自己的 a0（返回地址）还在，于是函数尾部直接 `retw.n`（真板反汇编确认：
`osi_semphr_take`/`osi_wifi_int_disable` 尾部的 `retw.n` 前面**没有** `l32i a0,a1,0`）。
一旦被调方改到 a0，`retw` 就跳到一个假地址。

两条正解：

1. **让壳把 a0..a15 全存全取**（`S3_W2C_SAVEALL` 的思路）——壳对调用者完全透明，
   编译器"以为 a0 没变"就成立。当前实现有 bug，需要在最小用例（`tests/t_win1.c6`
   的单 windowed 任务）上单独调通；
2. **让编译器自己知道 a0 被改**：在胶水里用内联汇编 `callx0`（clobber 列表里写
   `"a0"` 等），编译器就会在 `retw` 前自己 `l32i a0,a1,0`。这样连壳都不需要，
   还少一层窗口（我们只有 4 个窗口，深度很紧张）。

顺带记录：`spill 打开` 比 `不 spill` 更能跑远（有一次跑到 bringup rc=0），
因为 spill 至少把祖先窗口落到栈上；但它会重写 WINDOWSTART，所以时好时坏。

**下一轮第一件事**：用 `tests/t_win1.c6`（单 windowed 任务 + 一次壳调用）把
`S3_W2C_SAVEALL` 调通。这是自包含的小目标，调通后 `make ... S3_W2C_SAVEALL=1` 应该
直接让整条 WiFi 流程稳下来。

### §253 lwIP 阶段：DHCP 挪到关联之后 + 用 RTC 信标代替 printf

- `c6_lwip_start` 里原来在 `netif_set_up` 之后立刻 `dhcp_start`，真板实测**卡在
  dhcp_start 里**（它要发第一包 → 走 `esp_wifi_internal_tx` → 库的发送路径，而当时
  还没关联 AP）。现在改成关联成功之后再启动：新增 `c6_lwip_start_dhcp()`，
  `s3_win_wifi_main` 在 `s3_wifi_wait_connected()` 成功后调用它，再 `c6_lwip_wait_ip`。
- 但更靠前的卡点出现了：`[lwip] 4 netif_set_up ok` 之后连打印都不出来 —— 说明
  **windowed 的 `ets_printf` 本身就可能是压垮窗口深度的最后一根稻草**。于是把
  `c6_lwip_start` 里的所有 `ets_printf` 换成 RTC 信标 `LWSTAGE(v)`
  （写 0x50000A18，零函数调用）。注意：0x50000A18 与别的诊断区有重叠风险，
  读的时候要留意（实测读到过 0x403815a0 这种代码地址 ⇒ 被别处覆盖了）。

**当前状态（诚实记录）**：整条链能跑到 `esp_wifi_init_internal -> 0` +
`bringup rc=0`（多次里成功过几次），然后进 `c6_lwip_start` 并完成
lwip_init/netif_add/netif_set_up，再往后就**不稳定**：有时卡住、有时
`cause=0` 在 windowed 胶水函数的 `retw` 上炸。根因仍然是 §252：
**windowed ↔ call0 交界处的窗口簿记**。

**建议的下一步（优先级）**：
1. 把 §231 的"整镜像 windowed"路线做完（编译链接一次就通过，只差 `start.S` 的
   windowed 启动路径）——这是唯一能彻底消除这个 ABI 边界的方向；
2. 或者把 `c6_lwip_port.c` / `wifi_glue.c` 里的每个 OSI 回调改成**手写汇编的
   windowed 函数**（不做额外调用、不碰窗口状态），把层次压到最低。

### §254 时序敏感：**打开**追踪反而走得更远

| 配置 | 停在 |
| --- | --- |
| `g_dbg_sync = 0`（不打 `[take]/[give]/[recv]/[lock]`） | 握手后不久（`bu1` 之后）就静止 |
| `g_dbg_sync = 1`（打全部同步点） | 多数时候能到 `esp_wifi_init_internal -> 0` + `bringup rc=0`，再进 `c6_lwip_start` |

也就是说：**串口打印的延迟改变了时序**，把某些竞态窗口让开了。这既是坏消息
（说明还有竞态）也是好消息（说明主路径本身是通的，只差把竞态消除）。

跨多轮的真板统计（同一份代码）：
- `esp_wifi_init_internal` 返回 0：约 1/3 的运行；
- `bringup rc=0`：同上；
- 进 `c6_lwip_start` 并完成到 `netif_set_up`：同上；
- 再往后（DHCP/关联）：**还没成功过**（§253）。

**目前的判断**：主路径已经打通到"库初始化成功"，剩下的都在 windowed/call0 交界
（§252）与时序竞态这两件事上。下一步要么把 ABI 边界彻底去掉（整镜像 windowed，
§231 已验证能编译链接），要么把 OSI 回调改成手写汇编的最小 windowed 函数。

### §255 `-O0` 编 windowed 胶水：IRAM 溢出 4224 字节，不可行

想法：`-O0` 时 GCC 的 windowed 尾声会在 `retw` 前显式 `l32i a0, a1, 0`，正好能治
§252 那个"编译器不知道 a0 被 call0 改掉"的问题。为此把三个 windowed 单元的优化级别
参数化成 `S3_SHIM_OPT`（默认 `-Os`）。实测：

```
make ... S3_SHIM_OPT=-O0 https
ld: region `IRAM' overflowed by 4224 bytes
```

⇒ `-O0` 不可行（我们的 IRAM 只剩约 7KB 余量）。开关留在 Makefile 里备用。
要治 §252 只能走"壳存全寄存器"或"整镜像 windowed"这两条路（§252 已列）。

### §256 这一轮的实测定位：卡在 `[lock]` 之后、库内部，且是**陷阱**不是死等

加了 `[lockok]`（mutex pend 的返回值）和 `[delay]`（`osi_task_delay`）两处追踪后：

```
u1[semnew] #2 p=3fcc4b2c max=1 init=0 t=1
[lock]   t=1 m#3336896514 p=3fcc3f2c
[lockok] t=1 pr=0 ok=1          ← 互斥量拿到手了
（之后再没有任何 OSI 回调，也没有 'b' 阻塞标记）
```

JTAG 一停就看清了：**CPU 不在 port_idle，而在 `_vec_default` 里转圈**，
`cause=0`、`EPC1=0x4037bb9a`、`a0=0x8037bb7a`。
`a0` 的低 30 位正是 0x4037bb7a（合法代码地址）、高 2 位是 CALLINC=2 —— 说明这个
返回地址**本身是对的**，但 PC 跑到了它 +0x20 的**字面量池**里（取指到数据 ⇒
IllegalInstruction）。典型的"`retw` 弹出的窗口数/窗口基址与预期差一格"。

至此可以确认：剩下的问题**全部**是 §252 那条 windowed↔call0 边界的窗口簿记，
不是业务逻辑问题。真板统计（同一份代码）大致 1/5～1/3 的运行能侥幸过这一段。

**试过并排除的**：
- `-O0` 编 windowed 胶水（想逼 GCC 显式恢复 a0）：IRAM 溢出 4224 字节（§255）；
- `S3_W2C_SAVEALL`（壳存全寄存器）：壳自己有 bug，最小用例 `t_win1` 就炸；
- `S3_NO_WIN_SPILL`：更早炸（`osi_semphr_take` 的 `retw.n`）；
- `S3_NO_PREEMPT`（协作调度）：没有改善；
- 关掉 `g_dbg_sync`：反而走得更短（§254）。

**下一步（下轮直接照着做）**：按 FreeRTOS 的 Xtensa windowed 端口的做法，
**在切换点把所有活着的窗口都 spill 到任务栈、并把 WINDOWBASE 归一到 0**
（而不是现在的"归一化 WINDOWSTART 到 1<<当前base"）。这条路要求：
1. `port_task_switch` 里先 `call8 xthal_window_spill` 等价的手写 spill（`w2c.S`
   里已有能用的 `WIN_SPILL`，壳里验证过）；注意它会写到每个窗口调用者 sp 下方
   16 字节，所以**必须在任务自己的栈上做**（现在 port_task_switch 就跑在任务栈上，
   §200 当年的失败是因为在中断栈上做）；
2. spill 完之后 `wsr windowbase 0` + `wsr windowstart 1`，再保存 a0..a15；
3. 恢复时同样先归一化再装寄存器（现在的 `RESTORE_FRAME` 只差第 1 步）。

### §257 决定性现场：非法指令来自 **`retw` 的窗口配套规则**，不是业务逻辑

扩展了 `_vec_default` 的 RTC 转储（0x50000100 起：标记/cause/vaddr/EPC1/PS/a0/a1/
WINDOWBASE/WINDOWSTART），真板抓到完整现场：

```
0x50000100: 0x0000bad0 0x00000000 0x00000000 0x4037bc0e   ← cause=0(IllegalInstruction)
0x50000110: 0x0004033c 0x8037bbee 0x3fcbec20 0x0000000b   ← PS  a0  a1  WINDOWBASE=11
0x50000120: 0x00000800                                     ← WINDOWSTART = 1<<11
```

对照 ELF 反汇编：**EPC1 = 0x4037bc0e 正是 `osi_wifi_int_disable` 的 `retw.n`**，
`a0 = 0x8037bbee` 的低 30 位是合法返回地址、高 2 位 = `10` ⇒ **CALLINC(n)=2**，
而 `WINDOWSTART = 1<<11` ⇒ 当前窗口处只有 1 个活窗口（**m=1**）。

Xtensa ISA 对 RETW/RETW.N 的非法条件（官方文档伪码）：

```
n = a0[31:30]                      进入本函数用的调用指令
m = WindowStart 在 WindowBase 处的活窗口数编码
n == 0 || (m != 0 && m != n) || PS.WOE == 0 || PS.EXCM == 1  => IllegalInstruction
```

现场正好是 `m=1, n=2`。**三处状态来源都指向同一个动作：把 WINDOWSTART 归一化成
`1<<WINDOWBASE`**

1. `port/s3/w2c.S` 的 `WIN_SPILL` 结尾（每条壳进来 spill 完就归一化 ⇒ 壳自己返回时
   面对 m=1、而它是被 `call8` 调进来的 n=2）；
2. `port/s3/ctx.S` 的 `RESTORE_FRAME`（任务切换恢复现场时归一化）；
3. `port/s3/irq.S` `_vec_level1` 的返回路径（抢占切换后 WINDOWSTART 保持别人的值）。

**试过并否掉的三个改法**（都留了记录，别再重复踩）：

| 改法 | 结果 |
| --- | --- |
| `WIN_SPILL` 结尾改成"写回入口的 WINDOWSTART"（IDF `SPILL_ALL_WINDOWS` 的做法） | 更早崩：`pc=4037bf96`、`a0=0x3fcbecf0`（栈地址）⇒ **溢出向量清掉 live 位之后，物理窗口里的内容并不可靠**，把 live 位写回去是错的 |
| 壳退出前写回入口的 WINDOWSTART | 同上，更早崩 |
| `RESTORE_FRAME` / 中断返回按帧里 a0 的 CALLINC 算 `WS=((1<<n)-1)<<(WB+1-n)` | 抢占路径上更早崩（`pc=40048849` 在 ROM 里） |

⇒ 现有"归一化"虽然与 ISA 的 m/n 规则冲突，但**实测能跑通大部分调用**（因为我们
spill 过，下溢向量能把祖先窗口补回来），所以暂时保持不动。

**下一轮的两个具体方向**（按性价比排序）：

1. **验证 spill 槽地址与下溢填充约定是否一致**：我们的 `WIN_SPILL` 靠 `rotw` 触发
   溢出向量，向量按官方 HAL 把窗口内容写到 `[a1-16..-4]`；而下溢填充读的是
   `l32e a0, a5, -16`（a5 由硬件给出）。如果两边的基准差 16/32 字节，就会出现
   "retw 看似合法但补回来的是垃圾"—— 用一个受控实验（写已知图案到 spill 区、
   再强制一次下溢）就能确认；
2. **让壳的阻塞调用不在 windowed 帧里做**：壳进入内核前先把自己这一帧"封账"
   （把 a0/a1 存到自己的栈槽、把 WINDOWSTART 设为与 n 配套），返回后再恢复 ——
   等价于把"跨任务切换的窗口状态"从 4 个物理窗口解耦。这条如果做通，就再也不怕
   在 windowed 调用中间被切走。

### §258 **修好了根因的一半**：WINDOWBASE 必须按任务保存/恢复

上一节（§257）把非法指令归到 `retw` 的窗口配套规则上，但真正缺的一块是
**WINDOWBASE 从来没被保存过**：

- `WINDOWBASE` 是**全局寄存器**；闭源库的 windowed 调用（`call8`/`call12`）一走，
  基址就搬走了；
- 我们的 `port_task_switch` 只存了 `WINDOWSTART`/`a0..a15`/`EPC`/`PS`，**没存
  `WINDOWBASE`**；
- 于是一个任务在 windowed 调用中间被切走、另一个任务跑了一堆 windowed 代码之后
  再切回来，被打断的那层 `retw` 就会把"调用者的窗口"落到**错误的物理寄存器**上
  ⇒ 有时是 `cause=0` 落在 `retw.n`（§256/§257 的现场），有时直接跳到栈地址。

修法（`port/s3/ctx.S`）：

1. 两处保存点都加 `rsr a5, windowbase; s32i a5, a1, 72`（帧偏移 72，原来是空的）；
2. `RESTORE_FRAME` 里**先**恢复 `WINDOWBASE` 再算 `WINDOWSTART`。麻烦点在于
   `wsr windowbase` 会把**所有虚拟寄存器重新映射，包括 a1（帧指针）自己**：
   所以先用当前映射把帧指针存进全局 `g_ctx_tmp_sp`，`wsr windowbase` 之后再用
   `l32r a1, .Ltmpsp` + `l32i a1, a1, 0` 把它取回来（`l32r` 只依赖字面量地址，
   不需要任何寄存器仍然有效）。

**效果（真板实测，一改就稳定）**：

| 配置 | 改前 | 改后 |
| --- | --- | --- |
| 协作（`S3_NO_PREEMPT=1`） | 多数停在 `[lock]`，偶尔到 bringup | **2/2 次都跑到 `W2 bringup returned`** |
| 抢占（默认） | 约 1/3 能到 bringup | **2/2 次都跑到 `W2 bringup returned`** |

也就是说 `esp_wifi_init_internal -> 0` + `bringup rc=0` 这一大段现在**可复现**了，
不再是撞运气。

**下一个卡点**（§259 继续）：`W2 bringup returned` 之后进 `c6_lwip_start` →
`lwip_init()` → `sys_thread_new`（起 tcpip 线程）时会炸：
`cause=2`（InstrFetchProhibited）、`pc=0x3fcb9d45`、`vaddr=0x3fcb9d44`（栈地址）、
`a0=0`、`ws=0` —— 跳到了一个栈地址上。位置正好是 `os_task_start_core` 里
`callx0 port_task_switch` 之后（`a0=0x420188fa`），所以下一步查"新起的 windowed
任务第一次被调度进去"这条路径（`s3_task_tramp` 的 `callx8` 前后窗口状态）。

### §259 时序极敏感：**加 4 条 store 就能把结果从 3/3 打成 0/3**

为了定位 `W2 bringup returned` 之后的崩溃，我在 `s3_task_tramp` 里多写了 4 条
`s32i`（把入口/sp/ABI/计数记到 DRAM 全局 `g_tramp_dbg`）。结果：

| 构建 | `W2 bringup returned` |
| --- | --- |
| b7a7960（只有 §258 的 WINDOWBASE 修复，无额外诊断） | **3/3** |
| 同上 + tramp 里 4 条 store | **0/3**（退回到 `[lock]` 卡住） |

⇒ 这条链路上还压着一个**极紧的时序窗口**：多 4 条 store 就翻车。已经把那段诊断
删掉（只留原来 RTC 的 3 个字），并在源码里写了警告"别再往这条路径加东西"。

**现在的稳定状态（可复现）**：

```
[lock] ... [lockok] ... esp_wifi_init_internal -> 0 ... bringup rc=0
W2 bringup returned
.
Wu2.b*** TRAP sp=3fcd8ea0 cause=00000002 pc=3fcb9d45 ps=00000b10 vaddr=3fcb9d44 ws=00000000 a0=00000000
```

即：**WiFi 库初始化 3/3 成功**，然后进 `c6_lwip_start` → `lwip_init()` →
`sys_thread_new()` 起 tcpip 线程时崩：跳进栈地址（0x3fcb9d44）、`a0=0`、`ws=0`
（一个活窗口都没有）。`sp=3fcd8ea0` 贴着 `S3_STACK_TOP=0x3fcd9000`，说明是启动
主任务的栈。

**下一轮从这里接**：

1. 崩点在新起的 windowed 任务（`tcpip_thread`）第一次被调度进来前后。先在**不改
   时序**的前提下取证：用 JTAG 在崩住后 dump `g_lwip_stage`（已有 8 个字信标，
   写在 C 侧、不影响启动路径）确认是 `lwip_init` 内部还是线程首次运行；
2. 重点看 `s3_task_tramp` 的 `callx8`：新任务帧的 `PS.WOE=1`、`WB=0`、`WS=1`；
   `callx8` 之后硬件应把新窗口的 live 位置起来（m=2 与 CALLINC=2 配套）。如果
   这里 `WS` 没被置位，下一次 `retw` 就会按"下溢填充"去读一个从没写过的 spill
   槽，读回来就是 `a0=0` / 栈地址这种垃圾 —— 和现场吻合；
3. 若确认，就在 tramp 里 `callx8` 之前把 `WS` 设为 `(1<<0)|(1<<2)` 之外的正确
   组合（或者干脆让 tramp 用 `entry`+`retw` 的 windowed 序列进入口）。

### §260 **又一次大跨步**：lwIP 起来了，并且进了关联流程

两处"库还没 start 就调 API"是这一段的元凶：

| 调用 | 原来放在 | 现象 | 改到 |
| --- | --- | --- | --- |
| `esp_wifi_internal_reg_rxcb` | `c6_lwip_start` 里（bringup 之后、`esp_wifi_start` 之前） | 真板直接崩：`pc` 跳进栈地址、`a0=0`、`ws=0` | 关联成功之后（`c6_lwip_start_dhcp`，和 DHCP 一起） |
| `dhcp_start` | 同上（§253 已改） | 卡在 dhcp_start 里 | 同上 |

真板现在的完整链路（可复现）：

```
[wifi] esp_wifi_init_internal -> 0
[wifi] bringup rc=0
W2 bringup returned
[lwip] start rc=0
[wifi] associate ssid=weforpay.com
[wifi] set_storage(RAM)=0
*** TRAP cause=00000002 pc=3fcb9d45 vaddr=3fcb9d44 ws=00000000 a0=00000000
```

即 **WiFi 库初始化 + lwIP 启动都通了**，卡在 `s3_wifi_associate()` 里
`esp_wifi_set_storage()` 之后的下一个调用（`esp_wifi_set_mode()`）。

**取证结论**：崩溃前后 `g_osi_calls[]` 计数完全没变（JTAG 直接 dump 3FCC3984 起的
128 个字）⇒ `esp_wifi_set_mode()` 里**一次 OSI 回调都没发生**就跳进了栈地址
⇒ 是**闭源库内部的函数指针还没被初始化**（DRAM 里残留的栈数据），不是我们的
OSI 表接错。

**并且这一段时序极敏感**：仅仅在 `set_mode` 前后加一句 `ets_printf`，整条链路就
退回到连 `associate ssid` 都到不了。所以后续请用"DRAM 计数器/信标"取证，
**不要在这几行里加打印**。

下一轮从"`esp_wifi_set_mode` 内部跳野指针"接：方向是查库里哪张表应该在
`esp_wifi_init_internal` 阶段被填（很可能是我们 `esp_wifi_init_internal` 之后
少了 IDF 里的 `esp_wifi_set_mode` 前置动作，例如 `esp_netif_create_default_wifi_sta`
对应的 OSI 槽位），可以用"把我们所有 OSI 桩的返回值/写入地址都记到 DRAM"的办法，
事后 dump 出库到底读了哪个槽位。

### §261 崩溃点收窄到"闭源库的 API 锁/init 状态"这一层

用"叶函数 vs 会嵌套的函数"对照实验把范围压到了很窄的一条缝：

| 实验 | 结果 |
| --- | --- |
| 连调两次 `esp_wifi_set_storage`（叶函数） | **两次都成功**（第二次后仍打印正常） |
| 跳过 `esp_wifi_set_mode`，直接 `esp_wifi_set_config` | 崩点不变（说明不是 set_mode 特有） |
| 把调用顺序换成先 `set_mode` 再 `set_storage` | **一进 set_mode 就崩**（确认是函数本身） |
| 人为多加一层 windowed 调用（`noinline` 包装） | 崩点不变 ⇒ **"嵌套深度/窗口溢出"假设被否掉** |

再把 `esp_wifi_set_mode`（0x4203d594）反汇编出来看它到底做了什么：

```
entry a1, 32
call8 wifi_init_completed          ; ← 崩点在这条链里
l32r a8, g_osi_funcs_p
movi a10, 24
l32i a8, a8, 0        ; a8 = OSI 表
l32i a8, a8, 0x174    ; 槽 93 = _wifi_zalloc
callx8 a8             ; 分配 24 字节消息块
... 填 block[0]=10, block[4]=wifi_set_mode_process, block[8]=mode
call8 ieee80211_ioctl
```

而 JTAG 现场取证表明：

- OSI 表本身是**好的**：槽 17（`ieee80211_ioctl` 里用来拿锁的那个槽）= 0x4037c104（我们的
  glue，合法 flash 地址）；
- 崩溃前后 `g_osi_calls[]` **一个都没变**，连槽 93（`_wifi_zalloc`，之前计数 5）和槽 43
  （`_mutex_lock`，计数 0x25）都没涨 ⇒ 崩在 `wifi_init_completed` → `wifi_api_lock`
  的**前几条指令**里，连 OSI 都还没调到。

`wifi_init_completed`（0x4203c744）的结构是：

```
call8 wifi_api_lock            ; ← 拿库自己的 API 锁
...
l32r a8, (3fcba318)            ; 读"初始化状态"全局
l32i a8, a8, 0
beqi a8, 3, ...                ; == 3 表示完成
```

所以下一轮最该查的是 **`wifi_api_lock`（0x4203c6d8）一开始读的那个全局**：如果它在
`esp_wifi_init_internal` 之后仍然是垃圾（我们把这套初始化"半手动"跑起来，库里有些
静态对象可能依赖 IDF 的 `esp_wifi_init` 包装层），那就对上了"叶函数能过、只要一碰
锁就跳野地址"的全部现象。用 JTAG 在崩住后 dump 0x3fcba318 附近 + 反汇编
`wifi_api_lock` 的前 20 条即可确认。

### §262 顺手把 `WIN_SPILL` 结尾的归一化去掉了（对齐 IDF）

IDF 的 `SPILL_ALL_WINDOWS`（xt_asm_utils.h）转完一圈就结束，**不动 WINDOWSTART**
（由硬件的 overflow/`rfwo` 自己维护）。我们原来在结尾多做了一次
"归一化成 `1<<WINDOWBASE`"，会把祖先窗口的 live 位抹掉；现在已删除、并在源码里
写了原因。实测崩点未变（说明它不是当前的主因），但这是与官方实现对齐的正确做法。

### §263 本轮补充取证（都写在这里，免得下轮重复）

- **init 状态全局是好的**：JTAG dump 0x3fcba310 起 16 字，`0x3fcba318 == 3`
  ⇒ `wifi_init_completed()` 会返回"初始化完成"，所以崩溃不在它读状态那一步。
- `wifi_api_lock` 反汇编定位不可靠（objdump 在这段 `.flash.text` 上线性扫描会跑偏，
  同一段里既有指令也有字面量池），别再靠反汇编硬猜。
- `esp_wifi_get_mode`（只读 ioctl）实验**不可用**：仅仅把 `set_mode` 换成
  `get_mode` + 一句 printf，整条链路就退回到连 `associate ssid` 都到不了
  —— 又一次证明这一段是**时序敏感**的，实验必须只用 DRAM/RTC 信标，不能加打印。
- 现状（每次可复现）：`set_storage` 成功 → **下一个走 ioctl 的库调用必崩**
  （`cause=2`、PC 跳栈地址 0x3fcb9d45、`a0=0`、`ws=0`），且崩溃现场 OSI 计数不变。

### §264 AMPDU 对照组也是阴性；并且"ws=0"是误读

- 把 `ampdu_rx_enable/ampdu_tx_enable/rx_ba_win` 全关掉重跑：**崩点一模一样**
  （`cause=2 pc=3fcb9d45`）⇒ 不是 AMPDU/BA 初始化的问题。已回退。
- **重要更正**：`s3_trap` 里那句 `ws=` 打的其实是 `(ps>>16)&0xF`，即 **PS.CALLINC**，
  而且 `rsr ps` 取到的是**异常处理器自己的 PS**（EXCM=1、CALLINC 恒为 0）
  ⇒ 之前把日志里的 `ws=0` 当成"WINDOWSTART 被清零"是**误判**，别再据此推窗口。
  已把标签改成 `callinc=`，真要看窗口状态必须用 JTAG 读 WINDOWSTART。
- 剩下的硬事实只有三条：`cause=2`（取指失败）、PC 跳进**任务栈地址** 0x3fcb9d45、
  `a0=0`；且只在"走 ieee80211_ioctl 的库调用"上出现（叶函数 set_storage 永远没事）。
  纯静态反汇编译不可靠（`.flash.text` 里指令和字面量池混排，objdump 线性扫描会跑偏），
  下一轮要用 JTAG 在崩住时**读 WINDOWSTART + 扫栈上的返回地址链**，而不是继续猜。

### §265 真板调试设施（这一轮把"能不能单步"这件事查清楚了）

**重要更正**：真正的 `esp_wifi_set_mode` 在 **0x4203d2b0**（`esp_wifi_set_storage`
= 0x4203eb28，`esp_wifi_set_config` = 0x4203dc14）。§261 里被当成 set_mode 反汇编的
**0x4203d594 其实是别的函数**，那一节的静态结论要打折，别当成定论。

这一轮试出来 / 试死掉的东西，逐条记下来免得下轮重试：

| 手段 | 结果 |
| --- | --- |
| gdb `continue` / `stepi` | ✗ 永远报 "Cannot execute this command while the target is running"；即使 `set ESP_ONLYCPU 1` 只留 cpu0、`targets` 明确显示 halted 也一样 |
| gdb `hbreak` | 能用（能在寄存器里看到 `ibreaka0` 被写成目标地址），但后续 continue 就死 |
| telnet 手工写 `ibreaka0` + `ibreakenable` | 写得进去（读回正确），**但不会触发** —— Xtensa 的 IBRK 要生效还得配 debug 模块的 `DCRA`，gdb 的 hbreak 会顺带配，手工写没有 |
| OpenOCD 自己的 `reset run` / `reset halt`+`resume` | ✗ 起不来我们的 app：停在 `port_idle`，`g_lwip_stage` 全 0（内核活着，WiFi 任务没跑起来） |
| 先 `hbreak` 再让 esptool 复位 | ✗ 不行：这块板子的 USB-Serial-JTAG 和串口是**同一个 USB**，esptool 一复位 OpenOCD 就 "Target disconnected" |
| 新增 `tools/ocboot.sh` / `tools/ocstep.py` | ✓ 可用：telnet 驱动（halt / reg / mdw / stepi）。`stepi` 本身是好用的，缺的只是"怎么停下来" |
| 新增调试闸门 `g_s3_dbg_gate`（`S3_WIFI_ASSOC_GATE`） | ✓ **有效**：`flashmon` 真复位后 app 停在 `s3_wifi_bringup` 入口自旋，等 JTAG `mww` 写 1 再往下走（实测 halt 在 0x4037d33e = 闸门循环里） |

闸门的用法（**只在调试构建里编**，默认完全不存在，不影响 §259 的时序敏感性）：

```sh
# 让 ifdef 生效需要传给 port C 的编译（S3_C6FLAGS 只作用于 .c6，不够）
make C6_ROOT=/home/winger/c6 S3_CDEFS="-DS3_WIFI_ASSOC_GATE" ... https
./tools/flashmon.sh build/wifi/c6os.bin 2      # 真复位，app 会停在闸门
./tools/ocboot.sh                              # 起 OpenOCD（只暴露 cpu0）
# 然后 halt → 下断点 → mww 0x<gate> 1 → resume
```

**下一轮的目标很明确**：闸门已经能保证"app 一定在断点架好之前不动"，所以只要解决
"IBRK 怎么才触发"这一件事 —— 首选 `gdb hbreak`（它配 DCRA）+ **telnet 发 `resume`**
（绕开 gdb 的 continue bug），闸门开着、断点架好后再放行。

### §266 闸门实验（做了、失败、已全部回退）

想法：既然"先下断点再复位"走不通（§265），那就让 app 自己在指定位置等 JTAG。
做了两个闸门：闸门 1 在 windowed 主流程入口，闸门 2 在 `esp_wifi_set_mode` 之前。

结果全负，而且很有信息量：

- **c6 编译器会把 volatile 自旋编译成死循环**（§267，见下）；
- 换成 C 侧自旋（gcc 生成的循环是对的，反汇编确认是
  `memw ; l32i.n a8,a9,0 ; beqz a8, .-4`）之后，闸门 1 放行依然立刻掉进
  `_vec_default`（pc=0x40379bd6），`g_lwip_stage` 全是 0 —— 也就是说
  **只要在这条路径上加东西，它就不走到原来的崩点了**（又一次印证 §259）；
- 更糟的是：连"默认值为 1、正常构建里只多两条指令"的闸门也会破坏正常流程
  —— 正常构建加了一个空的 `s3_dbg_gate2()` 调用后，串口停在 WiFi init 的
  `[lockok]` 就再也不往下走了（连 associate 都到不了）。

结论：**这条路径对代码布局/时序的敏感度比想象的高得多，任何插桩都会改变结果**。
两个闸门 + 相关调用已全部 `git checkout` 回退，真板行为恢复到已提交的基线（同样
`associate ssid` → `set_storage` → `TRAP cause=2`）。§265 里那两个工具保留。

### §267 c6 编译器的一个坑：`while (volatile_flag == 0) { }` 会变成死循环

这个必须记下来，以后所有 .c6 里的自旋等待都会踩：

```c
volatile u32 g = 0;
while (g == 0) { }          /* 想让别人写 1 来放行 */
```

实际生成的（`tools/objdump` 反汇编，真实抓到的）：

```
403786f4: l32r  a9, <g>
403786f7: l32i.n a9, a9, 0
403786f9: bnez.n a9, 40378700     ; 非 0 才跳出
403786fb: j   403786fb            ; ← 自己跳自己，**再也不读 g 了**
```

volatile 的读被提到了循环外，效果等于 `if (g == 0) { for (;;); }` —— JTAG 无论
怎么写 `g` 都出不来（实测写进去了、读回来是 1，CPU 仍然卡死在 0x403786fb）。
循环体里加一个 `os_task_delay(10)` 可以让编译器不敢搬，但那样又会把后面的流程带崩
（放行后立刻 `_vec_default`）。**结论：不要在 .c6 里写空自旋**；真要等，用信号量
或者放在 C 文件里（gcc 处理 volatile 是正确的）。

### §268 下一轮的方向（别再重复上面的路）

1. 调试手段上，能用的只剩：`tools/flashmon.sh`（唯一可靠的真复位）+
   `tools/ocboot.sh`/`ocstep.py`（halt/reg/mdw/stepi 可用）+ 串口。断点这条
   路（gdb 的 continue、telnet 写 IBRK）都已确认不可用，别再试。
2. 既然"加东西就变样"，反过来想：**现在的崩点本身可能就是"时序刚好不对"的产物**。
   最值得试的一条是：在 `esp_wifi_init_internal` 返回之后、关联之前**真正睡一会儿**
   （让库的 ppTask 完全跑起来），这需要在 port/s3 里补一个 windowed→call0 的
   `s3_w2c_delay(ms)` 桥（现在没有，直接调 `os_task_delay` 会 TRAP），然后在
   `s3_wifi_associate` 最前面调用它。
3. 另一个便宜的对照：把 `S3_MAX_CTX`/任务栈调大一点，确认不是上下文池/栈的边界问题。

### §269 先睡 2 秒再关联：崩点完全不变 ⇒ 不是"库还没起来"的竞态

新加了 windowed→call0 的桥 `s3_w2c_delay(ticks)`（`port/s3/w2c.S`，映射到
`os_task_delay`；windowed 侧直接调 call0 的 `os_task_delay` 会 TRAP，历史坑）。
在 `s3_wifi_associate` 里 `esp_wifi_print` 之后先 `s3_w2c_delay(200)`（2 秒，
`C6OS_TICK_HZ=100`）再动库 API。

结果：**崩点一模一样**（`set_storage` 成功 → `TRAP cause=2 pc=3fcb9d4d`）。
⇒ 这个崩溃是**确定性**的，不是 ppTask 没跑起来的时序竞态。这条结论很值钱：
之前所有"时序敏感"的现象（§259）影响的是**能走到多远**，不是这次崩不崩。

### §270 关键收窄：`esp_wifi_get_mode` 能成功，只有"设置"这一路崩

把 `esp_wifi_set_mode(1)` 换成只读的 `esp_wifi_get_mode(&m)`（照样走
`ieee80211_ioctl` 的队列/等应答全流程）：

```
[wifi] associate ssid=weforpay.com
..[wifi] set_storage(RAM)=0
..[wifi] get_mode=0 m=2          ← ★ 整整一个 ioctl 来回，成功
.Wu2.b*** TRAP ...              ← 紧接着的 esp_wifi_set_config 崩
```

这一下把范围压得很死：

- ioctl 消息队列、semaphore、`_wifi_zalloc`、等应答 —— **全都是好的**（否则
  get_mode 不可能成功）；
- 崩的是 `esp_wifi_set_mode` / `esp_wifi_set_config` 这条**"应用模式/配置"**的路；
- 结合 §261/§265：入口、OSI 表、init 状态（=3）都正常，崩在库内部某张**按模式
  初始化时才用的函数表/对象**上（`wifi_set_mode_process` 一类）。

补充：把两次 `set_mode`（先 NULL 再 STA）一起塞进源码后，连 `associate ssid` 都
到不了（布局一变就变样，§259 又一次）。已全部回退，只留下 `s3_w2c_delay` 这个桥
（未使用、无害）。

**下一轮**：用 get_mode/set_mode 这个对照继续二分 —— 比如只调 `esp_wifi_set_mode(0)`
（NULL 模式，最轻）看崩不崩；崩就说明是"模式切换机制"本身，不崩就说明是 STA 模式
特有的初始化（那就要盯 `net80211_funcs_init` / `esp_wifi_set_mode_privacy` 那条路）。

### §271 一个重要发现：**只要代码大小变一点，库在 init 里就卡住**

用 `S3_NO_PREEMPT=1` 构建时，基线变得非常稳（连跑 2 次，崩点逐字节相同）：

```
[wifi] associate ssid=weforpay.com
..[wifi] set_storage(RAM)=0
.Wu2.b*** TRAP sp=3fcd8ea0 cause=00000002 pc=3fcb9d4d ...
```

在这个稳定基线上去做 §270 的二分（只加了一个 `get_mode` + 一次
`set_mode(0)`，一共几十字节代码），结果**连 `associate ssid` 都打不出来**了：

```
[semnew] #2 ... t=1
[lock]   t=1 m#3336896514 p=3fcc3f6c
[lockok] t=1 pr=0 ok=1          ← 到这里就再也不动了
```

而且卡住的位置在 **`esp_wifi_init_internal` 内部**，我们新加的那几行代码
**根本没执行到**。也就是说：这不是"我们加了调用扰动时序"，而是
**闭源库自己的执行结果跟二进制布局有关**。

排除掉的可能：
- `tools/chkimg.py` → `OK（2 个 flash 段，文件偏移 == LMA）`，镜像约束没问题；
- 栈池没有耗尽（没有任何 `[ctx] 栈池不够` 输出，池子 24KB / 实测用量约 18KB）。

下一轮最值得查的一条（纯离线、不用上板）：从 ELF 里把闭源库各段
（`.flash.text`/`.iram*`/`.dram*`）的 VMA 和长度 dump 出来，看**卡住的构建**和
**能跑的构建**之间，是否有某个段跨了 64KB 边界 / 落到我们 MMU 表覆盖范围之外。
代码大小一变就卡，最像"某段代码没被映射进地址空间"这类问题（对应 §202 那套
flash MMU 约束）。

### §272 DRAM 已经用满：WiFi 堆（64KB）加不上去

`g_wifi_heap_mem` 原来是 `[16384]`（64KB），实测这已经到顶：

| 大小 | 结果 |
|------|------|
| 64KB（现状） | 链接通过 |
| 96KB | `region DRAM overflowed`，链接失败 |
| 150KB / 160KB | 同样失败（160KB 时多出 4928 字节）|

当前 DRAM 布局（`nm`/`readelf`）：`.data` = 0x1b660（约 112KB）、`.bss` = 0x1cd10
（约 118KB，其中栈池 24KB + WiFi 堆 64KB）、`.text`(IRAM) 0xf530。DRAM 区
`0x3FCA0000 + 0x4F000` 基本被这三块吃干净了。

所以：**如果后面发现闭源库的 rx/tx buf 池（IDF 默认 32 个动态 rx + 32 个动态 tx，
按 1.6KB 算就是上百 KB）需要更大的堆，就必须先在别处腾 DRAM**（例如把 `.data`
里的大块静态缓冲挪走、或缩小栈池），单纯调大 `g_wifi_heap_mem` 是不可能的。

（已回退到 64KB，真板基线重新验证通过。）

### §273 拿到了崩溃时的**全部寄存器**（不用改一行代码）

关键技巧：`_vec_level1`（`port/s3/irq.S`）进陷阱时已经把 `a0~a15`、`EPC1`、`PS`
全部存进陷阱帧了，而 `_vec_exc` 打完日志后是 `j 1b` 死循环 —— **帧一直留在内存里**。
所以上板跑到崩溃之后，用 `tools/ocboot.sh` 起 OpenOCD，telnet 里
`targets esp32s3.cpu0` / `halt` / `mdw 0x3fcd8ea0 24` 就能把现场全部读出来
（帧基址就是串口打印的 `sp=`）。这次实测：

```
0x3fcd8ea0: 00000000 00000000 3fcb964d 00000000 42018686 3fcc2e4c 00000000 3fcc2e4c
0x3fcd8ec0: 00000000 00000000 50001500 00000001 3fcb96bd 00000002 00022599 3fcb9d4d
0x3fcd8ee0: 00000b00 000b0000 000af2b0 42018686 00060400 00000008 00000003 4201856d
```

按 irq.S 的存法（+0=a0 +4=a2 … +56=a15 +60=EPC1 +64=PS）解出来：

| 项 | 值 | 说明 |
|----|----|------|
| EPC1 | `0x3fcb9d4d` | 出错 PC，**奇数**（指令取指地址必须对齐→cause=2） |
| a3 | `0x3fcb964d` | = `__c99_d2` **块首** |
| a13 | `0x3fcb96bd` | = `__c99_d2+0x70` |
| a5 | `0x42018686` | = `os_task_start_core+346` |
| a11 | `0x50001500` | 外设寄存器地址 |
| PS | `0x00000b00` | |

**最重要的两条**（用 `nm` 对符号、再用生成代码对偏移）：

1. `0x3fcb964d` 这个符号是 `__c99_d2`，size 1876 —— 它来自
   **`build/wifi/c99/task.c`（即 `kernel/task.c6`）**：c6 编译器给每个 .c6
   模块生成一个 `static unsigned char __c99_d2[N]` 当它的静态数据区，所有
   文件级 static 变量都用 `(char *)__c99_d2 + 偏移` 访问。
2. 出错 PC `0x3fcb9d4d` = `__c99_d2 + 1792`，而 1792 在生成代码里正是
   **`os_task_create_on()` 里那个静态计数器**：

   ```c
   __c99_r[1].i = (unsigned int)*(unsigned int *)((char *)__c99_d2 + 1792);
   ...
   *(unsigned int *)((char *)__c99_d2 + 1792) = __c99_r[0].i;   /* ++ */
   ```

   也就是说：**CPU 跳到了"某个 static 变量的地址"上去执行** —— 一个
   `callx/jx` 的目标寄存器里装的是 `&变量`，而不是它的值（或是一个被写错成
   地址的函数指针）。

结合 a5 = `os_task_start_core+346`，最合理的解释是：**崩溃发生在内核建任务 /
任务入口这条路上**（`kernel/task.c6: os_task_create_on` → `t->entry = entry`
→ `port_task_ctx_create` / `os_task_start_core`），某个 entry 指针被写成了
一个数据地址。下轮就从这条线查：在建任务时把 `entry` 打出来（或者用 §273 的
JTAG 办法先读 `g_ctx[]` 里的 `entry` 字段），看是不是闭源库
`_task_create` 传进来的 entry 本身就不对，还是我们 `port_task_ctx_create`
的 a2/a3/a4 参数错位。

### §274 崩溃时 `g_ctx[]` 的真相：**闭源库 ppTask 的保存 sp 是野的**

接着 §273，用同样的 JTAG 办法读 `g_ctx`（`nm` 给的是 `0x3fcc2e00`，`S3Ctx` =
`u32 r[16]; u32 sp; void *stack; u32 size;` → 76 字节/ctx）：

```
0x3fcc2e40: 3fcbeb10 3fcbce00 00002000      <- ctx[0].sp / .stack / .size
0x3fcc2e8c: 3fcd8ea0 3fcbee00 00001800      <- ctx[1].sp / .stack / .size
```

- **ctx[0]**（我们的 windowed 主任务）：栈 `0x3fcbce00` + 0x2000，保存的
  `sp=0x3fcbeb10` —— 正好在栈顶附近，**完全正常**；
- **ctx[1]**（闭源库的 ppTask，栈大小 0x1800=6144，和 OSI `_task_create`
  打印的 6144 对得上）：栈 `0x3fcbee00`…`0x3fcc0600`，但保存的
  **`sp = 0x3fcd8ea0`** —— 比它自己的栈高出约 41KB，落在
  `S3_STACK_TOP(0x3FCD9000)` 附近那块**异常/中断用的栈**里。

而 §273 里串口打的 `sp=3fcd8ea0` 正是这个值（陷阱帧基址）。两件事对上了：

1. ppTask 是在一个**不属于它自己的栈**上被打断/切走的；
2. 它下一次被恢复时，`sp` 指向异常栈那块内存 → 一路 `retw`/`ret` 弹出垃圾
   → 最后跳到 `0x3fcb9d4d`（task.c6 静态数据 +1792 的地址，奇地址→取指异常）。

所以这次 `cause=2` **不是闭源库的 bug，而是我们的中断/任务切换把 sp 存错了**：
level-1 定时器中断里做任务切换时，往被中断任务的 ctx 里存的很可能是
**中断自己的栈**（`_vec_level1` 换到的那块），而不是被中断现场的 sp。

**下一轮第一件事**：查 `port/s3/irq.S` 的 `_vec_level1` + `s3_timer_irq` +
`port_task_switch` 这条链，确认"被中断任务的 sp"到底是哪个值被存进
`ctx->sp`；对照 `port_task_ctx_create` 里 `r[16]` 占位、`sp@64` 的约定
（`ctx.c` 注释写的就是"保持 sp@64 布局"）。

### §275 level-1 中断里的切换约定（已核对代码，问题在"谁写 ctx->sp"）

`port/s3/irq.S` 的 `_vec_level1` 尾部是：

```
call8 s3_timer_irq        /* a10 = 返回的 ctx（0 表示不切换） */
beqz a10, 2f
l32i a1, a10, 64          /* a1 = ctx->sp（S3Ctx 里 sp@64） */
2:
l32i a2, a1, 60 ; wsr epc1 ; l32i a2, a1, 64 ; ori 16 ; wsr ps
l32i a0..a15, a1, ...     /* 从 a1 指的 80 字节陷阱帧恢复 */
addi a1, a1, 80 ; rfe
```

也就是说约定很清楚：**`ctx->sp` 必须指向该任务自己的 80 字节陷阱帧**，
恢复时 `a1 = ctx->sp + 80` 就是它继续跑用的栈。

再对照 §274 的实测：ctx[1]（ppTask，栈 0x3fcbee00/0x1800）存的
`sp = 0x3fcd8ea0`，而 HR 串口打印的陷阱帧基址也正是 `0x3fcd8ea0` ——
说明**这个值就是"当前这次中断的陷阱帧"**，被当成了 ppTask 的保存现场。
以约定推回去：这说明"被打断时正在跑"的那个任务的 sp 本来是
`0x3fcd8ef0`（≈`S3_STACK_TOP` 下面 4 字节对齐处），却被记到了 ctx[1] 名下。

所以下一步要看的不是 irq.S，而是：
1. `s3_timer_irq()` 里"当前任务 → next 任务"的判定和 `cur->ctx->sp = 陷阱帧地址`
   的写法（谁被当成 cur）；
2. `port_task_switch`（`port/s3/ctx.S:98`）里保存 sp 的那段，和
   `port_task_ctx_create` 的 `r[16]` 占位/`sp@64` 布局是否一致。

### §276 ★找到病根并修掉：`port_irq_fill` 把别的任务的陷阱帧记成了"当前任务的保存 sp"

`port/s3/ctx.c` 的 `port_irq_fill()` 原来是无条件：

```c
if (c != 0) { c->sp = frame_sp; }      /* c = g_s3_cur[core] */
```

`g_s3_cur` 指的 ctx **不一定就是真正在跑的那个任务**（典型：闭源库的 ppTask
挂在 `g_s3_cur` 上，而主流程其实跑在主任务栈 `S3_STACK_TOP` 那片上）。于是
**别人的**陷阱帧被记进了 ppTask 的 ctx —— 就是 §274 里 JTAG 看到的
`ctx[1].sp = 0x3fcd8ea0`（不在它自己的 `0x3fcbee00+0x1800` 里）。它下次被恢复时
sp 指到别的栈上，一路弹栈弹进数据区，最后跳到 `task.c6` 的静态数据（§273）。

修法（最小改动）：只有陷阱帧确实落在**这个任务自己的栈范围**内，才认作保存点；
否则拒绝，并把 (次数, frame_sp, ctx) 写到 RTC `0x500017C0` 方便上板核对。

**效果（真板实测，立竿见影）**：

```
之前： [wifi] associate ssid=weforpay.com
       ..[wifi] set_storage(RAM)=0
       .Wu2.b*** TRAP ... cause=00000002 pc=3fcb9d4d   ← 卡在 set_mode 前

现在： b[wifi] set_mode(STA)=0                            ← set_mode 成功返回！
       .Wu2.b.[v2]wifi|Password length matches WPA2 standards,
                authmode threshold changes from OPEN to WPA2   ← set_config 也被处理了
       *** TRAP sp=3fcc0500 cause=0000001c pc=420458c7 ...
```

也就是说 §259/§261 以来那条"一调闭源库就崩"的路**已经通了**，剩下的是一连串
新的、更靠后的问题。

### §277 新的崩点：`sta_is_wpa3_enabled` 里空指针 +0x1b4

新陷阱：`cause=0x1c`（LoadStoreAlignment）、`vaddr=0x48`、`pc=0x420458c7`、
`a0=0x82037492`（返回地址 = `wifi_set_config_process+402`）。反汇编：

```
420458bc <sta_is_wpa3_enabled>:
  entry a1, 32
  l32r  a2, <g_cnxMgr 的地址>          ; JTAG 读出来 a2 = 0x3fcd55e8 = g_cnxMgr
  l32i  a8, a2, 0x1b4                  ; a8 = *(g_cnxMgr+0x1b4) = NULL  ← 就是它
  movi.n a2, 0
  l32i  a9, a8, 72                     ; 读 NULL+0x48 → cause=0x1c（不是普通 load，是宽对齐访问）
```

`nm` 里 `0x3fcd55e8` 正是闭源库的 **`g_cnxMgr`**（size 3944），JTAG 读
`0x3fcd579c`（= `g_cnxMgr+0x1b4`）确实是 0。旁边 `g_cnxMgr+0x1b0` 也是 0。

所以：**库的某个子对象还没被创建**，`sta_is_wpa3_enabled` 就把它当非空用了。
好消息是这份库**带符号表**（`sta_is_wpa3_enabled`、`wifi_set_config_process`
都能解析），下一轮可以直接查"是谁写 `g_cnxMgr+0x1b4`"（按符号名找
`esp_wifi_start` / `wifi_init_*` / `net80211_*` 里对 0x1b4 的 store），
把那条初始化路径补上；或者反过来先把 `esp_wifi_start()` 提前/单独跑一次验证。

### §280 ★★突破：注册 WPA 回调表（每格填"返回 0"的窗口化桩）—— `esp_wifi_set_config` 第一次返回 0

承接 §277。`g_cnxMgr+0x1b4` 是 IDF 里由 wpa_supplicant 调
`esp_wifi_register_wpa_cb_internal()` 塞进去的 **WPA 回调表**。我们注册全零表后
`sta_is_wpa3_enabled()` 的空指针没了，但库里紧接着 `callx8` 一个槽 →
`pc=00000000`（cause=0x14、`a0=820342bd` = `ieee80211_ioctl_process+0x62`）。

所以最终做法（`port/s3/win_bridge.S` + `port/s3/win_island.c`）：

1. 在 flash 段手写一个窗口化叶子函数 `s3_wpa_cb_stub`：`entry a1,16 / movi a2,0 / retw`。
2. 在**windowed 岛**里（不是 `wifi_glue.c`——那个 TU 一动就换分支，§271/§279）
   把 24 个字的回调表**每一格都填成 `s3_wpa_cb_stub`**，
   **只把 word 18（+0x48）留 0**，因为那一格库是当**布尔量**读的（§277）。
3. `esp_wifi_register_wpa_cb_internal(&表)` → 返回 0。

真板实测：

```
[wifi] register_wpa_cb=0
[wifi] set_mode(STA)=0
wifi|Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2
[wifi] set_config=0                       ← ★ 从来没成功过
[phy] OSI _phy_enable 回调进入              ← esp_wifi_start() 已经在跑 PHY 初始化
[phy] g_phyFuns=0x3fcef3d8
[phy] calling register_chipv7_phy(mode=0)  ← 现在卡/慢在这一步
```

也就是说：§259 以来"闭源库任一调用即崩"的整条路彻底通了，现在站在
`esp_wifi_start()` 的 PHY 初始化里。

**下一步**：`register_chipv7_phy(mode=0)` 之后要么需要更多 OSI 回调（`_phy_*`、
定时器、`_rtc_*`），要么耗时超过监视窗口；先加更长窗口 + 在 `_phy_enable`
后面加信标，看它是"慢"还是"卡"。

**注意（踩过的坑）**：
- 这一步**不能**用 `wifi_glue.c` 里的静态数组做回调表：多 24 个字的 .bss 会让
  堆基址/代码布局变化，实测直接退化成"卡在 `esp_wifi_init_internal` 的
  `[lockok]`"。放在 windowed 岛的 TU 里、`.text`/`.bss` 与 `wifi_glue.c` 无关才行。
- 往自己 cfg 的第 148 字节写 1 **无效**：`wifi_set_config_process` 里的 a5 是库
  自己那份内部 config，不是我们传进去的那块（§279 实验）。

### §281 新的顶头问题：`register_chipv7_phy()` 卡在 `ram_iq_est_enable()` 的硬件轮询上

`esp_wifi_set_config` 已经能过（§280），`esp_wifi_start()` 进来后首件大事就是
OSI 的 `_phy_enable` → 我们的 `s3_win_phy_enable()` → `register_chipv7_phy(mode=0)`，
**卡住不返回**（串口停在 `[phy] calling register_chipv7_phy(mode=0)`，等了 45s 也没
后文，说明不是"慢"）。

JTAG（跑到卡住 → 杀监视进程 → `ocboot.sh` → `resume`/等/`halt`）读到：

```
Target halted, PC=0x420547FD        → ram_iq_est_enable+0xad
420547f8: memw ; l32i a3, a10, 0            ; a10 = 0x60006174（字面量 0x4205474c）
420547fd: bnone a3, a9, 420547e5            ; a9  = 0x00010000（字面量 0x4204dca8）
420547e5: memw ; l32i a3, a11, 0            ; a11 = 0x6001c08c
          extui a3, a3, 12, 7 ; bltu a12(69), a3, 出去
```

也就是说它**死等 `0x60006174` 的 bit16**（"IQ 估计完成"），备用出口是
`0x6001c08c[18:12] > 69`，两个条件都不来 → 纯硬件条件不满足，不是软件超时逻辑。

含义：PHY/BB 的时钟或 RF 供电没真正打开（读回恒 0），或者我们多调的那句
`phy_bbpll_en_usb(1)`（§205 为了保住 USB-PHY 加的）在**这个上下文里**反而破坏了
BBPLL/I2C 状态。

**下一步（按代价排序）**
1. 卡住时 JTAG 读 `0x60006174` / `0x6001c08c`，确认是"恒 0"还是"bit16 不来"。
2. 试 `S3_PHY_CAL_MODE=2`（`PHY_RF_CAL_NONE`）看是否绕开 IQ 估计。
3. 试**不调** `phy_bbpll_en_usb(1)`（只在 `register_chipv7_phy` 之后再补一次），
   对比是否还卡在同一个 PC —— 这是 §205 折衷方案的反向验证。
4. 历史证据：`S3_PHY_BISECT=1` 的 `make bringup` 镜像里这一步是**能过的**
   （文档里 `[phy] register_chipv7_phy rc=0` 就是那么来的），所以分歧点一定在
   "bisect 路径 vs 正常路径"的调用顺序/上下文差异上，值得先把 bisect 的 6 个
   子步骤和 `s3_win_phy_enable()` 逐句对照一遍。

### §282 `register_chipv7_phy` 卡死的排查记录（电源/时钟/复位全都对，PHY 那块寄存器仍是死的）

有 IDF 源码在手（`/media/winger/inst_26/esp/esp-idf`），可以逐条比对了。

**IDF 在 `register_chipv7_phy` 之前做的全部事情**（`components/esp_phy/src/phy_init.c`
的 `esp_phy_enable()`→`esp_phy_load_cal_and_init()`）：

1. `esp_phy_common_clock_enable()` = `wifi_bt_common_module_enable()`
   → `SYSCON_WIFI_CLK_EN(0x60026014) |= SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M (0x78078)`（S3 不是
   `SOC_MODEM_CLOCK_IS_INDEPENDENT`，走老的那条路）
2. `phy_module_enable()` → `periph_ll_phy_calibration_module_enable_clk_clear_rst()`
   （`|= SYSTEM_WIFI_CLK_PHY_EN_M (0x400000)`）+ RNG 时钟 +
   `periph_ll_wifi_module_enable_clk_clear_rst()`（`|= SYSTEM_WIFI_CLK_WIFI_EN_M`）
3. `phy_init_param_set(1)`  ← 只有 `SOC_PHY_COMBO_MODULE`（S3 是 WiFi+BLE 组合片）
4. `phy_bbpll_en_usb(true)`（`CONFIG_ESP_PHY_ENABLE_USB`）
5. `register_chipv7_phy(init_data, cal_data, cal_mode)`

我们的 `s3_win_phy_power_on()`/`clock_enable()` 覆盖了 1、2、4，掩码也对得上
（`0x78078F` ⊇ `0x78078`，另外补了 `0x400000`）。

**JTAG 实测（卡住时）**：

| 寄存器 | 值 | 结论 |
|---|---|---|
| `0x60026014` SYSCON_WIFI_CLK_EN | `0xfffce7bf` | WiFi/BT/PHY 时钟位全置 1 ✓ |
| `0x60008090` RTC_CNTL_DIG_PWC | `0x00545010` | **bit18 = WIFI_FORCE_PU = 1**、bit17 = FORCE_PD = 0 ✓ 电源域是开的 |
| `0x60008094` RTC_CNTL_DIG_ISO | `0xaa805080` | bit28 = 0 ✓ 已解隔离 |
| `0x60026018` SYSCON_WIFI_RST_EN | `0` | 复位已释放 ✓ |
| `0x60006000` PHY 基址 | `0x0e828040` | 这块**能读**，非 0 |
| `0x60006174` 被轮询的那个 | **0** | 死 0；`0x60006170/0x60006178` 也都是 0 |
| `mww 0x60006174 0x10000` 后回读 | 仍为 0 | **写不进去**（只读/无响应） |

补充事实：
- `RTC_CNTL_WIFI_FORCE_PU` 是 **BIT(18)**，不是 BIT(16)（`soc/esp32s3/register/soc/rtc_cntl_reg.h`）；
  所以 §282 里试过的"补 PU 位"其实一直是开的，那次改动已回退。
- `S3_PHY_CAL_MODE=2`（`PHY_RF_CAL_NONE`）**不能**绕开这一步，照样卡在同一 PC。
- 补上 IDF 的 `phy_init_param_set(1)`（combo 模式）也**没有**变化。
- `make wphy` / `make wphyflash` 这两个目标**本身就是坏的**（链 `w2c.o` 时缺
  `os_event_*`），和本次无关，不去追。

**判断**：PHY 的"模拟/BB 寄存器窗口"整体没有响应（0x60006174 一带恒 0 且写不进），
而 APB 侧的 `0x60006000` 是活的。最可能是**还缺一个把 PHY 模拟域从复位/掉电里真正
拉起来的动作**（IDF 里由 `phy_module_enable` 的 RNG 时钟 + 一步步 `periph_ll_*` 完成，
我们只搬了一部分），或者这块 libphy 期望先有 `phy_init_param_set`/`esp_phy_apply_phy_init_data`
的某种组合。

**下一步建议（按性价比）**
1. 逐位对照 `syscon_reg.h` 把 `syscon_wifi_clk_en` 的目标值算出来（不要再用猜的掩码），
   写完回读确认每一位都真的置上了。
2. 在 `register_chipv7_phy` 之前先调一次 `phy_wakeup_init()` / `ram_phy_wakeup_init()`
   （这两个符号库里都有：`0x4204d604` / `0x4204d9b0`），看是否能把模拟域叫醒。
3. 换思路：把 IDF 的 `esp_phy` 组件源码直接编进我们镜像（`phy_init.c` 里真正需要
   替换的只有 nvs 存储那几处），这样上面 1~5 步全按 IDF 走，不用手工复刻。

### §283 板子 USB 又没了（PHY 老毛病复发）—— 需要人工断电重插

想用历史镜像对比"当年的 `register_chipv7_phy rc=0`"，于是构建了分步诊断镜像：

```
make C6_ROOT=/home/winger/c6 S3_PHY_BISECT=1 wifi      # 成功
./tools/flashmon.sh build/wifi/c6os.bin 40             # 刷完，没有 bisect 输出
./tools/flashmon.sh build/wifi/c6os.bin 35             # → "没有串口"
lsusb | grep 303a                                      # → 设备没了
```

也就是说这个 `S3_PHY_BISECT=1` 镜像一进 PHY 初始化就把内部 USB-Serial-JTAG
关掉了 —— 正是 §205 描述过的那个失败模式（`register_chipv7_phy()` 会顺手关掉
USB PHY，除非先 `phy_bbpll_en_usb(1)`）。现在 `303a` 不在 `lsusb` 里，
**只能人工断电重插**（软件无法恢复）。

顺带记两条：
- `make wphy` / `make wphyflash` **目标本身是坏的**：链 `build/phy/c6os.elf` 时
  `w2c.o` 引用 `os_event_set/clear/wait/destroy/get` 全都 undefined（那个最小镜像
  不链内核）。这是既有问题，和本次改动无关，不去修。
- 以后刷"会跑 PHY 初始化"的镜像前，先把 USB 恢复路径（`s3_win_usb_reclaim()`）
  确认在 PHY 失败路径上也会被调用，否则一刷就失联。

**恢复后要做的第一件事**：重新刷回 `https` 镜像（现在板上是 bisect 镜像，一上电
又会把 USB 关掉）。

### §284 FE（0x60006000 = DR_REG_FE_BASE，"front end"）这块寄存器窗口在进库之前就是死的

在 `s3_win_phy_enable()` 里加了两行读寄存器（`0x60006174` / `0x60006000`），
**紧跟在 `s3_win_phy_power_on()` 之后、还没调 `register_chipv7_phy`**：

```
[phy] FE 0x60006174=00000000 0x60006000=06828040（power_on 之后）
[phy] calling register_chipv7_phy(mode=0)
```

对照 IDF 的 `soc/esp32s3/register/soc/reg_base.h`：

| 地址 | 归属 |
|---|---|
| `0x60006000` | `DR_REG_FE_BASE`（PHY/RF 前端） |
| `0x6001CC00` | `DR_REG_NRX_BASE` |
| `0x6001D000` | `DR_REG_BB_BASE` |

所以 `register_chipv7_phy()` 里 `ram_iq_est_enable()` 死等的是 **FE 的一个状态位**，
而这块在库执行之前就已经恒 0 了 —— 不是库把它弄坏，是我们**没能把它叫醒**。

已经排除的（都有实测）：
- 电源域：`RTC_CNTL_DIG_PWC` 的 `WIFI_FORCE_PU` 是 **BIT(18)**（不是 16），实测已为 1；
  `WIFI_FORCE_PD`=0、`WIFI_PD_EN`=0 ✓
- 时钟：`SYSTEM_WIFI_CLK_EN = 0xfffce7bf`，IDF 要求的三组掩码
  `WIFI_BT_COMMON_M=0x78078F`、`PHY_EN_M=0x400000` 全在里面 ✓
  （S3 的 `SYSTEM_WIFI_CLK_WIFI_EN`/`BT_EN` 在 IDF 头里就是 `0x0`，没有额外位）
- 隔离/复位：`RTC_CNTL_DIG_ISO` bit28=0 ✓、`SYSCON_WIFI_RST_EN`=0 ✓
- `S3_PHY_CAL_MODE=2`（`PHY_RF_CAL_NONE`）：照样卡
- `phy_init_param_set(1)`（IDF 对 combo 片会调）：照样卡
- `phy_wakeup_init()`：**在它里面就卡住**（连我们的打印都出不来）
- 去掉 W0 阶段那次提前的 `s3_win_phy_cal()`（`phy_get_rf_cal_version`）：照样卡
- JTAG `mww 0x60006174 0x10000` 后回读仍是 0 → 这个地址写不进去

**下一步（下一轮从这里接着干）**
1. 先拿一个"已知能用"的参照物：本机上有现成的 ESP-IDF 工程
   `/media/winger/inst_26/esp/xiaozhi`（`CONFIG_IDF_TARGET="esp32s3"`，`build/` 里有
   `xiaozhi.bin`、`flash_args`：bootloader@0x0、ptable@0x8000、app@0x20000）。
   刷进去跑起来，用 JTAG 读 `0x60006174`/`0x60006000`：
   - 若 IDF 下这些地址也是 0 → 说明我们对"这个寄存器该是什么"的理解有偏差，
     该换判据；
   - 若非 0 → 我们的初始化确实缺一步，逐位对比 IDF 的 `esp_phy_enable()` 侧寄存器。
   实验完立刻刷回本仓库镜像（`flashmon.sh build/wifi/c6os.bin`）。
2. 更彻底：把 IDF 的 `components/esp_phy/src/*.c` 编进镜像（nvs/esp_log/esp_timer/
   efuse 用桩实现替掉），让 PHY 初始化序列**就是 IDF 的**，不再手工复刻。
3. 备选：`esp_phy_apply_phy_init_data()`（镜像里有，`0x4203fbd0`）在
   `register_chipv7_phy` 之前先调一次看看。

### §285 ★参照实验：刷一个**能正常跑 WiFi 的 IDF 固件**，用 JTAG 抓同一批寄存器对比

用的参照物是本机现成的 ESP-IDF 工程 `/media/winger/inst_26/esp/xiaozhi`
（`CONFIG_IDF_TARGET="esp32s3"`，`build/flash_args`：bootloader@0x0、ptable@0x8000、
app@0x20000；**不刷 8MB 的 assets 分区**，省时间也不影响 PHY）：

```
cd /media/winger/inst_26/esp/xiaozhi/build
sudo -n esptool --chip esp32s3 -p /dev/ttyACMx --before default-reset \
     --after watchdog-reset write-flash 0x0 bootloader/bootloader.bin \
     0x8000 partition_table/partition-table.bin 0x20000 xiaozhi.bin
```
跑 14s 后 JTAG `halt` 读（PC=0x40382D7E，WiFi 已经起来）：

| 寄存器 | IDF 正常系统 | 我们（卡在 `ram_iq_est_enable` 时） |
|---|---|---|
| `0x60026014` SYSCON_WIFI_CLK_EN | `0xffffdfdf` | `0xfffce7bf`（改了以后 `0xffffffff`） |
| `0x60026018` SYSCON_WIFI_RST_EN | `0x00003e08` | `00000000` |
| `0x60008090` RTC_CNTL_DIG_PWC | `0x00000000` | `0x00545010` |
| `0x60006174` FE（被轮询的状态位） | **0** | 0 |
| `0x6001c08c` FE 隔壁那个"计数器" | **0x03fff9f3**（在走） | **0** |

**两条关键结论**：
1. `0x60006174` 在**正常系统里也是 0** —— 所以它根本不是"PHY 死没死"的判据（§284 的
   推断作废）。`ram_iq_est_enable()` 那个循环真正的出口是**另一个**：
   `0x6001c08c[18:12] > 69`。
2. 那个"计数器"在 IDF 下是活的（`0x03fff9f3`），在我们镜像里恒 0。它才是分水岭。

**试过但没用的**：
- 把 `SYSCON_WIFI_CLK_EN` 写成 `0xffffffff`（比参照的 `0xffffdfdf` 还全）→ `0x6001c08c`
  仍然 0，而且 **JTAG 写 `0x6001c08c` 也写不进去**（`mww` 后回读仍 0）⇒ 这块不是
  "少一个 SYSCON 时钟位"，是整个块没被打开。
- 把 PHY 初始化挪到 `esp_wifi_init_internal()` 之后（IDF 的天然顺序：
  init → esp_phy_enable）→ 依然卡在同一个 PC；而且在 init 内部被 OSI `_phy_enable`
  回调进来时，`0x6001c08c` 也还是 0。
- 顺序改成"先只上电（power_on+bbpll+romfuncs）→ bringup → 完整 PHY 初始化"也一样。
- （都试完已回退，`win_island.c` 保持 §280 提交时的样子。）

**判断**：`0x6001c000` 那块（IDF 的 `reg_base.h` 里**没有**它，夹在 LEDC 0x60019000 和
NRX 0x6001CC00 之间，就是 WiFi MAC）的时钟不是 SYSCON 给的，八成来自 **PHY/BB PLL**，
也就是 `register_chipv7_phy()` 自己后面的 `rf_init`/`bb_init` 才会打开 —— 于是
"要先有它才能过 IQ 估计"成了鸡生蛋问题。IDF 能过，说明它有一条我们没复刻的路径
把这块提前点亮。

**下一轮方向（按性价比）**
1. 反汇编 `wifi_init_process`（`0x420368e0`，`esp_wifi_init_internal` 的实际实现）
   找它有没有写 `0x6001cxxx` / `0x60026xxx`；有就直接搬过来。
2. 或者干脆把 IDF 的 `esp_phy` 源码编进镜像（nvs/esp_log/esp_timer/efuse 打桩），
   让整条 PHY 初始化就是 IDF 的，彻底不再手工复刻（§284 的备选方案，仍然是最终解）。
3. 参照物实验的套路很好用（刷 IDF 工程 → JTAG 读 → 立刻刷回本仓库镜像），
   以后**任何**"我们 vs 正常系统"的寄存器差异都可以这样拿，比继续猜快得多。

### §286 补一条：`0x6001c0xx` / `0x60026xxx` 这些寄存器**只有 libphy 自己会碰**

把整份反汇编（`objdump -d` 后 23 万行）扫了一遍"谁 `l32r` 到
`0x6001c0xx` / `0x60026xxx`"：**73 处命中全部在 libphy 里**
（`phy_set_signal_test`、`ram_pbus_force_mode`、`phy_11p_set`、`phy_get_cca` …），
`libnet80211` 的 `wifi_init_process`（`esp_wifi_init_internal` 的实际实现，`0x420368e0`）
一处都没有。

也就是说：**没有"应用侧多调一个 API 就能点亮 MAC 块"这回事**，点亮它的代码就在
`register_chipv7_phy()` 里面（`rf_init` / `bb_init` 那几步），我们卡在它前面的
`ram_iq_est_enable()` 上 —— 这是真正的鸡生蛋。

所以下一轮不必再试"再开某个时钟位/再调某个 IDF API"这条路了，只有两条路可走：
1. 反汇编 libphy 里 `register_chipv7_phy` 到 `ram_iq_est_enable` 之间的调用链，
   找它凭**什么条件**才去点亮 MAC（很可能是某个 `get_chip_version()`/effuse 分支）；
2. 直接把 IDF 的 `esp_phy` 组件源码编进镜像（打桩 nvs/esp_log/esp_timer/efuse），
   让序列不再是我们复刻的 —— 这条更稳，建议下一轮直接上。

### §287 参照实验（二）：**RTC_CNTL_CLK_CONF 明显不一样** —— 我们根本没有 IDF 的二级 bootloader 时钟配置

把参照固件再刷一遍，对着同一批寄存器读（这次多读了时钟配置）：

| 寄存器 | IDF 正常系统 | 我们 |
|---|---|---|
| `0x60026000` SYSCON 侧 CPU/外设配置 | `00000001` | （待读） |
| `0x60008074` **RTC_CNTL_CLK_CONF** | **`30c8069c`** | **`1158321c`** |
| `0x60008070` PLL 配置 | `00000000` | `00000000` |
| `0x6001c08c` MAC 块计数器 | `03fff9f5`（在动） | `00000000` |

按 `soc/esp32s3/register/soc/rtc_cntl_reg.h` 的 CLK_CONF 字段位：

- bit29 `FAST_CLK_RTC_SEL`：参照 **1**，我们 **0**；
- bit28 `XTAL_GLOBAL_FORCE_NOGATING`：都是 1；
- 参照比我们多 `0x20800480`（bit29 / bit23 / bit10 / bit7），
  我们比参照多 `0x11103000` 那一片（含 CK8M 相关位）。

**根因线索**：`Makefile` 里那句注释说得对 —— **我们的镜像没有二级 bootloader**
（`S3_FLASH_AUTO_MAP` 就是为此存在）。而 IDF 的二级 bootloader 第一件事就是
`bootloader_clock_configure()`：选时钟源、开 SPLL、配 CK8M、把 CPU 拉到 80MHz。
这些**谁都没替我们做**，ROM bootloader 只做最小配置。

也就是说：PHY 初始化卡住的真正原因，很可能是**芯片的 RTC/CPU 时钟树还是 ROM 的默认状态**，
而 `register_chipv7_phy()` 里 `ram_iq_est_enable()` 依赖的 MAC 块计数器正是挂在
某个（没开出来的）时钟域上。

**试过但没用的（本轮）**：
- 让 `_phy_enable` 回调第一次进来只"上电"、把完整 PHY 初始化推迟到
  `esp_wifi_init_internal()` 之后（IDF 的天然顺序 init→phy）→ **init 自身卡在
  `[lockok]`**（库本来就指望这个回调返回时 PHY 已经好了）。改动已回退。
- 把 `SYSCON_WIFI_CLK_EN` 写成 `0xffffffff` → MAC 计数器仍 0（§286 已记）。

**下一轮（明确的两条路）**
1. **把 IDF 的 `rtc_clk` 初始化搬过来**：`components/esp_hw_support/port/esp32s3/rtc_clk.c`
   里的 `rtc_clk_init()` / `rtc_clk_cpu_freq_set_config(RTC_CPU_FREQ_80M)`，
   在 kernel 启动早期（进 PHY 之前）执行一次，让 `0x60008074` 与参照一致。
   这是目前证据最硬的一条。
2. 若仍不行，再上"把 IDF `esp_phy` 整组件编进镜像"的终极方案（§286 备选）。

### §288 试一下"BBPLL 上电"（IDF bootloader 的 clk_ll_bbpll_enable）—— 位本来就是 0，MAC 仍然死

`clk_ll_bbpll_enable()` 就三行：清 `RTC_CNTL_OPTIONS0`（0x60008000）的
`BB_I2C_FORCE_PD(6)` / `BBPLL_I2C_FORCE_PD(8)` / `BBPLL_FORCE_PD(10)`。
在 `s3_win_phy_power_on()` 里补上，并在调 `register_chipv7_phy` 前把两个值打出来：

```
[phy] MAC 0x6001c08c=00000000 OPTIONS0=1c00a000
[phy] calling register_chipv7_phy(mode=0)
```

**OPTIONS0 = 0x1c00a000，bit6/8/10 本来就是 0** —— 也就是说 BBPLL 早就没被强制关电。
清完照样 `0x6001c08c` 恒 0，`register_chipv7_phy` 还是卡在 `ram_iq_est_enable()`。

结论：**"BBPLL 没上电"这个假设不成立**。要么是 BBPLL 没被*配置*（分频/锁定，
`clk_ll_bbpll_set_config` 那几个 regi2c 模拟写没人做过），要么 MAC 块的时钟根本不是
从 BBPLL 来的。无论哪种，都得走 §287 定的第 1 条路：**把 IDF 的 rtc_clk 初始化整段搬过来**。

顺带确认了两件事：
- `clk_ll_bbpll_enable` 只需要一次寄存器操作，成本可忽略（已留在代码里，无害）；
- 打印 `0x6001c08c` / `OPTIONS0` 的探针也留下了，以后每次进 PHY 都能看见。

### §289 ★ 让 c6os 改用 **IDF 真正的二级 bootloader** 引导 —— 成功了：内核能在 IDF 的时钟树/cache/MMU 环境下启动

前面 §277~§288 一直卡在同一件事上：**我们的镜像自己就是二级 bootloader**（ROM 直接
从 0x0 加载它），所以 IDF 二级 bootloader 干的活（`bootloader_clock_configure()`
→ `rtc_clk_init()` 选时钟源/开 SPLL/配 CK8M、cache & MMU、PSRAM…）**没有任何人做**。
§287 测出的 `RTC_CNTL_CLK_CONF` 差异（0x1158321c vs 0x30c8069c）就是这么来的。

**改法（本轮实现，已生效）**

1. `port/s3/link.ld`：flash 段的 LMA 改成 `AT (0x20 + __app_base)` /
   `AT (0x10000 + __app_base)`，VMA 一律不动（同余关系保持）。`PROVIDE(__app_base = 0)`
   保证默认行为不变。
2. `Makefile`：新增 `S3_APP_BASE`（默认 `0x0` = 老行为），链接时
   `-Wl,--defsym=__app_base=$(S3_APP_BASE)`，并把它传给 `tools/chkimg.py`
   （比较时先把 `p_paddr` 减去应用基址）。
3. `tools/flashbl.sh`：把 **参照工程那套 IDF 二级 bootloader** 写进去
   （`bootloader.bin@0x0` + 分区表`@0x8000` + 我们的应用`@0x20000`），再擦掉 otadata
   让 bootloader 确定地启 `ota_0`。
4. 构建：`make ... S3_APP_BASE=0x20000 https`。

**结果：真的起来了！** 串口完整输出（`boot:0x8 SPI_FAST_FLASH_BOOT` 之后
`load:0x3fce2820` … `entry 0x403c88f4` 是 IDF bootloader，再往后是我们的内核）：

```
c6os s3 boot
M1 icache ok
flash: text va=0x42010000 lma=0x00030000 …   ← LMA 已经是基址后的 0x30000 ✓
flash: mmu(text)=0x00000003 mmu(rodata)=0x00000002
flash: probe(42)=0x0000002a                    ← flash 读实测正确 ✓
M5 tasks ok, starting scheduler
T1 call0 任务进入 windowed 主流程
```

顺带测量（**这次是 IDF bootloader 之后的寄存器**）：

| 寄存器 | 参照(IDF) | 我们(ROM 直接引导) | 我们(IDF bootloader 引导) |
|---|---|---|---|
| `0x60008074` CLK_CONF | `30c8069c` | `1158321c` | **`10c8069c`** ← 已经很接近了 |
| `0x6001c08c` MAC 计数器 | 活的 | 0 | 仍然 **0**（这时 PHY 还没初始化，属正常） |
| `0x60026014` WIFI_CLK_EN | `ffffdfdf` | `fffce7bf` | `fffce030` |

**新问题（下一轮的主线）**：走 bootloader 这条路时，windowed 的 WiFi 任务在 `T1`
之后**立刻**死在 `_vec_default`，JTAG 抓到的现场（`0x50000100`）：

```
标记 0000bad0  EXCCAUSE=0000001d  EXCVADDR=ffffffe0  EPC1=400442a7(ROM 里的代码)
PS=00060e10 (EXCM=1, INTLEVEL=6)  a0=0  a1=3fcbeda0  WINDOWBASE=0
```
同样的镜像走 **ROM 直接引导**（`S3_APP_BASE=0x0`）时，同一个任务能一路走到
`[phy] calling register_chipv7_phy(mode=0)` 才卡住（§281）。所以差异完全是
"谁先启动"造成的，不是我们的 windowed 主流程本身。

可疑点（下一轮按顺序查）：
1. **窗口溢出/下溢向量**：ROM 引导时那 6 个向量由 ROM 处理；我们 `_start` 里
   `wsr vecbase` 换成自己的表以后，这 6 个槽位落到了 `_vec_default`。
   若 IDF bootloader 让 ROM 库进入更深的 `call8` 层级，就必炸。
   查法：在 `_vec_default` 里把 **%index/向量号**也存进 RTC（现在只有 EXCCAUSE），
   或者干脆补一套 xtos 风格的 `_WindowOverflow4/8/12`、`_WindowUnderflow4/8/12`。
2. **高优先级中断**：`PS.INTLEVEL=6` 说明异常发生在异常/中断上下文里；
   IDF bootloader 可能留下了我们没接管的中断源（RTC/WDT/PMU）。
3. 我们的 `_vec_level1` 是否在新环境下仍被正确挂上。

**目前默认仍是老路径**（`S3_APP_BASE=0x0`，已验证行为不变，板子上跑的就是它）。
新路径随时可用 `S3_APP_BASE=0x20000 + tools/flashbl.sh` 复现。

### §290 bootloader 路径上 T1 后就死：**寄存器窗口溢出把寄存器 spill 到坏的 a5 (−16) → double exception**

§289 发现"走 IDF 二级 bootloader 时 windowed 任务在 `T1` 之后立刻死"，本轮把它查清了。

**手段**：向量表的 4 个 default 槽位各自先 `movi a3, <向量号>` 再 `j _vec_default`，
`_vec_default` 入口第一件事把 a3 存到 RTC `+0x124`（第 10 个字）。JTAG 读 `0x50000100`：

```
0000bad0 0000001d ffffffe0 400442a7 00060e10 00000000 3fcbeda0 00000000
00005555 0000000f
                        ↑ +0x124 = 0x0F → 向量 15 = double exception
```

现场含义：
- **`0x0F` = double exception**（CPU 在 `PS.EXCM=1` 的异常上下文里又吃了一次异常）；
- `EXCVADDR = 0xffffffe0`（**−32**）、`EXCCAUSE = 0x1d`；
- `PS = 0x00060e10` → **`PS.INTLEVEL = 6`、`PS.EXCM = 1`**。

对照 xtos 的窗口溢出处理体 `_WindowOverflow4`：`s32e a0, a5, -16` ——
**访问地址 = a5 − 16**。`a5 = −16` 时正好 = `0xffffffe0`，和 EXCVADDR 完全对上。
也就是说：**窗口溢出处理体被触发时，a5（下一个窗口的栈指针）是个垃圾值 −16**，
spill 到 `0xffffffe0` 又一次异常，而处理体本身跑在 `PS.EXCM=1` 里 → 直接 double exception。

即 §202 那个老问题的变种：**ROM/IDF bootloader 留下的窗口里 a1 不是合法栈指针**。
我们 `_start` 里虽然有"窗口预热"（§236），但 IDF bootloader 的调用链会再轮转出
新的窗口状态，预热过的那几个窗口不够用。

**顺带排除**：`INTERRUPT_CORE0` 状态里确实挂着 WiFi/BT 的源
（`INTR_STATUS_0(0x60010000) = 0x000000f2` → 源 1/4/5/6/7 挂起，即
WIFI_MAC_NMI / BT_MAC / BT_BB / BT_BB_NMI / RWBT），一度怀疑是 level-6 中断引起的；
在 `_start` 里 `rsr/wsr intenable` 屏蔽这 5 个源之后**现场一字不差**
（`+0x124` 仍是 `0x0F`，EXCVADDR 仍是 `0xffffffe0`），所以**跟中断无关**，
排除掉。

**下一轮的两条候选修法**
1. `_win_ovf4/8/12`、`_win_unf4/8/12` 入口先做**栈指针合法性检查**：a5（或 a9/a13）
   落在我们 DRAM 窗口 `0x3FCA0000..0x3FCEEFFF`、并且 4 字节对齐、且
   `a5-64 >= 0x3FCA0000` 才 spill；否则把现场（a5/窗口号/EPC1）写 RTC 后直接停住，
   这样至少能看清是谁在用坏窗口，而不是变成 double exception 什么都留不下。
2. 干脆在 `s3_win_call_wifi_main` 这个跳板进入 windowed 之前，**把 4 个窗口的
   a1 全部重设成我们自己分配好的栈顶**（比 §236 的预热更强：不只是"合法"，
   而是"我们自己的、后面还会用到的"），把 ROM/bootloader 的窗口状态彻底切断。

**默认路径没动**：`S3_APP_BASE=0x0`（ROM 直接引导）行为与之前完全一致
（板子上现在跑的就是它，`[phy] calling register_chipv7_phy(mode=0)` 停住）。

### §291 抓住溢出处理体：`[a1-12]` 里的"上一级 sp"是 **0**（a1 本身是合法栈地址）

在 `_win_ovf8` 的 `l32e a0, a1, -12` 之后加一句 `beqz a0, .Lovf8_bad`，坏分支把
现场写 RTC `0x50000120` 起（诊断体放在 `.text` 里，`.vector` 只有 1KB 塞不下）。

走 IDF bootloader 引导，JTAG 读回：

```
0x50000120  3fcbeda0    ← a1（帧指针，合法栈地址）
0x50000124  3fcbed80    ← a9（下一个窗口的 sp）
0x50000128  400442a7    ← EPC1
0x5000012c  00000000    ← WINDOWBASE
0x50000130  00005555    ← WINDOWSTART（8 个 call8 帧都活着）
0x50000134  00060e10    ← PS（INTLEVEL=6, EXCM=1）
0x50000138  00000b08    ← 记号：确认确实进了 _win_ovf8 的坏分支
```

**结论**：`a1` 是好的，但 **`[a1-12]`（按 ABI 应该放着"上一级的 sp"）是 0**。
xtos 的处理体接着要 `s32e a4, a0, -32` → 写 `0xffffffe0` → 在 EXCM 里二次异常 →
double exception（§290 的 EXCCAUSE=0x1d / EXCVADDR=0xffffffe0 就是这么来的）。
根源是这条调用链里**有一帧不是真正的 windowed 帧**（我们的 call0 → 跳板/W2C 壳
手工造的帧就没有 `[fp-12]` 这个槽），调用链一深就塌。

**重要副作用（已回退）**：加了 `beqz` 守卫之后，**ROM 直接引导那条路也死在 T1 了**。
说明同样的 `[a1-12]==0` 在 ROM 路径上也会出现，而原代码"往 0xffffffe0 写"
在那边其实**没炸**（写进去被丢掉，`rfwo` 重试后继续跑），于是带了守卫反而
把本来能过的路挡死。所以 §291 的诊断代码只用来看现场，已经 `git stash drop`
回退，树里没留。

**下一轮真正该做的**：让 call0→windowed 的**每一帧都符合 ABI 的帧布局**
（尤其是把上一级 sp 放在 `[fp-12]`），或者进 windowed 之前先把整个窗口组
spill 干净、只留一个活窗口，从根上避免"深链溢出"。

## §292 PHY 初始化成功的钥匙 = IDF 的二级 bootloader（本轮突破）

### 做对的事

1. **irq.S 的窗口溢出/下溢处理体加了"回退"**：`_win_ovf8` 里
   `l32e a0, a1, -12`（取上一级 sp）如果拿到 0，就用本帧 `a9` 顶上；
   `_win_unf8` 对称地也回退。这样把寄存器溢写到 `[a9-32..-20]`，
   和恢复侧完全自洽——不再像原来那样写到 `0xffffffe0`，在 EXCM 里
   二次异常（§290/§291）。
2. 有了这个回退，**走 IDF 真 bootloader 启动**（`S3_APP_BASE=0x20000`
   + `tools/flashbl.sh`）的那条路终于越过了 `T1`，一路跑到 PHY：

```
T1 call0 任务进入 windowed 主流程
[wifimain] id=1 tick=1
W0 phy start
[phy] CLK_EN=ffffdfdf MAC 0x6001c08c=03fffa00     ← MAC 寄存器块"活了"
[phy] calling register_chipv7_phy(mode=0)
[phy] register_chipv7_phy rc=1                      ← §281 的挂死消失
W1 bringup
B0 osi0=... B1 slot13=... B2 cfg@...
[lockok] t=1 pr=0 ok=1
[pp] pp rom version: e7ae62f
[net80211] net80211 rom version: e7ae62f
[mtxnew] #1 #2 [semnew] #1 [task] 库建任务 fn=[give] t=2 sem#1
[recv] t=2 qh=3fcc450c tk=ffffffff
```

   **结论（推翻 §287 的失败结论的根因猜测）**：ROM 路径之所以
   `register_chipv7_phy` 卡在 `ram_iq_est_enable`，是因为 MAC/PHY
   寄存器块（`0x6001c000` 段）根本没被点亮——JTAG 实测 ROM 路径该段
   **全部读 0**，而 bootloader 路径读得到真实复位值 `0x03fffa00`。
   这是"块没时钟/没供电"的典型表现，不是 libphy 参数问题。

### 排除掉的假设（都做了实验）

* **PHY init data 不一致**：从参照工程 `xiaozhi.elf` 里把
  `phy_init_data`（0x3c28a507，128 字节）抠出来和我们的
  `s3_phy_init_data` 逐字节比 —— **完全相同**。
* **`SYSCON_WIFI_RST_EN`(0x60026018) = 0x3e08**（参照值）：写入生效，
  MAC 仍读 0。
* **`SYSCON_WIFI_CLK_EN`(0x60026014) = 0xffffdfdf**（参照精确值）：写入
  生效，MAC 仍读 0。我们的 `|= 0x78078F | 0x400000` 本来就覆盖了
  `WIFI_BT_COMMON` 和 `PHY_EN` 两组必须的位。
* **`RTC_CNTL_DIG_PWC`**：我们置的全是 FORCE_PU（上电），没有置任何 PD。
* **把 bootloader 路径那 6 个不同的 RTC 寄存器值抄到 ROM 路径**
  （0x60008044/8074/8084/809c/80b4/80e8）：MAC 仍读 0 —— 说明差别不是
  "几个寄存器的终值"，而是 bootloader 那段**时序**（BBPLL/时钟序列）。

### 下一步的墙：PHY 成功之后，任务切换里二次异常

* RTC 记录：`cause=3(LoadStoreError)`，`EXCVADDR=0x403816a8`（**代码地址**，
  正好是 `ppTask`(0x403816d8) 往前 0x30），`EPC1=0x40379569`，
  `PS=0x0006081f`（INTLEVEL=15）、`WINDOWBASE=9`、`WINDOWSTART=0x200`。
* `EPC1` 落在 `port_task_switch+0xc1`，那一段正是
  `rsr.windowbase` / `wsr.windowbase` / `wsr.windowstart` 的
  "切窗口 + 重设 WINDOWSTART" 逻辑；而 INTLEVEL=15 是 `rsil a5,15`
  造成的，**窗口异常是 level-1，被 15 屏蔽 ⇒ 直接走二次异常**
  ⇒ 落进 `_vec_default` 而不是我们的 `s3_trap`。
* 也就是说：**bootloader 路径下，库创建 `ppTask` 之后第一次任务切换**，
  窗口状态和我们的 `port_task_switch` 对不上。下一步要么让切换时
  WINDOWBASE/WINDOWSTART 的改动不触发异常（先 `rsil` 回 1），
  要么把"库任务"整个跑在自己的窗口基址上。

## §293 bootloader 路径复现：这次是"栈跑飞"

同一条 bootloader 路径再跑一次（`flashbl.sh`，20 秒抓串口），这次拿到的是
我们自己 `s3_trap` 打出来的记录（不是二次异常了 —— §292 的回退生效）：

```
[recv] t=2 qh=3fcc44dc tk=ffffffff
b*** TRAP sp=3fca0565 cause=00000000 pc=40048c6e ps=00060410 vaddr=00000000 callinc=2 a0=00060000
```

* `cause=0` = 非法指令，`pc=0x40048c6e` 在 ROM 里（返回地址飞了），
  `a0=0x00060000` 明显是被当指令地址取了的数据。
* 关键线索：**`sp=0x3fca0565` 不是 16 字节对齐、而且落在 DRAM 最底部**
  （DRAM 起于 0x3FCA0000，我们的栈池在 0x3fcbce30，`.bss/.data` 就在这一带）。
  也就是说栈指针一路往下走穿了，进到数据区。
* 时序上正好发生在"库创建 `ppTask`（`[task] 库建任务 fn=[give]`）→
  切到 t=2 → `[recv]` 阻塞"之后，说明闭源协议栈这条调用链比我们给的
  6144 字节栈深得多（`osi_task_create` 里把库请求的栈抬到 6144，见
  wifi_glue.c）。

**下一步**（按性价比排序）：

1. 把库任务的栈抬到 12~16 KB，并把 `S3_STACK_POOL` 相应扩大
   （DRAM 已经紧张，可能需要挪 `g_wifi_heap_mem` 或减小 lwIP 的池）；
   同时在 `port_task_switch` 里加上"sp 不在自己栈区间内就打印并停机"
   的断言（§276 那种守卫，但针对 sp 越界），把跑飞的第一现场抓到。
2. 已经证明 **PHY/射频这块通了**（`register_chipv7_phy rc=1`、
   `0x6001c08c=03fffa00`），所以后续不必再在 PHY 初始化上花时间。

## §294 切换前 sp 守卫：ctx 本身是干净的

`kernel/task.c6` 在 `port_task_switch(t->ctx)` 之前加了一句
`port_task_ctx_check(t->ctx)`（实现放在 `port/s3/ctx.c`）：目标 ctx 的
`sp` 必须落在它自己的 `[stack, stack+size)` 里且 4 字节对齐，否则打印
`[ctx] sp 越界：...` 并停机（同时把 sp/lo/hi/ctx 写进 RTC 0x50000100）。

真板跑下来**守卫一次都没触发**，而且崩溃后用 JTAG 把 `g_ctx`
（0x3fcc2e40，8×76 字节）整个 dump 出来看，两个任务都正常：

```
ctx[0] sp=3fcbeaf0 stack=3fcbce40 size= 8192 OK
ctx[1] sp=3fcc0330 stack=3fcbee40 size= 6144 OK
```

⇒ **不是 ctx 被写坏，也不是栈太小**。§293 里那个 `sp=3fca059b` 更像是
"被抓到时 a1 已经是垃圾"，而不是任务栈指针本身。

另外试过把栈池从 24KB 抬到 40KB、库任务栈抬到 12KB —— 链接器直接报
`S3 BSS/stack collision`：DRAM 顶（`S3_STACK_TOP=0x3FCD9000`，坏区以下）
已经用满了，**没有空间再抬栈**，所以回退了这两个改动。

### 当前墙的形态

* `[recv] t=2 qh=... tk=ffffffff` → 内核 `b`（阻塞）→ 之后立刻
  `cause=0`（**非法指令**）`pc=0x40048c6e`（ROM 区）`a0=0x00060000`
  `callinc=2`、`PS.WOE=1`。
* `cause=0` + ROM 代码 + 垃圾 a0/a1，历史上（link.ld 注释里写过）
  都是 **`retw` 恢复出错误返回地址**的典型症状：ROM 里的闭源函数是
  windowed，如果它被 **call0** 方式调用（或反过来），`retw` 在
  `n==0`/`m!=n` 时就是非法指令。
* 也就是说：下一个要抓的是 **"哪个入口的 ABI 用错了"**——
  优先怀疑 OSI 表里还没套 `W2C` 壳的回调（`HIT_OF` 打印的槽号
  0x0d/0x10/0x12/0x13/0x15/0x16/0x1d/0x25/0x2a/0x2b/0x56/0x58/0x5e
  就是这条路上被调到的），以及库自己创建的任务（`ppTask` 等）第一次
  被真正调度时的窗口基址。

## §295 下一个最可疑的点：切走时"活跃窗口没落盘"

`port_task_switch` 现在的做法是：`rsil a5,15` → 想到新任务的
`WINDOWBASE`（记录在 ctx+72）→ `wsr.windowbase` → 再把
`WINDOWSTART` 写成"只有当前一个窗口活跃"（`1<<WINDOWBASE`）。

对 **call0** 的任务（我们内核自己的任务）这套没问题：call0 不用窗口。
但闭源库的任务是 **windowed**：它跑起来时一次 `call8` 嵌套就会占多个
物理窗口，硬件是在"窗口溢出"时才把寄存器写进栈的（spill）。
我们在切换时直接把 `WINDOWSTART` 清成一个窗口，等于**丢掉被切走任务
还压在物理窗口里、尚未 spill 的那几组寄存器**（a0~a15 的一半）。
等它下次被调度回来，`retw`/下溢处理体从栈上恢复的是旧值 ——
表现就是"返回地址飞了、PC 落在指令中间"，和 §293/§294 抓到的
`cause=0`、`pc` 在 ROM 里、`a0` 是垃圾完全吻合。

**下一步（下一轮直接做这个）**：

1. 在 `port_task_switch` 里，改 `wsr.windowstart` **之前**先把当前
   WINDOWBASE 起、WINDOWSTART 里标记为活跃的窗口逐个 spill 到各自的
   `[sp-16..-4]`（就是 `s32e a0,a1,-16` / `s32e a1,a1,-12` /
   `s32e a2,a1,-8` / `s32e a3,a1,-4`，配合 `rotw` 一圈回到原点），
   对齐 xtos 的 `_xtos_spill_all_windows` 语义。此时再清
   `WINDOWSTART` 才是安全的。
2. 或者更省事：**切换时不动 WINDOWBASE/WINDOWSTART**，让硬件窗口
   自然轮转（任务之间共享同一套窗口，靠 spill/underflow 保证正确性），
   ctx 里只存 a0~a15 + ps + pc。这样完全避开"窗口状态跟任务绑定"
   这件事——代价是 a0/a1 的保存要更小心。

两条路都要先做一件事：把 `port_task_switch` 里 WINDOWSTART 这段的
真板行为记录下来（现在只有静态推断）。

## §296 修好"窗口状态被丢弃"：非法指令崩溃消失 ✅

按 §295 的思路改了 `RESTORE_FRAME`（`port/s3/ctx.S`）：恢复现场时
**优先写回任务自己保存的 WINDOWSTART**（帧偏移 68），只有偏移 68 为 0
的"新建任务"才归一化成 `1<<WINDOWBASE`。原来的实现是一律归一化，
等于把被切走任务还压在物理窗口里、尚未 spill 的那几组寄存器丢掉。

真板验证（bootloader 路径，`flashbl.sh`）：

```
[recv] t=2 qh=3fcc451c tk=ffffffff
b                          ← 之后不再有 *** TRAP（之前必崩）
```

§293/§294 里那条 `cause=0（非法指令）pc=0x40048c6e` 的崩溃**彻底消失**。
这条改动是迄今为止对"混合 ABI + 多任务"最关键的一次修复。

### 新暴露的问题（进度，不是退步）

JTAG 读 RTC 记录 `0x50000100`：

```
bad0 0000001d 00000370 40048839 00060410 00000000 00000000 00000005
0x50000120: 000005eb 0000000f
```

* `cause=0x1d(29)` = 非法地址读写，`EXCVADDR = 0x370` —— **空指针 + 偏移**，
  `EPC1 = 0x40048839`（又是 ROM 区），`WINDOWBASE=5`、`WINDOWSTART=0x5eb`
  （多窗口活跃 ✓ 说明 §296 生效），tag `0x0F` = **二次异常**。
* 也就是说：现在不是"返回地址飞了"，而是**某处真的解引用了 NULL+0x370**。
* 位置：`[wifi] associate ssid=...`（wifi_glue.c:1469）**还没打印**，
  所以还卡在 `s3_wifi_bringup()` 里的库初始化过程中；
  最后一次输出是 `[recv] t=2 ...` 后的内核阻塞标记 `b`。

**下一步**：查 `_malloc` 是否返回了 NULL（64KB 的 `g_wifi_heap_mem` 可能不够），
以及 ppTask 第一次跑起来时用到的、我们还没接的 OSI 回调。

## §297 二次异常把第一次现场盖掉了 —— 加一级 RTC 记录

`_vec_exc` 现在**在调 `s3_trap` 之前**就把 level-1 现场的 8 个字写进
RTC `0x50000140`（exccause / excvaddr / epc1 / ps / frame_sp / a0 /
windowbase / windowstart）。这样即使 `s3_trap` 打印时再出二次异常
（0x50000100 被第二次覆盖），第一次的线索也还在。

同时给 `osi_malloc` 的"堆耗尽"出口加了计数（`0x50000748` 次数、
`0x5000074c` 最近一次大小，第一次失败打一行 `[heap] 分配失败 n=`）。

### 本轮实测到的新状态

* 串口：`[recv] t=2 qh=... tk=ffffffff` → `b` → **之后再无输出**。
* `0x50000140` 是随机数（没有被写过）⇒ **没有 level-1 异常**。
* `0x50000100`：`bad0 0000001c 00000374 40048839 00060010 60000000 00000380
  00000002`，`+0x120`：`00000605 0000000f`
  * cause `0x1c(28)` = **LoadStoreAlignment**，EXCVADDR `0x374`
  * a1 = `0x380`、WINDOWBASE=2、WINDOWSTART=0x605（6 个窗口活跃）
  * tag `0x0F` = 二次异常
  ⇒ 出事的是**窗口处理体自己**：某个窗口的 sp 变成了 `0x380`（近似 NULL），
    `_win_unf8`/`_win_ovf8` 于是去读写 `0x374` 附近而二次异常。
* `[heap] 分配失败` 没有打印 ⇒ **不是 64KB WiFi 堆被耗尽**。

### 结论 / 下一步

1. `0x380` 这种"近似 NULL 的 sp"不是正常栈：说明某条 windowed 调用链
   的帧指针被写成了小整数。需要在 `_win_unf8` 里对
   `extui(a1,24,8)==0`（高字节为 0，肯定不是 DRAM/flash 地址）的填充
   做记录/停机，抓第一次坏 fill 的 `a9` 与 `a1`。
2. 另一条并行的检查：`WIN_SPILL`（w2c.S）在任务切换时是否真的把所有
   祖先窗口落盘了 —— 现在 `WINDOWSTART=0x605` 说明切换时还有 6 个窗口
   活跃，而 `RESTORE_FRAME`（§296）会把这份 live 位写回去，物理寄存器
   里的内容却已经是别的任务的了。

## §298 坏 fill 的记录点没触发 —— 坏 sp 不是"填充回来的"

在 `_win_unf8` 里，`l32e a1, a9, -12` 之后加了一个判断：如果填充回来的
"调用者 sp"高字节为 0（0x00000000~0x00FFFFFF，不可能是 DRAM/flash），就把
`a9`/`a1` 记到 RTC `0x50000160` 并停机。

真板结果：`0x50000160` 是两个随机数（**没被写过**）⇒ 下溢填充没有读到坏值。

再结合 §297 的 `EXCVADDR=0x374`、`p_ps=0x00060010`（EXCM=1）可以定位：
出错的 `l32e/s32e` 用的基址寄存器就是 **a9 = 0x380**，而这条指令在
`_win_unf8`/`_win_ovf8` 的**最前面**，所以我们的判断根本来不及跑：

```
_win_unf8:
    l32e a0, a9, -16      ← 这里/附近的 a9 已经是 0x380
    l32e a1, a9, -12      ← EXCVADDR = 0x374 = 0x380 - 12
```

⇒ 结论：**某个寄存器窗口的 a1（帧指针）本身被写成了 0x3xx 量级的小整数**，
不是 spill 槽内容错，而是窗口里那个 a1 错。

下一步候选：
1. `W2C` 壳的 `addi a1, a1, -64` 死区 + `WIN_SPILL` 那段，检查是否有
   某条路径在 a1 已经被改动后还去 spill；
2. `s3_task_tramp` 给"新建 windowed 任务"铺的第一帧，a1 是否合规；
3. 在 `_win_ovf8/_win_unf8` 的入口就比对 a9（用 `extui`，不破坏任何要
   恢复的寄存器：只能借 a2/a3 在被装载之前的空档），记录**进入处理体时**
   的 a9。

## §299 窗口处理体入口带坏 a9 的记录也没触发

把 `_win_unf8` 的入口改成"先看 a9 高字节，是 0 就记 RTC 0x50000160"，
真板再跑一次：`0x50000160` 仍是随机数（没被写过）。

⇒ **`_win_unf8` 根本没有带着坏 a9 进来过**，甚至可能从未被进入。
那么 `cause=0x1c / EXCVADDR=0x374 / a1=0x380` 这一组就**不是下溢填充**造成的，
而是二次异常时这些寄存器/寄存器对本身就是**陈旧值**（二次异常不更新
EXCVADDR 语义），真正的信息只有：

* tag `0x0F` = **二次异常**本身（一次异常 → 处理体里再异常）；
* `EPC1 = 0x4004883e` 在 ROM 区 ⇒ 触发第一次异常的那条窗口指令
  （`call8`/`retw`）在 **ROM 的闭源代码**里。

也就是说：**ROM 里的 windowed 代码做窗口轮转时，处理体不归我们管**
（很可能我们 `.vector` 段并不是硬件真正在用的那套向量 —— 需要确认
`VECBASE`/`PS`，以及硬件是否真的跳到我们的 `_win_*`）。

**下一轮第一件事**：用 JTAG 读 `VECBASE`（以及 `0x40379000` 一带是否被
其它 init 改写过），确认窗口向量到底是谁在处理。若确实是 ROM 的向量，
那 §292/§296/§298 的改动只是在"我们这套向量"上生效，真正跑的是 ROM 的，
需要把 `_win_*` 装到正确地址或者直接采用 ROM 的语义。

## §301 重要教训：这一带的"改动 = 换布局"，诊断代码会把进展顶掉

本轮踩了一个很值钱的坑：在拿到 **PHY 初始化成功 + 库初始化跑到 `[recv]`**
这个已知良好状态之后，我又往 `irq.S` 里加了 §297（`_vec_exc` 里存 RTC）、
§298/§299（`_win_unf8` 入口记录）、§300（`_vec_default` 里记 VECBASE）。
结果**同一份逻辑、只是布局变了**，表现就变成：

* 加 §299 之后：`register_chipv7_phy` 里 `pc=0x4205291e` 非法指令（PHY 都过不去）；
* 把 §298/§299/§300 全部回退（`git checkout ddfe0eb -- port/s3/irq.S`）后：
  `[phy] register_chipv7_phy rc=1` → `W1 bringup` → … → `[recv]` → `b` ✓ 复现。

⇒ **`port/s3/irq.S` 的字节布局对这条路径极度敏感**（和 §271 一个性质）。
以后所有实验：
1. 只改 `win_island.c` 或 `.c` 文件，**不要**在 `irq.S` 里加诊断；
2. 每次拿到新进展，立刻把镜像和 elf 存到 `/tmp/c6good/` 并 commit；
3. 要加诊断就加在 C 侧、或者用现成的 RTC 区写（不新增代码体积）。

**当前锁定的已知良好状态**（`/tmp/c6good/c6os_bl_recv.bin`，build/wifi/c6os.bin）：

```
[phy] MAC 0x6001c08c=03fffa00 OPTIONS0=1c00a000
[phy] calling register_chipv7_phy(mode=0)
[phy] register_chipv7_phy rc=1              ← 射频初始化成功
W1 bringup
B0/B1/B2 OSI 表校验通过（magic=deadbeaf ver=00000008）
[lockok] t=1 pr=0 ok=1
[pp] pp rom version: e7ae62f
[net80211] net80211 rom version: e7ae62f
[mtxnew] #1 #2  [semnew] #1
[task] 库建任务 fn=40381760 [give] t=2 sem#1
[recv] t=2 qh=3fcc452c tk=ffffffff
b                                            ← 之后停住（下一个要攻的墙）
```

`fn=40381760` 是库自己创建的任务入口（IRAM），`[recv]` 是它第一次去队列上阻塞。

## §302 停在 `[recv]` 之后的实证：主任务的 EPC 落在 ROM 的**数据区**

在 §301 那个已知良好状态下（串口停在 `[recv]` → `b`），把它 halt 住，
把 `g_ctx`（0x3fcc2e50，8×76 字节）整个 dump 出来，再顺着每个任务的
`ctx->sp` 读它的 17 字陷阱帧：

```
ctx[0] sp=0x3fcbeb00 stack=0x3fcbce50 size=8192
   帧: epc=0x4004883b ps=0x60000  ws=0 wb=0
ctx[1] sp=0x3fcc0340 stack=0x3fcbee50 size=6144
   帧: epc=0x42017418 ps=0x60600  ws=0 wb=0
```

把 ROM `0x40048800` 一带读出来反汇编（`objdump -b binary -m xtensa`）：

```
40048831: neg  a0, a0
40048834: slli a0, a0, 31
40048837: .byte 0x60
40048838: .byte 0x50
40048839: l32r a15, 0x40018774
4004883d: bnone a4, a8, ...
```

这段根本**不是代码**（`0x0000c2 / 0x010010 / 0x000060 / 0x100001`
这种小端常数是典型的只读数据表）。⇒ `ctx[0]`（主任务）保存的 EPC
`0x4004883b` 落在 ROM 的**数据区**，也就是又一次"返回地址飞了/PC 跳到
数据区"。

同一个 0x40048xxx 区域在本轮多次出现（§293 `0x40048c6e`、
§299 `0x4004883e`），而且每次反汇编出来都是"半个指令 / 数据"，
**说明 ROM 里这段被闭源库当作可执行代码调用，但我们的窗口/栈状态
让它跳到了错的偏移**（±1 字节级别）。

⇒ 下一个真正的攻关点仍然是 **windowed ABI 的寄存器窗口在任务切换时
的一致性**，而不是 WiFi 逻辑本身。具体建议：

1. 把 `_win_ovf8/_win_unf8` 里 §292 的"回退"改成**只在 a9 合法**时才
   回退（`extui a9,24,8` 非 0），否则走 `_vec_default` 记录并停机——
   现在它会往可疑地址写，把别人的帧踩坏；
2. `W2C` 壳的 spill 死区从 64 字节加大到 128（`addi a1,a1,-128`），
   给 4 个窗口 × 16 字节留足余量；
3. 每次改动都保留 `/tmp/c6good/` 的镜像并在真板复核 `[recv]` 是否还在。

## §303/§304 两个"理论上更正确"的改动：都没改变崩溃点

1. **§303**：`W2C` 壳的 spill 死区从 64 字节加到 128（`addi a1,a1,-128`；
   `addi` 立即数只到 127，所以回程拆成两次 `+64`）。
   效果：崩溃时 `*** TRAP` 终于能打出来了（之前 `s3_trap` 打印本身就会
   再异常，什么都看不到），**但没有修掉崩溃**。
2. **§304**：在 `port_task_switch` 的保存路径里整圈 spill 祖先窗口
   （把 w2c.S 的 `WIN_SPILL` 复制成 `S3_SPILL_ALL`），同时把
   `RESTORE_FRAME` 退回"一律归一化 WINDOWSTART"。
   效果：**签名完全不变**，说明"切走时没落盘"这个猜测也不成立。

两次实验后的崩溃签名（稳定复现）：

```
[recv] t=2 qh=3fcc452c tk=ffffffff
b*** TRAP sp=3fcbeb00 cause=00000000 pc=40048c6e ps=00060010 vaddr=00000000 callinc=00000002 a0=00060000
```

注意几个细节：

* `sp=0x3fcbeb00` **正好等于 ctx[0].sp**（§302 dump 出来的主任务帧基址），
  也落在主任务自己的栈区间 `[0x3fcbce50, 0x3fcbee50)` 里 —— 栈没跑飞；
* `a0 = 0x00060000`，而 `ps = 0x00060010` —— **a0 就是 PS 抹掉低 4 位**，
  说明某个恢复路径把 **PS 的值装进了 a0**；
* `pc = 0x40048c6e` 落在 ROM 那条 `bnez a8, 0x40048c65` 的**指令中间**
  （§302 已经证明 0x40048xxx 一带不是代码）。

⇒ 下一步的重点从"窗口 spill"转向 **"帧里 a0 / PS 两个槽被写错或读错位"**：
建议先在 `RESTORE_FRAME` 里把要写进 `wsr ps` 的值和要写进 a0 的值分别
记到 RTC（不新增分支，只加两条 store），确认恢复时两者没串位。

## §305 a0/EPC 一致性检查：ctx 本身是自洽的，问题在"ctx 被谁写"

在 `port_task_ctx_check` 里加了一条：帧里 `f[0]`(a0) 与 `f[15]`(EPC) 抹掉
CALLINC 两位后必须相同（call0 语义）。真板：**一次都没触发**，
RTC `0x50000170` 是随机数（没被写过）。

同时用 JTAG 读调度计数（崩溃后）：

```
g_yl_cnt=2  g_sw_cnt=2  g_ts_cnt=2   RTC 0x50001000=2
g_s3_win_spill_en=1
```

⇒ **总共只切了 2 次任务就崩了**，而且每次切换时 ctx 帧里的 a0 与 EPC 都是
一致的。结合 §302（ctx[0] 的 EPC = 0x4004883b，落在 ROM 数据区）可以断定：

> 不是"恢复时读串了"，而是**这个 ctx 在保存的那一刻，被保存下来的 PC
> 本身就已经指向 ROM 的数据区**。

保存 PC 的路径只有两条：
1. `port_task_switch`（内核主动切）—— 此时 a0 是内核 call0 的返回地址，
   不可能落在 ROM 数据区；
2. **`port_irq_fill`（中断里把陷阱帧盖进 ctx）** —— 此时 a0/EPC1 是
   **被打断的那段闭源码**的值，完全可以落在 ROM 里任何地方。

⇒ 高度怀疑：`S3_NO_PREEMPT` 下 `s3_timer_irq` 返回 0（不切换），但
**`port_irq_fill` 仍然把中断帧写进当前任务的 ctx**。于是这个任务下次
被正常切回来时，就 `rfe` 到"某次中断发生时闭源码刚好在的地方"——
如果那次中断发生在闭源码调用 ROM 工具函数的中途，PC 就是在数据区里。

**下一轮第一件事**：确认 `s3_timer_irq` 返回 0 的分支是否跳过了
`port_irq_fill`；如果没跳过，就把它挪到"确实要切换"的分支里。
这是目前最有希望的一击。

## §306 试过"中断帧里也存 WINDOWSTART/WINDOWBASE"：无效，已回退

按 §305 的推断，给 `port_irq_fill` 补写了帧偏移 68/72
（`rsr windowstart` / `rsr windowbase`），并在 `RESTORE_FRAME` 里恢复
"优先用帧里的 WINDOWSTART"。真板结果：

```
[recv] t=2 qh=3fcc456c tk=ffffffff
b*** TRAP sp=50000ab0 cause=0 pc=40048c6e ps=00060010 a0=00000000
```

`sp` 变成了 **RTC 地址 0x50000ab0**（比之前更离谱），崩溃点不变。
说明这条推断也不是根因，已 `git checkout 510978f -- port/s3/ctx.c port/s3/ctx.S`
回退，板子重新回到"`[recv]` → `b`"的基线。

### 本轮（第 50~51 轮）净收获

1. §296 修好"任务自己的 WINDOWSTART 被丢弃"（`RESTORE_FRAME`），
   消除了这条路径上最凶的非法指令崩溃；
2. §292 的溢出回退让 **IDF bootloader 启动路径**能过 `T1`，
   从而拿到 **`register_chipv7_phy rc=1`**（射频初始化成功）和
   `[pp]`/`[net80211]`/`[mtxnew]`/`[semnew]`/`[task]`/`[recv]`
   一整套库初始化流程；
3. 明确排除：PHY init data 不一致、WiFi 堆耗尽、DIG_PWC/WIFI_CLK_EN/
   RST_EN 寄存器差异、ctx sp 越界、ctx 里 a0/EPC 不一致、
   切换时没 spill 祖先窗口、中断帧没带窗口状态；
4. 摸清了这条路径对**代码布局极度敏感**（§301），并留下
   `/tmp/c6good/c6os_bl_recv.bin` 作为基线镜像。

### 目前唯一的墙

闭源库的第一个任务（`fn=0x40381760`）去队列上阻塞、内核完成 2 次任务
切换之后，闭源/ROM 代码里出现 `cause=0 (非法指令)`、`pc=0x40048c6e`
（经反汇编确认 **落在 ROM 数据区/指令中间**）。栈本身没跑飞
（`sp` 始终落在该任务自己的栈区间内）。

下一步建议换思路：与其继续在窗口语义上试，不如
**让闭源代码只在一个"受控 ABI 环境"里跑**——例如所有 OSI 回调都不真正
阻塞（`_queue_recv` 用有限自旋 + `os_task_delay` 让出），保证
windowed 调用链永远不会在"窗口活着"的时候被切走；或者干脆把
ppTask 这类库任务固定跑在一次 `s3_win_call_wifi_main` 的长调用里，
不参与我们的抢占式调度。

## §307/§308 关键发现：看到的 `pc=0x40048c6e` 是**打印路径里**的二次现场

两件事：

1. **§307**：让 `RESTORE_FRAME` 干脆**完全不碰** WINDOWBASE/WINDOWSTART
   （让硬件窗口像单任务那样自然轮转）。崩溃签名不变（只是 a0 值不同），
   说明"每任务一份窗口状态"这个设计不是根因。
2. **§308**（信息量最大）：在 `s3_trap` 里，**打印之前**先把第一次异常的
   现场写进 RTC `0x50000140`（cause/excvaddr/epc1/ps/frame_sp + 帧里
   a0/a1/EPC/PS/ws/wb）。真板读出来：

```
0x50000140: 00000000 00000000 40048c6e 00060f10 3fcbeb00 00060d00 00000000 40048c6e
0x50000160: 00060f00 00000000 00000000
```

* `ps(异常发生时)=0x00060f10` → **INTLEVEL=15、EXCM=1**
* `帧里保存的 PS=0x00060f00` → 被打断的代码运行在 **INTLEVEL=15**（临界区）、
  EXCM=0、WOE=1
* `sp=0x3fcbeb00` 落在主任务自己的栈里 ✓、`f[18]`(windowbase)=0

也就是说：**第一次异常是在一个"关中断到 15 的临界区"里发生的，
而且当时 PS.EXCM 已经是 1（说明我们已经在一个异常处理体里）**。
随后 0x40048c6e 这条"落在指令中间的点"才是二次现场。

⇒ 之前所有"pc=0x40048c6e"的记录都是**我们自己的打印/处理路径**里的
二次异常，不是真正的第一现场。下一轮必须：
1. 让 `s3_trap` **完全不打印**（只写 RTC 然后停机），避免打印路径引入
   新的异常；
2. 找出"在 INTLEVEL=15 的临界区里、且已经处于异常上下文"的那条路径
   —— 最可疑的是 `RESTORE_FRAME` 开头的 `rsil a5,15` 之后紧接着的
   窗口状态操作，以及 `port_enter_critical` 包住的 `ets_printf`。

## §309~§311 真正的元凶找到了：W2C 壳里的 **PS.CALLINC** 被破坏

### §309 先把"阻塞"从等式里去掉

临时让 `osi_queue_recv` 在 `ticks` 很大时只 try 一次、**不进内核阻塞**。
结果：库不再一上来就死，而是**连续调了几百次 recv 在轮询**，
最后崩溃点换了地方：

```
*** TRAP sp=3fcc04e0 cause=0 pc=4037d062 ps=00060f10 callinc=2 a0=00060f00
```

`pc=0x4037d062` 落在**我们自己的 `osi_queue_recv_body+0x1b2`**，反汇编一看
那条指令是 **`retw.n`**。`retw.n` 只有在
`CALLINC==0`、`EXCM==1`、`WOE==0`、或"返回步数 m≠n"时才非法。
现场 `PS=0x00060f00`：`WOE=1`、`EXCM=0`、但 **`CALLINC=2`（call12）**，
而这条函数是被 `callx8` 调进来的 —— **m≠n ⇒ 非法指令**。

### §310 修法：壳里把 PS 存下来，`callx0` 进内核之后再写回

`W2C` 壳的结构是"windowed（`entry`/`retw`）→ `callx0` 调 call0 内核"。
内核那边 `call0`/`ret` 不会维护 `PS.CALLINC`，于是回到 windowed 侧时
`CALLINC` 已经不是调用者那一档，壳末尾的 `retw` 就非法。

在 `port/s3/w2c.S` 的壳里加上：

```
    entry   a1, 128
    s32i    a0, a1, 16
    rsr     a8, ps
    s32i    a8, a1, 20        /* 注意：必须用 a8（caller-saved），
                               * 用 a6 会踩掉第 4 个参数 —— 实测会让
                               * register_chipv7_phy 当场失败 */
    ...
    l32r    a8, 9b
    callx0  a8
    l32i    a8, a1, 20
    wsr     a8, ps
    rsync
    l32i    a0, a1, 16
    retw
```

效果立竿见影：**`retw.n` 非法指令这一类崩溃消失了**，
库的初始化一路跑通（`[pp]`/`[net80211]`/`[mtxnew]`/`[semnew]`/`[task]`）。

### §311 恢复"写回任务自己的 WINDOWSTART"（§296 的结论仍然成立）

§307 试过切换时完全不碰窗口状态，仍崩；把 §296 的
`frame[68] → WINDOWSTART` 写回加回来后，崩溃彻底消失：

```
[recv] t=2 qh=3fcc4530 tk=ffffffff
b
```

板子进入**稳定空闲**：库建的任务（id=2）在队列上等消息，
主流程在 ROM 里等它，两边都不动，**没有异常、没有跑飞**。
这是本项目第一次达到"闭源 WiFi 库整套初始化跑完且不崩"的状态。

### 现状与下一步

* 两个上下文（JTAG 读 `g_ctx@0x3fcc2e50`）：
  * ctx0（主流程）：`sp=3fcbeb00 stack=3fcbce50 size=2000`，
    帧里 `EPC=0x4004883b`、`PS=00060900`、`WB=0` —— 停在 ROM 里；
  * ctx1（库任务）：`sp=3fcc0340 stack=3fcbee50 size=1800`，
    帧里 `EPC=0x420176e8`、`WB=0xf`、`WINDOWSTART=0x8000` —— 在队列上等。
* 下一步就是**让它别等**：查清 ROM 那个"主流程等待"是在等哪个标志/队列，
  把 `esp_wifi_start` + `connect` 推下去；或者直接把主流程改成
  "init 不等待、异步继续"。

## §312~§314 崩溃点定位到 ROM 里的 `retw.n`：窗口状态在任务切换时被污染

### §312 先把默认向量记录读出来（JTAG 直接读 RTC，不改 irq.S）

`0x50000100`：`0000bad0 | 0000001d | ffffffd1 | 40379590 | 00060817 | 00000001 | 3fcc03e0 | 00000009 | 00000200 | 0000000f`

* `cause=0x1d` = StoreProhibited，`vaddr=0xffffffd1` = **`a9-16` 且 `a9=0xffffffe1`（垃圾）**；
* `epc1=0x40379590` 正好在 `port_task_switch` 里的 `S3_SPILL_ALL`（`rotw` 串）；
* `ps=0x00060817`：`EXCM=1`、`INTLEVEL=7`、`WOE=1` ⇒ 是**双重异常**。

结论：§304 的"切换时整圈 spill"里 `rotw` 撞上**活窗口**，溢出处理体拿到的 `a9` 是
我们 call0 内核帧里的临时值（不是栈指针）→ `s32e a0,a9,-16` 直接写飞。
关掉 `S3_SPILL_ALL`（§312）后双重异常消失，恢复成可打印的 `*** TRAP`。

### §313 新建任务的 `WINDOWSTART` 必须归一化

`RESTORE_FRAME` 在帧里 `WINDOWSTART==0`（新建任务）时**不能直接跳过 `wsr`**：
别的任务留下的 `WINDOWSTART=0x8000`（base=15）会生效，恢复出来的 a0..a15 落在错的窗口。
改成 `1<<WINDOWBASE`。这一条是正确性修复（本身没改变症状）。

### §314 真正的崩溃点：ROM 里的 `retw.n`（非法指令）

关掉 spill 后 `*** TRAP` 稳定复现：

```
*** TRAP sp=3fcbeb00 cause=00000000 pc=40048c6e ps=00060f10 vaddr=00000000 callinc=00000002 a0=00060900
```

用 JTAG 把 ROM 那一段 dump 下来反汇编（`objdump -D -b binary -m xtensa`），
`0x40048c6e` 就是 **`retw.n`**：

```
40048c30: entry a1, 32
40048c33: call8 0x400442a4
40048c4b: call8 0x40048804
40048c69: call8 0x40048804
40048c6c: mov   a2, a10
40048c6e: retw.n          <-- 非法指令
40048c70: entry a1, 32    <-- 下一个 ROM 函数
```

`entry` 会把 `PS.CALLINC` 清 0，`retw.n` 要求 `CALLINC==0` 且与 `a0[31:30]` 配套；
现场 `PS.CALLINC=2`（call12）⇒ 非法。`a0=0x00060900` 恰好是**帧里保存的 PS 值**，
说明返回地址寄存器早被踩成 PS 了。

也就是说：**任务切回 task1（`sp=0x3fcbeb00` 就是 ctx0）之后，它的窗口/寄存器是脏的。**
这正是"切走时闭源库的活窗口没被 spill"的直接后果 —— §304 想解决它，但做法
（在 call0 内核帧里 `rotw`）反而写飞。§312 的 RTC 记录把这两件事串起来了。

### 下一步（明确的工程结论）

要正确切走"正处在 windowed 深调用里"的任务，必须在**windowed 上下文里**
（也就是 `port/s3/w2c.S` 的壳里，那里 `[a1-12]` 的帧链是 ABI 合法的）
把祖先窗口 spill 出去，而不是在 call0 内核的 `port_task_switch` 里 `rotw`。
对应的填充侧已经在 `_win_unf4/8/12` 里了，两边必须严格对称。

## §315~§317 用 IDF 官方 window_spill 替换手写 rotw；两条关键实测结论

### §315 `W2C` 壳改用官方 `window_spill_function`

`port/s3/w2c.S` 的 `WIN_SPILL` 从"手写一圈 `rotw 3`×4 + `rotw 4`"改成调用
**IDF `coreasm.h` 里 `window_spill_function` 的 64-areg 版本**（本仓库实现为
`s3_win_spill`，含 `.Lws16/28/40/52` 这条 `call12` 深度链）：

* 官方版本不依赖"帧链 `[sp-12]`"，而是用一串 `call4/8/12` 把窗口基址推满一整圈，
  由**硬件溢出机制**把每个活窗口 spill 到它自己的栈帧，返回时再退回来；
* 它会破坏 caller-saved 的 a2..a7，而壳要把参数透传给内核，所以先把 a2..a7
  存进壳帧的 [a1+64..84] 再取回。

**实测：签名完全没变**（还是 `pc=40048c6e` 的 `*** TRAP`）⇒ 崩溃与"切换时有没有
spill 祖先窗口"**无关**，不要再在这条线上花时间。

### §316 切换点记录（RTC 0x50000300，每项 32 字节）

`port_task_switch` 里记 `a0 / 算出的 EPC / PS / WINDOWSTART / WINDOWBASE`。
第一次切换的记录：

```
a0=42018498 EPC=42018498 PS=00060800
```

* EPC 是 **flash 里的内核代码**（0x42xxxxxx），完全正常；
* `PS=0x60800`：`CALLINC=2`（这次是从 **call12** 进壳的）、`WOE=1`、`INTLEVEL=0`。

也就是说**切出去的时候现场是干净的**，"切回来落到 ROM 的 retw.n" 不是切换保存
出了问题。

### §317 恢复点记录（RTC 0x50000400，每项 32 字节）

`RESTORE_FRAME` 里记"这次真正用的帧地址 / EPC / PS / WS / WB"。唯一一条有效记录：

```
frame=3fcbee00 EPC=40379761 PS=00060800 WS=0 WB=0
```

`frame=0x3fcbee00` = 任务 1 栈顶（`0x3fcbce50+0x2000`）往下 0x50，正是
`port_task_ctx_create` 造的**初始帧**；`EPC=0x40379761` 是任务 trampoline。
⇒ 任务 1 的**启动恢复是正常的**。

于是结论收紧为：**崩溃发生在库自己跑起来之后、ROM 某函数 `retw.n` 返回时**，
不是我们的切换/恢复路径（这也解释了为什么 §304/§315 两种 spill 改法签名都不变）。

### §318 顺带证实：`§292` 的兜底不能删

把 `_win_ovf8/`_win_unf8` 里的"上一级 sp 为 0 就回退到 a9"兜底删掉、恢复 IDF
原版后，启动直接卡在 `T1 call0 任务进入 windowed 主流程`（`s32e a0,a9,-16`
写到 0xffffffe0 的双重异常，静默）。
⇒ 我们 call0 → windowed 的入口确实存在一条 `[a1-12]==0` 的帧链，
兜底是**给这条边界擦屁股**的，删不得。已用 `git checkout` 恢复。

### 下一步（方向已经明确）

崩溃点在"ROM 函数 `retw.n` 遇到 `PS.CALLINC` 为 2、`a0` 却是垃圾"，
说明**闭源库的 windowed 调用链在跨过我们 call0 边界时窗口簿记被打乱**。
两个可选方向：

1. **根治**：把内核（`kernel/*.c6` → C）从 `-mabi=call0` 换成 `-mabi=windowed`，
   全系统统一 ABI，`W2C` 壳与那些兜底全部删掉。改动大但一次到位。
2. **打补丁**：在 `call8/call12` 进壳和 `retw` 出壳的两端，显式校正
   `PS.CALLINC`（§310 已经在做一半），并把 `_win_*` 兜底改成"写回正确帧链"。

## §319 JTAG 硬件断点可用（新工具），以及它给出的两条硬事实

这一轮摸清了 OpenOCD 对 S3 的能力边界，后面排查可以直接用：

| 能力 | 结论 |
|------|------|
| `bp <addr> 2` | ✗ ROM 不可写：`address 0x40048c6e not writable` |
| **`bp <addr> 2 hw`** | ✓ **硬件断点可用**，`breakpoint set at 0x40048c6e` |
| `reg` | ✓ 能读 **ar0..ar63 全部物理寄存器** |
| `reg ps` / `reg windowstart` | ✗ 特殊寄存器读不到（只有 OCR 那批 trax_/pm_/ocd_ 返回 0） |
| `step` | ✗ `Stepping doesn't seem to change PC!`，**无法单步越过断点** |
| `resume` 在断点上 | ✗ 原地不动（不会自动略过断点） |

在 `0x40048c6e`（ROM 那个 `retw.n`）上命中断点时读到的物理寄存器：

```
pc=40048c6e  ar0=00000000  ar1=3fceb710  ar2=0000cdcd  ar3=0000abab
ar4=60000000 ar5=00000380 ar6=00000045 ar7=6000001c  ar8=3fcef130 ar9=60038004
```

说明这条 `retw.n` 在**启动早期（`ar1` 还是 boot 栈 `0x3fceb710`）就会被正常执行到**，
`ar2/ar3` 还是 `0xcdcd/0xabab` 这种未初始化填充值 —— 所以"断在 0x40048c6e"本身
**不等于**撞上崩溃点，不能只凭这一条下结论。另外 `ar4=0x60000000`、`ar7=0x6000001c`
正是 `0x40048c10` 那个 UART FIFO 轮询函数算出来的基址/状态地址，
可以确认 `0x40048c30` 这一族 ROM 函数是 **UART 输出（ets_printf 底层）**。

顺带确认：`S3_ABI ?= call0`（`https` 目标没有覆盖它），所以
`irq.S` 里 level-1 定时器 handler 走的是 `call0 s3_timer_irq` 分支
**不轮转窗口** —— "中断里轮转窗口导致溢出" 这条假设可以排除。

## §320 崩溃归位：栈扫描证明崩在 **task1（主流程）**，不是 ppTask

用 OpenOCD 的 `mdw` 顺着 `sp` 向下扫 `0x3fcbe700..0x3fcbeb00`，把像代码地址的
字逐个 `addr2line`：

```
[3fcbe81c] = 42017738 os_sched_preempt_ctx
[3fcbe87c] = 420545c4 ram_iq_est_enable      <- PHY 早期留下的旧值
[3fcbe8a0] = 40055cdc ??                      <- ROM
[3fcbe8cc] = 40041a76 ??                      <- ROM
[3fcbea98] = 403817f0 ppTask                  <- 库任务函数体（不是返回地址）
```

结论：
1. `0x3fcbeaf0` 落在 **task1 的栈区**（`0x3fcbce50..0x3fcbee50`），所以撞死的是
   **call0 主流程那个任务**，`ppTask` 那边只是正常阻塞在 `_queue_recv`
   （反汇编 `ppTask` 开头：`callx8 funcs[16]` = `_wifi_thread_semphr_get`，
   然后 `callx8 funcs[29]` = `_queue_recv(xphyQueue,&msg,MAX_DELAY)`）。
2. 崩溃帧正好压在 ROM 那个函数 `entry a1,32` 的溢出区上
   （`[sp-16..-4]` ↔ 我们陷阱帧的 PS/WS/WB 槽），所以**打印出来的 `a0=0x00060900`
   很可能不是原始 a0**，而是被陷阱帧覆盖后的残留 —— 这条线索不能再用。

## §321 已排除的两个方向（都试过，症状不变）

| 试的改动 | 结果 |
|---|---|
| 把 `[recv]` 那条 `ets_printf` 删掉（改成纯 RTC 记录） | 症状**完全不变**，pc/a0/cause 一模一样 ⇒ 不是这条打印惹的 |
| 给 `W2C_TRACE`（阻塞 `queue_get` 走的就是它）补上 §310 那套 PS 存取 | 症状**完全不变** |
| 整体换成 `S3_W2C_SAVEALL=1`（保存全部 a0..a15 的"理论正确版"） | **更糟**：一开始就炸在 `42013fb8`、`a0=3fcb9400`（一个栈指针）⇒ 这个变体现在不能用 |

另外确认 `S3_ABI ?= call0`（`https` 目标没覆盖），所以 `irq.S` 里
level-1 handler 是 `call0 s3_timer_irq`（反汇编 `40379d37: call0 40379ac0`），
**中断路径不轮转窗口** —— "中断里轮转窗口" 这条也排除。

## §322 现在的定位结论

崩溃点唯一确定的事实是：**在 ROM 的 `retw.n`（0x40048c6e）上取到非法指令，
`PS.CALLINC=2` 而 `a0` 没有窗口标签**。也就是窗口/调用深度账目在
"我们 call0 内核 ↔ windowed 闭源库" 之间来回穿的那一层被打乱了，
而不是某一条具体语句。下一步只有两条路：

1. **统一 ABI**：`S3_ABI=windowed` 重编内核（连同 `c6` 生成的代码），
   把 `W2C`/`_win_*` 兜底全部删掉，让整个系统只有一种调用约定；
2. **精确定位**：接管 `ets_printf`（ROM 符号是 `PROVIDE` 的，我们给出强定义即可覆盖，
   真身 `0x400005d0`）记录每次调用的返回地址，把"谁在第 N 次打印时进入 ROM"钉死。

## §323 崩溃函数第一次被**点名**：ROM 的 `uart_tx_one_char`

用 JTAG 把 ROM 跳转桩表 dump 出来，配合 IDF 的 `esp32s3.rom.ld` 对齐：

```
0x400005c0: 40048b3c  a0ffff91  00000009 4004423c  ← ets_is_print_boot / ets_printf 的桩
0x40000630: ...                                     
0x40000640: 00000009  40048c30   ← uart_tx_one_char 桩指向的真身
```

| ROM 符号 | 桩地址 | 真身 | 约定 |
|---|---|---|---|
| `ets_printf` | 0x400005d0 | **0x4004423c** | windowed（`entry a1,80`） |
| `uart_tx_one_char` | 0x40000648 | **0x40048c30** | windowed（`entry a1,32`） |
| 内部轮询 | — | 0x40048804 | windowed（`entry a1,32`） |

所以 `*** TRAP pc=40048c6e` 就是 **`uart_tx_one_char` 的 `retw.n`**，
整条 ROM 打印链（printf → putc → uart_tx_one_char → 轮询）**全是 windowed**，
ROM 内部没有跨 ABI 问题 —— 出问题的只能是"进链之前的窗口账目"。

## §324 又排除两条

| 试的改动 | 结果 |
|---|---|
| `RESTORE_FRAME` 把恢复的 `PS.CALLINC`(16:17) 清 0 再 `wsr ps` | 症状完全不变 ⇒ 不是恢复出来的 CALLINC 不对 |
| 编译加 `-DS3_IRQ_HOLD_OFF`（**定时器中断根本不打开**，没有任何中断/切换） | 症状**一模一样** ⇒ 崩溃与任务切换、与 `RESTORE_FRAME`、与中断路径**全都无关** |

后一条很关键：它把范围压缩到"**纯调用链**"—— 只有我们的 windowed 壳
（`W2C`/`WIN_SPILL`）、闭源库、ROM、以及**窗口溢出/欠载向量**这几样东西在跑。

## §325 想接管 `ets_printf` 失败了（记下来省得再试）

`esp32s3.rom.ld` 里是**普通符号赋值**（`ets_printf = 0x400005d0;`），不是
`PROVIDE(...)`，链接脚本里的赋值优先，我们自己定义强符号也覆盖不了 ——
`nm` 里只会看到 `400005d0 A ets_printf`。所以"给 `ets_printf` 打探针记录调用者"
这条路走不通（探针文件已删）。要探，只能从 `_vec_exc` 或者窗口向量里做。

顺带记录：`s3_trap` 的现场记录里 `PS` 是 `rsr ps`（**异常处理器的 PS，EXCM=1**），
不是被中断时的 PS —— 所以 `*** TRAP` 里打印的 `callinc=` 一直不可信，
别再拿它推结论。

## §326 给 `_win_ovf12`/`_win_unf12` 补上和 ovf8/unf8 一样的兜底（防守性）

四个窗口向量里，§292 当初只给 `ovf8/unf8` 加了"上一级 sp 为 0 就退回本帧"的
兜底，`ovf12/unf12` 没有：

```asm
_win_ovf12:
    s32e a0, a13, -16
    l32e a0, a1, -12      ; 上一级 sp，可能是 0
    s32e a1, a13, -12
    ...
    s32e a4, a0, -48      ; a0=0 时 → 往 0xffffffd0 写！
```

而 §312 真板上确实抓到过 `EXCCAUSE=0x1d(StoreProhibited) vaddr=0xffffffd1`
—— 正好是 `a0-48`（a0=0）算出来的地址，说明**这条路径真的会走到**。
现已补上兜底（`bnez/bnez` + `mov a0,a13` / `mov a1,a13` / `mov a11,a13`）。

补完症状不变（崩溃点照旧），所以它不是这次的直接原因，但能堵掉一次野写。

另外这一轮也确认了两条负结果：
- `-DS3_NO_WIN_SPILL`（祖先窗口完全不 spill）：症状**一模一样** ⇒ `WIN_SPILL`
  这条路径也不是本次崩溃的原因。
- 由于 ovf8 的兜底里"存"用的是 `[a0-32..-20]`（a0 可能被换成 a9），
  而 unf8 恢复时"取"用的是 `[a7-32..-20]`（a7 来自 `[a1-12]`，可能也被换成 a9），
  **两边兜底触发条件不同步时会存/取到不同地址** —— 这是下一个要处理的疑点：
  要么把兜底去掉（改用官方向量 + 在任务启动时就把 `[sp-12]` 写成合法值），
  要么让 ovf/unf 的兜底严格对称。

## §327 现场里能确定的东西（这一轮把数据补全了）

从 RTC `0x50000140`（`s3_trap` 在**打印之前**落的现场）读出来的九个字，按代码
布局是 `cause, vaddr, pc, ps(rsr ps), frame_sp, f[0], f[1], f[15](=EPC), f[16](=PS)`：

```
cause=0  vaddr=0  pc=40048c6e  ps=00060f10  frame_sp=3fcbeaf0
f[0](a0)=00060900   f[16](帧里的PS)=00060f00   f[15](EPC)=40048c6e
```

- 帧里的 PS = `0x00060f00` ⇒ **WOE=1(bit18)、EXCM=0、CALLINC=2(bit17:16)、INTLEVEL=0**。
  `rsr ps` 那个 `0x00060f10` 只是多了 EXCM=1（异常处理器自己的 PS），以后只看帧里的。
- 所以 `retw` 变非法**不是** WOE=0，也**不是** EXCM=1 ⇒ 只剩 `n==0`（a0 没有窗口标签）
  或 `m!=n`（CALLINC 与 a0 标签不一致）两种。帧里 `a0=0x00060900` 的 bit31:30 = 0
  ⇒ **就是 `n==0`：a0 被写成了一个没有标签的值**（而且这个值恰好长得很像 PS）。
- `uart_tx_one_char` 自己的 prologue 是 `entry a1,32; call8 0x400442a4 ...`，
  `call8` 会把 CALLINC 置 1 —— 而现场是 2，说明**进入它的那次调用用的是 call12**
  （ROM 的 vprintf 用 `callx12` 调 putc1 钩子是可能的），或者中途有人改了 PS。

## §328 反查"windowed 代码有没有直接调 call0 函数"——干净

把 `wifi_glue.c` / `win_island.c` 里所有 `os_* / port_* / c6_*` 调用点列出来逐个查：
- `port_cur_ctx()` 只有声明，实际用的是 `s3_w2c_current_task()` 壳（注释里已经写明原因）；
- `c6_lwip_*` / `c6_log_fmt` / `c6_ok_str` / `c6_wpa_crypto_*` 要么是 wifi_glue.c 自己
  的 static 函数，要么（`c6_lwip_start`）本身就是 `entry a1,64` 的 **windowed** 代码；
- 没有发现"windowed 代码直接 call8 进 call0 函数"的漏网之鱼。

## §329 下一步的取舍（两条路，准备动手的是第 1 条）

1. **统一成 windowed 全量构建**（`S3_ABI=windowed`）：内核 `.c6` 生成的 C 也用
   windowed 编，`s3_w2c_*` 这层壳通过头文件 `#define` 直接映射到真函数（同一 ABI，
   壳就不需要了）；`irq.S`/`ctx.S` 里已经有 `#ifdef __XTENSA_CALL0_ABI__` 的另一半
   分支（`call8` + 置 WOE）可以直接用上。跨 ABI 这一整类问题就消失了。
2. 继续在 call0 内核 + windowed 库这套混合架构上抠：目前只剩"a0 被写成无标签值"
   这一条线索，还要找出是谁写的。

风险提示：走 1 之后窗口溢出/欠载向量变成**唯一**的窗口机制（全 windowed 调用链
比现在深得多），§326 说的"四个向量必须严格对称"会变成硬要求。

## §330 "ISR 里 spill 被打断的窗口链" —— 试了，反而更早崩，已回滚

思路（§329 的推论）：异常不轮转窗口，所以从 ISR 里 `callx8 s3_win_spill`
看到的"祖先窗口"正是被打断的那条链；spill 完 `WINDOWSTART` 就只剩当前窗口一位，
和 `RESTORE_FRAME` 的"单窗口"假设一致。为此还要临时把 `PS.EXCM` 清掉
（EXCM=1 时 call8 不轮转/不溢出）并把 INTLEVEL 提到 15。

真板结果：**退化**——原来能顺利跑完 `[phy] register_chipv7_phy rc=1`，加了这一步
之后直接卡在 `[phy] calling register_chipv7_phy(mode=0)` 就 TRAP 了。
说明在 ISR 里对闭源库的活窗口做显式 spill 会破坏库自己的寄存器保存（它和
`W2C` 壳里的 `WIN_SPILL` 会互相踩同一批溢出槽）。**已回滚**（`irq.S` 恢复到最后
一次提交的状态，仍保留 §326 的 ovf12/unf12 兜底）。

顺带确认（本轮查证的库可用性，避免重复造轮子）：
- **DHCP**：`port/s3/lwip/lwipopts.h` 里 `LWIP_DHCP 1` 已经打开，DHCP 客户端
  直接用 lwIP 自带的 `dhcp.c`，不需要自己写。
- **TLS**：IDF 源码树里有完整的 mbedTLS
  （`$IDF/components/mbedtls/mbedtls/library/`），能拿 xtensa 工具链直接编；
  现有 `net/tls12_*.c6` 是之前的极简实现。等 WiFi 关联这段通了，优先评估
  直接用 mbedTLS（DRAM/IRAM 预算允许的话），不再自己维护 TLS 状态机。

## §331 ISR 里 spill 加 WOE 判断再试：还是不行，回滚

加了"只有被打断的代码 `PS.WOE=1`（在 windowed 里）才 spill"的判断后重试：
不再像上一轮那样立刻崩，但 **`[phy] calling register_chipv7_phy(mode=0)` 之后就没了**
（没有 `rc=1`、也没有 TRAP）—— PHY 初始化跑不完。

原因推断：闭源 PHY/库自己在跑的时候，活窗口和它们的 sp 链并不是我们
`WIN_SPILL` 那套假设的样子；在 ISR 里对这些窗口做显式 spill 会**覆盖库正在用的
溢出槽**（同一个栈帧的 [sp-16..sp-4]）。这条路上又试死了一次，**已回滚**。

结论（重要，别再重复）：**在我们这套"call0 内核 + windowed 闭源库"的混合架构里，
不能从 ISR 或内核栈上对库的活窗口做 spill。** 窗口只能由硬件向量按需要 spill/fill。

## §332 因此下一步只剩"全 windowed 构建"这一条自洽的路

混合架构无解的原因已经很清楚：
- 从 **W2C 壳**切走时：窗口链在壳里可见，`WIN_SPILL` 能干净地 spill ✓；
- 从 **ISR/内核**切走时：被打断的窗口链要么是闭源库的（不能碰，§331），
  要么根本不存在（call0），而 `RESTORE_FRAME` 的"单窗口"假设又要求
  切走时只剩一个活窗口 —— 两者矛盾。

全 windowed 构建下这个矛盾消失：**所有**任务都在窗口里，切走时必然可以从
当前窗口顺着 [sp-12] 链干净地 spill（FreeRTOS/IDF 的做法），
`RESTORE_FRAME` 的单窗口假设天然成立。要改的点：
1. `S3_ABI=windowed`（内核 `.c6` 生成的 C 也用 windowed 编）；
2. `w2c.S` 里的 `W2C`/`W2C_TRACE` 改成 `.set name, target` 别名（同 ABI 不需要壳）；
3. `port_task_switch` 里恢复 `call8 s3_win_spill`（全 windowed 下这条链是合法的）；
4. `irq.S` 用现成的 `#else`（windowed）分支：置 WOE 后 `call8 s3_timer_irq`；
5. `ctx.S` 的 `S3_SPILL_ALL`（rotw 链）仍然不要用，改走 IDF 的 `s3_win_spill`。

## §333 全 windowed 构建：能编能链，但连第一条打印都到不了

按 §332 动手做了第 1、2 步：
- `w2c.S` 里加了 `#ifdef S3_ONE_ABI`：`W2C`/`W2C_TRACE` 退化成
  `.set name, target` 别名（全 windowed 时不需要壳）✓ 已提交；
- `make S3_ABI=windowed S3_C6FLAGS="… -DS3_ONE_ABI"` **编译链接全通过**，
  镜像也生成了 ✓。

真板结果：bootloader 打印完 `entry 0x403c88f4` 之后**彻底安静**，
连 `M1`/`M2` 这些最早期标记都没有 → 死在 `start.S` 里。
原因很清楚：`start.S` 也是用 `-mabi=$(S3_ABI)` 编的，切成 windowed 后
它会被当成 `entry/retw` 的 windowed 函数，而我们是**从 ROM 直接跳进来的**
（call0 约定），入口就对不上。

要做全 windowed 还得改至少这些：
1. `start.S` 保持 call0 入口（单独用 `-mabi=call0` 编），只在进入 C 之前
   手工建一个 windowed 帧（或直接 `callx8` 到 windowed 的 `main`）；
2. `port_task_switch`/`RESTORE_FRAME` 在切换点做 spill（§332 第 3 条）；
3. `irq.S` 走 windowed 分支（现成的 `#else`）。

这条路仍然自洽但工作量不小，**当前先把混合架构的已知镜像恢复回来**。

## §334 血的教训：换 `S3_ABI`/`S3_ONE_ABI` 必须 `rm -rf build/wifi`

这一轮中途出现过一次"崩溃点跑到 `4204e3ac`、`a0=0x00060d00`"的怪现象，
排查发现是**构建目录里的陈旧对象**：`build/wifi/obj/*.o` 不会因为编译选项
（`-mabi=call0` ↔ `-mabi=windowed`、`-DS3_ONE_ABI`）变化而失效，
`make` 只比时间戳。于是 call0 和 windowed 的目标文件被混在同一个镜像里，
表现出来的崩溃点毫无意义。

⇒ **凡是要改 `S3_ABI` / `S3_SHIM_ABI` / `S3_ONE_ABI` / ABI 相关 `-D`，
先 `rm -rf build/wifi` 再编。** 同理，§333 那个"全 windowed 构建死在
`start.S`"的结论是在脏目录下得到的，**不可信，需要重做**。

## §335 全 windowed 构建的干净复测 + 现场：窗口账目彻底乱了

清空 `build/wifi` 重编（§334 的教训），`S3_ABI=windowed -DS3_ONE_ABI` 依然
一进 `_start` 就没输出。用 JTAG 读 `_vec_default` 的现场记录（`0x50000100`）：

```
marker=0000bad0  cause=0000001d(StoreProhibited)  vaddr=fe1f80e0
epc1=4004e4c3(ROM)  ps=00060710  a0=fe1f8100  a1=3fcd9000
windowbase=0x9   windowstart=0xaaaa   槽位=0x0f(双重异常)
```

`a1` 是我们的栈顶 ✓（说明 `_start` 已经跑起来，RTC/DRAM 标记 `A1` 也写了），
但 **`windowbase=9`（不是 4 的倍数）、`windowstart=0xaaaa`（根本不是合法掩码）**
—— 窗口状态从一开始就是乱的。切 windowed ABI 之后，`start.S` 里那套
"4 层嵌套 call8 预热窗口"（`call8 .Lprime1..4`）不再成立。

## §336 决定：直接借用 IDF/FreeRTOS 的 Xtensa 端口汇编当底座

手写窗口向量 + 上下文切换一共试了 60 多轮，混合 ABI 和全 windowed 两条路都卡在
同一件事上：**跨任务保存/恢复整个寄存器窗口链**。这件事 IDF 已经写好了，
而且源码就在本机：

| 文件 | 行数 | 作用 |
|---|---|---|
| `components/xtensa/xtensa_vectors.S` | — | 完整向量表：4/8/12 溢出/欠载、level-2..7、双重异常、窗口 spill |
| `components/xtensa/xtensa_context.S` | — | `xthal_*` 上下文保存/恢复 |
| `components/freertos/.../portable/xtensa/portasm.S` | 753 | FreeRTOS 的任务切换（`_frxt_dispatch` 等） |
| `components/freertos/.../portable/xtensa/port.c` | 686 | 同上（C 侧） |

下一步：把这套**已被无数产品验证过**的窗口/向量/切换代码移植进来，替换
`port/s3/irq.S` 的手写向量和 `ctx.S` 的 `port_task_switch`/`RESTORE_FRAME`，
我们的调度器 API 保持不动。这条路比继续抠手写汇编靠谱得多。

## §337 把陷阱帧扩到 160 字节再 spill：还是不行

思路：§331 里 ISR 内的 spill 会把 assist 的帧（[sp-48, sp)）压在自己的 80 字节
陷阱帧上，把现场写花。于是把 `_vec_level1` 的帧扩到 160 字节，
逻辑帧（a0@0..a15@56/EPC@60/PS@64）搬到 `a1+80`，顶上 80 字节空出来给 assist；
`ps.WOE=1` 时才 spill，spill 时把 a1 换成"被打断的 sp"让链从正确的位置往下走。

真板结果：**还是卡在 `[phy] calling register_chipv7_phy(mode=0)`**，和 §331 一样。
⇒ 结论不是"帧布局撞了"，而是**在 ISR 里对闭源库的活窗口做显式 spill 这件事本身
就和库不兼容**（库对 `[sp-16..-4]` 这批溢出槽有自己的假设，或者它的窗口链里
有我们不该碰的帧）。**已回滚。**

至此"手写向量 + 手写切换"这一路已经试穿：无论怎么改，只要跨 windowed 上下文
切换就会坏。按 §336 的结论，接下来移植 IDF/FreeRTOS 的 Xtensa 端口汇编。

## §338 用 RTC 探针把崩溃点夹住了：崩在**切走之后、新任务里**

在阻塞路径上加了三处纯 RTC 探针（不经过串口）：

| 探针 | 位置 | 真板结果 |
|---|---|---|
| `0xB1` | `os_task_block_prepare` 打完 `'b'` 之后 | **没写进 RTC**（被后续噪声盖掉，不可靠） |
| `0xB2` | `os_task_block_wait` 入口 | **有值** ✓ |
| `0xB3` | `port_task_yield()` 返回之后 | 没有（说明**再也没切回来**） |

⇒ 阻塞路径本身走到了 `port_task_yield()`（也就是做了上下文切换），
崩的是**切过去以后在目标任务里跑的代码**。结合 `S3_NO_PREEMPT` 已经关掉了定时器
抢占、而崩溃 PC 又落在 ROM 的 windowed 打印代码（`uart_tx_one_char` 的 `retw.n`），
最合理的解释是：**这次切换切到了闭源库自己建的新任务（`ppTask`，id=2），
它一开口用 ROM 打印就 `retw` 炸了** —— 也就是我们
`s3_task_tramp` / 新任务初始帧 / 窗口状态这套"任务第一次被调度"的代码有问题。

下一轮就从这里下手：审计 `port/s3/ctx.S` 的 `s3_task_tramp`（a4=ABI 标志、设 WOE、
`callx8`）、`port_task_create` 造初始帧时写进去的 PS/CALLINC/a1/a0，
以及 `RESTORE_FRAME` 里 `WINDOWSTART` 归一化对"新 windowed 任务"是否正确。

## §339 新任务初始帧没清 WS/WB 槽（潜在 bug，已补；但不是当前崩溃的原因）

`port_task_ctx_create` 只把初始帧的前 17 个字（a0..a15 + EPC + PS）清零，
帧偏移 68（WINDOWSTART）和 72（WINDOWBASE）**没清**。`RESTORE_FRAME` 看到
"WINDOWSTART 非 0"就会跳过 `1<<WINDOWBASE` 归一化、把垃圾直接写进 WINDOWSTART。
已改成清满 20 个字。

真板上症状**不变**（还是 `[give]` 之后 `pc=40048c6e cause=0 a0=0x00060900`），
因为栈池在 BSS 里、第一次用到时本来就是 0 —— 但这是个真隐患，留着补丁。

另外确认了一件可疑的事：`RESTORE_FRAME` 里
`wsr a3, windowstart` 会**重新映射所有虚拟寄存器（包括 a1）**，
文件里 60 行处的注释也说"要先用全局变量把帧指针存起来"，但现在的代码
**并没有**做这一步 —— 只是因为我们写进去的 `1<<WINDOWBASE` 和当前 WB 一致，
硬件才没有搬 WB。如果哪天写进去的是别的值，`l32i a2, a1, 60` 就会读到错的地址。
下一轮的探针就打在 `RESTORE_FRAME` 里：记录进入时的 a1、写入的 WS、
`wsr` 之后的 WB、以及随后读出来的 EPC/PS/a0。

## §340 RTC 诊断环在开机时清零（否则会读到上一轮的记录）

真板上踩过：把上一轮留下的 switch 记录当成这一轮的，误判成"`RESTORE_FRAME`
没跑"。RTC 慢速内存在**复位后不会丢**，而 `flashbl.sh` 只是复位芯片。
现在 `start.S` 在清完 `.bss` 后把 `0x50000200..0x500007FF` 全部清 0
（`0x50000000..0x500000FF` 留给开机标记）。清零之后所有 RTC 记录都是**本轮**的，
诊断结论才可信。

## §341 新任务一律从**窗口 0** 开始 —— 崩溃（TRAP）消失了

`RESTORE_FRAME` 给新任务只做了 `WINDOWSTART = 1 << WINDOWBASE`，
但 `WINDOWBASE` 是**上一个任务**留下的（真板读到过 **9** —— 不是 4 的倍数，
本身就是非法值，硬件只有在 `rotw` 奇数步进时才会到那种值）。
于是新任务（闭源库的 `ppTask`）第一次 `call8/retw` 窗口账目就是错的，
表现就是 `[give]` 之后 `retw.n` cause=0、a0 变成 PS 值。

改成：新任务（帧里 WS==0）**把 `wsr windowbase` 强制归零、WS=1**，从干净的
窗口 0 起跑。`wsr windowbase` 会重映射虚拟寄存器（含 a1），所以先用全局
`g_ctx_tmp_sp` 把帧指针藏起来再换。

真板结果：**`*** TRAP` 不再出现** ✓（原来是 trap，现在是"卡住"）。

## §342 补上 Alloca 异常处理（0x340 用户异常向量）

顺带发现 0x340（用户异常向量）以前是 `j _vec_default`（只记现场 + 死循环）。
windowed ABI 下编译器用 `movsp` 做动态栈分配，硬件抛 **Alloca 异常(cause=5)**，
不处理的话被中断的 `movsp` 永远不完成，而且异常入口已经 `rotw -1`，
**WINDOWBASE 会停在奇数**（真板实测 9、WS=0x200）—— 正是 §341 里那个非法值。
已照抄 IDF 的 `_xt_alloca_exc` 补上（用 PS.OWB 修正基址，再按 CALLINC
跳对应欠载向量由 `rfwu` 回到 `movsp` 重试）。

补完之后仍然卡住（不再是 trap），现场是**双重异常**：

```
_vec_default: cause=1d(StoreProhibited) vaddr=182d49f8 epc1=4201855a
              a0=182d4a28 a1=50000b00 windowbase=9 windowstart=0x200 slot=0x0f
```

`epc1=4201855a` 落在 `os_task_start_core`（flash），`a1=0x50000b00` 是
**RTC 内存**——内核代码带着一个假的栈指针在跑。⇒ 下一个要查的是：
这条 `os_task_start` 调用链（OSI `_task_create` → `os_task_start` →
`os_task_start_core`）里，a1 是在哪一步被换掉的（`port_task_ctx_create`
分配失败？还是窗口恢复时 a1 被错误覆盖？）。

## §343 内核模式下的 Alloca 异常必须从 **0x300** 分流

`0x340`（用户异常向量）只在 `PS.UM=1` 时用；我们全程内核模式，所以
`movsp` 抛出的 Alloca 异常(cause=5)是走 **0x300** 的。已在 `_vec_level1`
最前面分流：`wsr a3, excsave2` 暂存 → 读 exccause → 是 5 就
`wsr a0, excsave1` 后跳 `_xt_alloca_exc`（照抄 IDF）。其余原因不受影响。

## §344 定时器中断里的 WINDOWBASE 采样探针

`s3_timer_irq` 里第一次发现 `windowbase` 不是 4 的倍数就记到 `0x50000900`
（`0xBAD2 | wb | 被打断的 epc1 | windowstart`）。**一直没触发** —— 说明
窗口基址在中断采样的时刻都是合法的。

## §345 新任务的"干净窗口"必须三件套一致（WB / WS / PS.OWB）

- 初始 PS 里清掉 **OWB(bit8-11)**（`port_task_ctx_create`）；
- `RESTORE_FRAME` 对新任务（帧里 WS==0）用 `wsr windowbase 0` + `WS=1`；
- `wsr windowbase` 会重映射虚拟寄存器（含 a1），所以先用全局 `g_ctx_tmp_sp`
  把帧指针藏起来再换。

`rfe` 会把帧里的 PS 整个装回去，PS.OWB 决定返回后的 WINDOWBASE ——
所以这三者必须一致，否则返回后 WS 与 WB 对不上，寄存器映射整体错位。

## §346 **重大更正：0x50000300 那批"WS/WB"记录是假的**

`wifi_glue.c:560` 里有 `*(volatile u32 *)0x50000310u = (u32)q;`（记队列句柄），
`0x50000320/0x340/0x350/0x360/0x370/0x3C0` 也都被 wifi_glue 的诊断占了 ——
**正好压在 switch 记录的 `+0x10`(WS) / `+0x14`(WB) 上**。
之前所有"WINDOWBASE=9 是非法值"的推断都建立在这些被覆盖的数据上，**作废**。

已把切换点记录搬到 **`0x50000500`**（32×4 字节，确认无人使用；
`0x50000C00` 也是 console.c 在用的，别再往那儿搬）。搬完重读，WS=`0x200`、
WB=`9` —— 两者**互相自洽**，而且 Xtensa 的 WINDOWBASE 就是"窗口序号"，
9 是合法值。所以 **WB=9 从来就不是 bug**。

## §347 现在的真实故障：调度器主循环恢复后 a1 变成了一个常量

`_vec_default` 现场（`0x50000100`）：
`cause=1d(StoreProhibited) epc1=4201855a` —— 落在 `os_task_start_core`
（调度器主循环，`kernel/task.c6:603`），`a1=0x50000b00`。

而 `0x50000B00` 正是这个函数自己代码里的常量：
`volatile u32 *sd = (volatile u32 *)0x50000B00u;`（§诊断用）。
反汇编也印证：`l32i.n a10, a1, 40` 取的局部变量就是它。

⇒ **a1 拿到了本该在 a10 里的值，等于寄存器窗口整体错位了一格**，
发生在 `port_task_switch()` 返回之后。也就是说：任务从 windowed 调用链里
阻塞切走、再切回调度器循环时，**恢复出来的窗口映射是错的**。

下一轮从这里下手，两个候选：
1. 把调度器主循环本身也当成一个"任务"（有自己的 ctx/帧），所有切入切出
   都走同一套 save/restore，不再有"从 windowed 链里返回到 call0 主循环"这种混合路径；
2. 或者查 `RESTORE_FRAME` 恢复 `PS.CALLINC` 的方式：`rfe` 装回 PS 后
   CALLINC 与 WINDOWBASE 必须配套，否则下一条 `retw` 会退错一格。

## §348 全部改动落地状态

- `port/s3/irq.S`：0x300 分流 Alloca；`0x340` 用户向量的 Alloca 处理体；
  §290 标记保留。
- `port/s3/ctx.S`：新任务干净窗口（WB=0/WS=1，用 `g_ctx_tmp_sp` 藏帧指针）；
  switch 记录搬到 `0x50000500`。
- `port/s3/ctx.c`：初始帧清 20 字；初始 PS 清 OWB。
- `port/s3/start.S`：开机清 RTC 诊断环 `0x50000200..0x500007FF`。
- 真板现状：**`*** TRAP` 消失**，卡在 `[give] t=2 sem#1` + `b` 之后。

## §349 更正 §347：调度器主循环**本来就是**一个正经 ctx

读完 `port/s3/ctx.S` 的实现，§347 的猜测不成立：

- `port_task_switch(ctx)`：把现场压进 **`g_sched[core]`**（调度器主循环自己的
  ctx），然后 `RESTORE_FRAME` 到目标任务；
- `port_task_yield()`：任务阻塞时把现场存进 **任务自己的 ctx**（`g_s3_cur`），
  然后恢复 **`g_sched`** ⇒ 回到调度器主循环，由主循环再挑下一个任务。

所以"主循环 → 任务 → 阻塞 → 主循环"这条链是有 ctx 的、设计上没问题。
`RESTORE_FRAME` 结尾也有 `addi a1, a1, 80` 把帧指针还原成调用者的 sp，
新任务（`port_task_ctx_create` 的 `top = …-80`）、陷阱帧（`_vec_level1` 的
`addi a1,a1,-80`）三者一致 ⇒ a1 也不会偏 80。

⇒ 剩下的差异只有一个：**出问题的这条链是 windowed 的**（闭源库）——
它的祖先窗口是被 `w2c` 壳里的 WIN_SPILL 手工 spill 的，恢复时靠硬件欠载向量
重新 fill。下一步就在这条链上做**最小可复现实验**：
用我们自己的一个 windowed 小函数（不进闭源库）走同样的
"阻塞 → 切到另一个 windowed 任务 → 再切回来"路径，看是否同样炸；
如果炸，问题就完全在我们自己的窗口恢复逻辑里，和闭源库无关。

## §350/§351 试过的两个"恢复顺序"改动（已回滚）

1. **§350**：`RESTORE_FRAME` 里改成"先从帧里的 PS.OWB 算出 WINDOWBASE、
   `wsr windowbase` 搬过去、再写 `WINDOWSTART = 1<<WB`"，并且全程用全局
   `g_ctx_tmp_sp` 藏帧指针（因为改窗口基址会重映射 a1）。
2. **§351**：恢复 PS 时无条件补 `PS.WOE`（阻塞时帧里存的是 call0 内核的
   PS，WOE=0；不补的话任务回到 windowed 壳后第一条 `retw` 就非法）。

真板结果：**TRAP 又回来了**，而且现场变了 —— `ps=0x00060010`（WOE=0）、
`a0=0x00060000`（看着像 PS 值）。说明这两个改动并没有让"恢复 windowed 上下文"
这件事变对，只是把失败点挪了个位置。**两处都已回滚到 `809da78` 的状态**
（那个状态是"没有 TRAP，卡在 `[give]` 之后"）。

下一步仍然按 §349：先做一个**完全属于我们自己的** windowed 最小复现
（不进闭源库），把"windowed 任务阻塞 → 切走 → 切回"这条链单独跑通；
在闭源库上继续盲试性价比太低了。

## §352 用 `tests/t_winabi.c6`（纯自有代码）做最小复现，拿到了关键数据

这条用例完全不碰闭源库：两个 windowed 任务 + `s3_win_stress`（call12 递归 8 层、
四层窗口装不下，必然走 `_win_ovf*/_win_unf*`）+ `s3_w2c_task_delay`（真正切走）。

试了两种 `RESTORE_FRAME` 写法：

| 写法 | 结果 |
|---|---|
| A. 按任务写回 WS（当前 HEAD） | 崩溃点更早 |
| B. **归一化** `WS=1<<WINDOWBASE`，同时把 **PS.OWB 改成当前 WB/4**、**PS.WOE 置 1** | **`s3_wifi_test_recv` 跑通了**（`0x50001304 = 0xC0DE5678`）——说明"windowed 里阻塞 → 切走 → 切回 → 继续跑完"这条路已经通 |

B 之后仍然崩的地方很具体：`pc=0x4201062c` = **`s3_win_stress`**、`a0=0`、
`cause=0`（非法指令，就是那条 `retw`）。也就是**递归深链**里 `a0`（返回地址）
在某个窗口被填成了 0 —— 溢出时把 0 存进了栈上的 spill 槽，欠载时又原样填回来。

## §353 把 `_win_ovf*`/`_win_unf*` 改回 IDF 原版（去掉 §292/§326 的兜底）：更糟

想验证"§292/§326 那几个 `bnez a0, 1f / mov a0, a9` 兜底是不是元凶"，
把四个处理体改成一字不差的 IDF 原版 —— 结果板子在更早的启动阶段就死了
（连 `u1u2` 之后都没了）。⇒ **那些兜底是必需的**（真板上链节的
`[sp-12]` 确实可能是 0）。已回滚。

## §354 当前状态（本轮收尾）

- `port/s3/irq.S`、`port/s3/ctx.S` 回到 `809da78`（**没有 TRAP、卡在
  `[give] t=2 sem#1` + `b` 之后**那个最好状态）。
- 明确的下一步：在 `t_winabi` 上把 `s3_win_stress` 递归链的 `a0=0` 修掉
  —— 那是纯自有代码、迭代只要几十秒，而且它是 WiFi 那条链的同类问题。
  重点看：`s3_w2c_task_delay` 壳里 `WIN_SPILL` 之后，**递归帧的
  `[sp-16..-4]` 是否被壳自己的 spill 覆盖过**（壳的帧在递归帧下面 128 字节，
  溢出的 assist 帧也落在那一带）。

## §355 分阶段跑 `t_winabi`：**深链单独跑是对的，深链+切换才坏**

把压力任务拆成三段，用 `0x50001300` 的 d[5..9] 记进度：

| 阶段 | 内容 | 结果 |
|---|---|---|
| 1 | 只有 `s3_w2c_task_delay`（浅链 + 切换） | 通过（能继续往下走） |
| 2 | **只有**递归深链：`s3_win_deep12_entry(8)` + `s3_win_deep(16)`，不切换 | **完美**：`d[6]=74017` = 74*1000+17，和期望值一字不差 ⇒ 窗口溢出/欠载向量本身是对的 |
| 3 | 递归深链 + `s3_w2c_task_delay` | **depth=1 就崩**（`d[9]=1`） |

阶段 3 的 depth=1 形状是：
`s3_win_stress_task → stress(1) → stress(0) → s3_w2c_task_delay(壳)`
⇒ 切走的那一刻正好有 **3 个 windowed 帧 + 壳本身 = 4 个窗口，寄存器文件刚好装满**。
切回来时壳的 `retw` 要从 spill 槽里把祖先填回来 —— 就在这儿坏。

也就是说：**"窗口溢出/欠载向量"(阶段 2) 和 "任务切换"(阶段 1) 各自都没问题，
坏在两者叠加** —— 最可能是壳里 `WIN_SPILL` 的辅助函数在"寄存器文件已满"时
把帧写进了别处（`window_spill_function` 的 assist 帧需要 `[sp-48, sp)` 空闲，
而此刻下面正好是递归帧的 spill 槽）。

另外把 `RESTORE_FRAME` 换成 §352-B（归一化 WS + PS.OWB + WOE）在这个用例上
**更差**（`d[8]` 都没到），已回滚 ⇒ 保持"按任务恢复 WS"。

下一步（很具体）：在 `w2c.S` 的 `W2C` 壳里，把 `WIN_SPILL` 之前先
`addi a1, a1, -64` 多留 64 字节（或者改成 `entry a1, 192`），让 assist 帧
落在壳自己的空闲区里而不是递归帧的 spill 槽上；再跑阶段 3。

## §356 「只归一化 WS」（不动 PS/OWB/WOE）：更差

试了最小改动的归一化（`WS = 1<<当前WINDOWBASE`，PS 保持帧里的原值）：
`t_winabi` 变成 `cause=0x1c(LoadProhibited) vaddr=0x00000fff pc=42010691 a0=0`，
连阶段 2 都没到。**已回滚**。

三次对比（同一用例）：

| `RESTORE_FRAME` 的 WS 处理 | 结果 |
|---|---|
| **按任务写回（HEAD）** | 走到阶段 3 depth=1 才崩 —— **目前最好** |
| 归一化 + PS.OWB + WOE | `d[8]` 都没到 |
| 只归一化 | LoadProhibited，阶段 2 都没到 |

⇒ 结论：**按任务写回 WS 是对的**，问题不在这儿。
另外把压力用例的标记从 `0x50001300` 搬到 **`0x50000A40`**
（`0x50001300` 那一带又被别的诊断撞了，`d[0]/d[5]` 读出来是垃圾，
只有 `d[6]=74017`、`d[8]=0xAAAA0002` 这种明显有意义的才可信）。

## §357 按任务恢复 WINDOWBASE + 归一化 WS + 改 PS.OWB：直接起不来

把"保存时归一化 WS"+"恢复时 `wsr windowbase`(用帧里的 WB)"+"PS.OWB=WB/4"
三件事一起做，`t_winabi` **连 `u1u2` 都过不去** ⇒ 已经回滚。

目前四次对比（同一份 `RESTORE_FRAME` 代码、同一个用例）：

| 变体 | 做法 | 结果 |
|---|---|---|
| **HEAD** | 按任务写回 WS（原始值） | 阶段 3 depth=1 才崩（最好） |
| B | 归一化 WS + PS.OWB + WOE | 阶段 2 都没到 |
| B' | 只归一化 WS | LoadProhibited，阶段 2 都没到 |
| C | 保存时归一化 WS + 恢复 WB + PS.OWB | 起不来 |

⇒ 到这个程度可以确认：**WS/WB 的恢复策略不是瓶颈**（怎么改都只会更差）。
`HEAD` 的"按任务原样恢复 WS"就保留着。

## §358 WiFi 那条链的**无冲突**现场（第一次）

诊断地址搬家、开机清 RTC 之后，一次运行的四份记录（全部可信）：

```
switch  #1 : EPC=4201854c (os_task_start_core)  PS=00060800  WS=00000200  WB=9
restore #1 : frame=3fcbedf0  EPC=403797b4 (s3_task_tramp)  PS=00060000  WS=0  WB=0
trap       : cause=0  epc1=40048c6e (ROM uart_tx_one_char 的 retw.n)
             ps=00060610  frame_sp=3fcbeaf0  a0=00060400  帧内 PS=00060600(WOE=1,CALLINC=2)
tramp      : 启动过 2 个任务（wifi 主任务 + ppTask）
```

`a0=0x00060400` 和帧里的 PS `0x00060600` 长得很像 ⇒ **`a0` 被一个 PS 值顶掉了**，
这正是"寄存器窗口被填错"的典型现象（`retw.n` 于是 cause=0）。

## §359 关掉定时器中断，这个 TRAP 就**不会出现**

- `-DS3_IRQ_HOLD_OFF`（定时器中断根本不打开）：`trap` 记录区全是噪声 ⇒ **没有 trap**，
  只是卡住（PC=`40379dd4`）。
- **最小中断**（`-DS3_MINIMAL_ISR`：ISR 里只做 tick + 重装定时器，不调
  `port_irq_fill`、不调 hook）：同样**没有 trap**。

⇒ TRAP 是"中断 + `port_irq_fill`"这条路径造出来的，不是纯切换路径。
但把 `port_irq_fill` 里的两件事分别关掉（`-DS3_NO_ISR_WOE_PATCH` /
`-DS3_NO_ISR_SP_PATCH`）都不会改变现象 ⇒ 不是单独某一行的错，
更像是"中断把被打断帧的 PS/ctx 改了"这类**状态耦合**。

另外 `-DS3_NO_WIN_SPILL`（关掉壳里的祖先 spill）也一样不变。

## §360 本轮结论与下一步

1. 我们自己的 windowed 机制（溢出/欠载、阻塞/恢复、深链）在 `t_winabi`
   里已经分别验证通过；问题集中在"闭源库 + 中断 + 切换"三者叠加。
2. `port/s3/irq.c` 里新增了三个**开关式**排查点（默认全开，和原来一致）：
   `S3_MINIMAL_ISR` / `S3_NO_ISR_WOE_PATCH` / `S3_NO_ISR_SP_PATCH`。
3. 下一步：把中断路径也换成 IDF 的写法 —— 即 `_vec_level1` 里不再手工拼
   80 字节帧 + 调 C，而是按 IDF 的 `_xt_lowint1` 结构：**先把 16 个寄存器
   连同 PS/EPC 一起按 IDF 的 `XT_STK` 布局压栈**，C 处理完再按同一布局恢复。
   我们现在这套帧布局是我们自己发明的，和 IDF/FreeRTOS 的
   `XT_STK_*` 不是一回事，而闭源库的 ROM 打印链是按 IDF 的假设写的。

## §361 **真病根（之一）：中断帧压在 windowed 溢出槽上**

这一条终于对上了所有现场：

- windowed ABI 把 `[sp-16..sp-4]`（call8 还要 `[sp-32..sp-20]`，call12 到
  `[sp-48..sp-20]`）当作"被调用者保存 caller 的 a0..a11"的**溢出槽**；
- 我们的 `_vec_level1` 以前是 `addi a1, a1, -80` 就地压帧 ⇒ 帧里的 **PS 槽
  （帧+64 = 原 sp-16）正好落在溢出槽的第一格**；
- 一旦中断发生在"caller 的寄存器已经被溢出到槽里"之后，这些槽就被我们的帧
  写花；等到窗口欠载把 a0 从槽里填回来时，拿到的就是**我们的 PS 值**
  —— 真板现场 `a0=0x00060400`、帧内 PS `0x00060600`，`retw.n` 直接 cause=0
  （§358）。这就是追了十几轮的"ROM 打印链 retw 崩溃"。

修法（最小改动、不动 ABI）：中断帧整体挪到 **`[sp-128, sp-60)`** ——
`addi a1, a1, -128`，退出时 `addi a1, a1, 128`。这样帧完全低于 sp-48，
碰不到任何溢出槽。

真板结果：
- **`retw.n`/PS 那个 TRAP 消失了**（`0x50000140` 的记录区不再被写）；
- 切换次数从 1 涨到 2、恢复次数从 1 涨到 3（说明库里多跑了一段）；
- 新的失败点：`_vec_default` 记录 `cause=1c(LoadProhibited) vaddr=fffffffc
  epc1=4004909a(ROM) a1=00000008 windowbase=6 windowstart=8`（`a1` 已经是垃圾，
  WS 与 WB 也不自洽）⇒ 还有第二个独立的问题。

⚠️ 注意：`port_irq_fill()` 现在把 `c->sp` 记成这个 128 字节帧的地址，
而 `RESTORE_FRAME` 结尾只 `addi a1,a1,80` —— **一旦以后打开定时器抢占，
这两处必须一起改成 128**。当前 `-DS3_NO_PREEMPT` 下不会用到。

## §362 §361 之后的新失败点，以及 `port_irq_fill` 的取舍

- 默认构建（`port_irq_fill` 照旧把 `c->sp` 记成中断帧）：
  `cause=1c LoadProhibited vaddr=fffffffc epc1=4004909a(ROM) a1=00000008
  windowbase=6 windowstart=8` —— `a1` 已是垃圾、WS/WB 也不自洽。
  怀疑与"中断帧现在是 128 字节、而 `RESTORE_FRAME` 只 `+80`"有关。
- 加 `-DS3_NO_ISR_SP_PATCH`（完全不记 `c->sp`）：又回到老现场
  `cause=1d epc1=4201860a a1=50000b00`（就是调度器主循环 a1 被换成常量的那个）
  ⇒ 说明记录 `c->sp` 这件事本身在起正面作用，不能简单去掉。

**下一步**：要么把中断帧的**大小也统一成 80**（用 §361 的办法：帧放在
`[sp-128, sp-60)` 但只占 68 字节，可以改成 `addi a1,a1,-96` 且把布局整体
下移 —— 注意必须整体低于 sp-48），要么让 `RESTORE_FRAME` 认识"中断帧"
（帧里存一个标记/长度）。两者目标一致：**帧指针恢复出来的 sp 必须等于
被打断的 sp**。

## §363 帧健全性闸门 + 修掉一处 RTC 冲突

- `kernel/task.c6` 里"最近 8 次阻塞"的诊断区原来写 **0x50000C00**，那正好是
  console 的 backlog 区 ⇒ 把控制台缓冲写花了。已搬到 **0x50001A00**。
- `RESTORE_FRAME` 里加了闸门：EPC 不在 `0x40000000..0x43FFFFFF` 就说明这个帧是
  垃圾，直接把 (帧, EPC, PS, WS, WB, a0, a2) 记到 **0x50001880** 并原地停住，
  不再 `rfe` 跳进垃圾。真板上闸门**没有触发** ⇒ 每次恢复的 EPC 都在代码区。

## §364 中断入口的 sp 看门狗（没触发）+ 现场直读

- `_vec_level1` 入口加检查：被打断的 sp 不在 `0x3FCA0000..0x3FCEF000` 就记
  (sp, a0, epc1, ps, wb, ws) 到 **0x500018C0** 并停住。真板上**没有触发** ⇒
  中断进来时 sp 一直是好的。
- 直接连 JTAG 读活寄存器（不是记录），崩住时是：

  ```
  pc = 0x40379e48   ← _vec_default 里那句 `j 1b` 自旋
  a0 = 0x182d6a28   ← 垃圾（每次运行都变，和 vaddr 相差 0x30）
  a1 = 0x50000b00   ← **恰好是 IRQ_RING_BASE 这个常量**
  ps = 0x00060610   exccause=0x1d(StoreProhibited)  epc1=0x4201860e
  ```
  `epc1=0x4201860e` 落在 `os_task_start_core+0x226`，但反汇编显示那里是
  `l8ui` 的**中间字节**（0x4201860c 起 3 字节）⇒ **EPC1 不是真 PC**，是被
  软件写进去的（我们只有 `wsr epc1` 两处，都从帧+60 取）。
  ⇒ 说明**某次切换帧里的 EPC/a0 本身就是垃圾**，而那个垃圾来自于
  "调用 `port_task_switch` 时 a0 已经被毁"。
- `dbg2`（0x50001800）显示**最后一次**切换是干净的：
  `switch_out_sp=3fcd8eb0 target_ctx=3fcc2e40 restored_sp=3fcbedf0`，
  恢复记录（0x50000400）两条 EPC 也都在 `s3_task_tramp` 里 ⇒ 坏的那个帧
  出现在更早、记录环写过之后。

**下一步（方向已定）**：`-DS3_ONE_ABI`（全 windowed 构建）在 §335 之后没再试过，
而 §361 正好修掉了当时最可能的干扰源（中断踩溢出槽）。全 windowed 能把混合
ABI 的壳、spill、win_bridge 全部去掉，值得重测一次。

## §365 全 windowed 构建（`-DS3_ONE_ABI` + `S3_ABI=windowed`）在 §361 之后重测：仍然起不来

干净重建后能链接成功，但真板**连 `c6os s3 boot` 都打不出来**（ROM 引导打印完
就没有了，`Saved PC=0x40379d98`）。所以"用全 windowed 消掉混合 ABI"这条路
还需要把 `start.S`/`irq.S`/`ctx.S`/`win_bridge.S`/`abi_thunk.S` 里
"假定内核是 call0"的地方逐个改成 windowed —— 不是一轮能做完的事。
已回滚到标准构建（`S3_ABI=call0` + 壳）。

**当前最好状态**（就是本轮的默认镜像）：
开机 → `[phy] register_chipv7_phy rc=1` → `W1 bringup` → `[net80211]` →
`[task] 库建任务` → `[give] t=2 sem#1` → `b`，然后停在 `_vec_default`
（活寄存器：`pc=40379e48`、`a1=0x50000b00`、`exccause=1d`、
`epc1=0x4201860e` 是软件写入的假 PC）。

## §366 跳过库任务（`S3_NO_LIB_TASKS`）——开关已加，但宏必须走 `S3_CDEFS`

`wifi_glue.c` 是用 `$(S3_CDEFS)` 编的，所以排查宏要写成
`S3_CDEFS="-DS3_... "`，写在 `S3_C6FLAGS` 里到不了它（这轮先踩了一次）。
开关本身：`#ifdef S3_NO_LIB_TASKS` 时不真的建库任务，只回一个假句柄，
用来判定崩溃是"库任务开始执行后"还是"主流程自己继续走"就会出。

## §367 **又一个真 bug：W2C 壳没有保存 a12..a15（= 调用者的 a4..a7）**

`port/s3/w2c.S` 的默认 `W2C` 壳只存了 `a0`（和 PS），然后
`callx0` 进 call0 内核 —— 而 GCC 生成的 call0 代码把 **a12..a15 当
caller-saved 随手改**。

问题在于：壳是 **windowed** 函数，被闭源库用 `call8` 调进来。窗口轮转 8 个
寄存器后，壳的 **a12..a15 正是调用者的 a4..a7**，而 a4..a7 在 windowed ABI 里
是 **callee-saved** —— 库把长期存活的对象指针放在那儿（ppTask、net80211 都是
这么编的），根本没料到有人会改。

⇒ **每一次 OSI 回调（semphr_take / queue_recv / delay …）都会毁掉库的 4 个
长期指针**。之后出现"垃圾基址 + 假返回地址"的现场（§364 的
`a1=0x50000b00`、`epc1=0x4201860e`、`a0/vaddr` 差 0x30 这类）就完全说得通。

修法：默认壳里加上 `s32i a12..a15, a1, 24/28/32/36` 和对应的 `l32i`；
`W2C_TRACE` 也补上 a14/a15。`port/s3/win_bridge.S` 早就存了 a12..a15，只有
`w2c.S` 这份漏了。

（§245 的 `S3_W2C_SAVEALL` 早就写过这个要点，但它那份没有 PS 保存和 spill
死区，所以两者要合并 —— 现在默认实现就是合并后的。）

## §368 双 ABI 边界的整体复查（§367 之后）

把"windowed 代码 → call0 代码"的每条路都过了一遍：

| 桥 | 保存 a12..a15？ | 备注 |
|---|---|---|
| `port/s3/w2c.S` 默认 `W2C` | **原来没有 ← 本轮修了** | 库/ lwIP 调 OSI 回调全走这里 |
| `port/s3/w2c.S` `W2C_TRACE` | 原来只存 a12/a13 | 已补 a14/a15 |
| `port/s3/win_bridge.S` | 有 | 一直是对的 |
| `port/s3/abi_thunk.S` | 有（文件头注释还专门写了这条规则） | 正确 |

另外确认：`wifi_glue.c` / `win_island.c` / `lwip/c6_lwip_port.c` /
`lwip/sys_arch.c` **都只经 `s3_w2c_*` / `s3_win_*` 桥**调进内核，没有直接
（违反 ABI 地）调用 call0 函数；窗口化的 lwIP 也是 `-mabi=windowed` 编的，
`sys_arch.c` 用的是 `s3_w2c_sem_*` 桥 ⇒ 这四个文件的 OSI 路径都被 §367 覆盖到。

> ⚠️ 硬件侧：本轮 ESP32-S3 的 USB-Serial-JTAG 整条从总线上掉下去了
> （`lsusb` 里已经没有 `303a:1001`，`/dev/ttyACM*` 不存在，openocd 报
> `libusb_get_string_descriptor_ascii() failed`），**需要物理插拔/断电重插**
> 才能继续烧写和串口验证。§367 的修复已经编好（`build/wifi/c6os.bin`），
> 等设备回来就能直接验证。

## §369 同类漏洞补齐：`osi_queue_recv` 专用壳也没有存 a14/a15

`port/s3/w2c.S` 里手写的那条 `osi_queue_recv` 壳（ppTask 真正阻塞的地方）
只存了 a12/a13，同样漏了 **a14/a15 = 调用者的 a6/a7**（callee-saved）。
已补 `s32i a14, a1, 28` / `s32i a15, a1, 32` 与对应恢复。

顺带把 `w2c.S` 里所有 `callx0/callx8` 点过了一遍：
133 行（`S3_W2C_SAVEALL` 版，存了全部 16 个）、
153 行（`S3_W2C_MIN` 极简版，仅排查用、默认不开）、
190 行（默认壳，§367 已修）、
233 行（`W2C_TRACE`，§367 已修）、
`osi_queue_recv`（本条）。
⇒ **默认构建里所有 windowed→call0 的入口现在都满足 windowed ABI 的
callee-saved 约定**。

## §370 自愈看门狗：定时器中断里喂 RTC 看门狗

这轮板子"CPU 死转 → USB-Serial-JTAG 整条从总线消失"，只能人工断电重插。
`port/s3/flash.c` 里早就有 `os_wdt_start/os_wdt_feed`（RWDT，复位整片），
但**没有任何地方去 arm 它**（`-DS3_NO_WDT` 在代码里根本没有对应实现，
是个空开关）。

现在在 `s3_timer_irq()`（定时器中断，10ms 一次）里做两件事：
第一次进去时 `os_wdt_start(8)`，之后每次 `os_wdt_feed()`。
⇒ 中断还在跑就一直喂；真卡死就 8 秒后整片复位、串口重新枚举，
**不用再人工断电**，而 RTC 慢存跨复位不清，死之前的信标照样能读。

## §371 ⚠️ §370 的自动看门狗**必须默认关**（已改成 `-DS3_AUTO_WDT` 才开）

上板后果很严重：应用只要卡死（中断一停），8 秒后 RTC 看门狗整片复位，
于是板子进入**无限复位循环** —— USB-Serial-JTAG 每 8 秒断一次，
主机侧根本来不及 `set config #1`：

```
usb 2-4: USB disconnect, device number 108
usb 2-4: new full-speed USB device number 109
cdc_acm 2-4:1.0: ttyACM1: USB ACM device
usb 2-4: can't set config #1, error -71      ← 每 8 秒重复
```

`/dev/ttyACM*` 忽有忽无，openocd 报 `libusb_get_string_descriptor_ascii() failed`，
esptool 报 `OSError: [Errno 71] Protocol error` —— **烧写和串口都被自己搞死**。
这就是上一轮"设备从总线上消失"的真正原因（不是硬件坏）。

⇒ 改成 `#ifdef S3_AUTO_WDT` 才 arm，默认关闭。要用自愈狗调试时再加，
而且**只在本来就会卡死的实验镜像上用**。

## §372 **活 bug：§361 把中断帧改成 128 字节，但恢复路径还按 80 弹**

`RESTORE_FRAME` 结尾原来写死 `addi a1, a1, 80`，而 §361 之后
`port_irq_fill()` 恰好把 `c->sp` 记成那个 **128 字节**的中断帧
⇒ **任何被中断过的任务，恢复后 sp 都低 48 字节**，直接踩在别人栈上。
这和 §364 观察到的"垃圾 a1 / 假 EPC"完全同类。

修法不是把 80 改成 128（那样"ISR 直接切任务"的 80 字节帧又错了），
而是让**帧自描述**：偏移 **76** 存"恢复这个帧之后 sp 应该是多少"。

| 产出帧的地方 | 写的值 |
|---|---|
| `port_task_switch` | `a1 + 80` |
| `port_task_yield` | `a1 + 80` |
| `_vec_level1` | `a1 + 128` |
| `port_task_ctx_create` | `top + 80` |

两处恢复路径（`RESTORE_FRAME` 与 `_vec_level1` 末尾）都改成
**`l32i a1, a1, 76` 自加载写法** —— 那时 a0..a15 已经全部恢复好，
任何临时寄存器都会毁掉刚恢复的值；自加载只用旧 a1 当基址，一条指令搞定。

⚠️ 这个 bug 之前被 §363 的"帧闸门"和 §364 的"sp 看门狗"挡住了症状（都被判为
"没触发"），所以一直没暴露成显式崩溃，只是让每次切换后的 sp 漂移。

## §373 又一处 RTC 区重叠：调度诊断环 vs `set_isr` 环

`kernel/task.c6` 的 `os_task_start_core()` 里那个"最近 8 次调度选中"的诊断
用的是 **0x50000B00**，而 `port/s3/wifi_glue.c` 的 `IRQ_RING_BASE`
（`esp_intr_alloc` → `_set_isr` 的环形记录）**也是 0x50000B00**。
两边互相覆盖 ⇒ 读出来的"调度记录"里全是中断号，"set_isr 记录"里全是
任务 id/状态。这也是 §364 里 `a1=0x50000b00` 那个假象的一部分来源
（那个常量正好是这个基址）。

搬到 **0x50001B00**。现在的 RTC 慢存分工（已无重叠）：

| 地址 | 用途 |
|---|---|
| 0x50000000 | 开机标记 |
| 0x50000100 / 0x50000140 | 默认向量记录 / 首次陷阱记录 |
| 0x50000200 | OSI 函数表 |
| 0x50000300 | wifi_glue 诊断（0x310/0x320/0x340/0x350/0x360/0x370/0x3C0） |
| 0x50000400 | 恢复记录环 |
| 0x50000460 | ctx 分配诊断 |
| **0x50000500** | 切换记录环 |
| 0x50000600 | 收包诊断 |
| 0x50000700 / 0x50000780 | tick 计数 / 被打断 PC 采样 |
| 0x50000900 | WB 探针 |
| **0x50000A00 / 0x50000A10 / 0x50000A40** | tramp 记录 / 阶段信标 / stress 标记 |
| 0x50000B00 | `set_isr` 环（wifi_glue） |
| 0x50000BE0 | ctx 池诊断 |
| 0x50000C00 | console backlog |
| 0x50000F00–0x50000F18 | 库返回地址槽 |
| 0x50001000 | switch 计数（ctx.S） |
| 0x50001100–0x50001600 | 各阶段诊断 |
| 0x50001700 / 0x500017C0 | ISR ctx 记录 / fill 拒绝 |
| 0x50001800 | dbg2 |
| **0x50001880 / 0x500018C0** | 帧闸门 / sp 看门狗（停机会写） |
| **0x50001A00 / 0x50001B00** | 阻塞记录 / 调度选中记录（本轮搬家） |
| 0x50001C00 / 0x50001D00 / 0x50001F00 | 其它诊断 |

## §374 调度热路径上的 RTC 诊断写全部删掉

`os_task_start_core()` 每次选中任务都往 RTC 慢存写 3 个字。这条循环是**每次
上下文切换都要跑**的热路径，而它写的正是"切换帧已经坏了"时最需要干净的地方。
真板现场直接暴露了这一点：`a1` 精确等于那个诊断基址常量（先是
`0x50000b00`，搬到 `0x50001b00` 后 `a1` 立刻跟着变成 `0x50001b00`）——
不是巧合，是诊断代码自己把 a1 换成了字面量地址。
改成只在内存里计数（`g_dbg_pick_n`），不写 RTC。

## §375 `_vec_level1` 的帧**从来没写过偏移 68/72**（又一个真 bug）

`port_irq_fill()` 会把这个中断帧记成任务的保存点，之后走 `RESTORE_FRAME`；
而 `RESTORE_FRAME` 的逻辑是：

```asm
l32i a3, a1, 68      /* WINDOWSTART */
bnez a3, 7f
   ... 归一化 ...
7: wsr  a3, windowstart
```

`ctx.S` 的保存路径（`port_task_switch` / `port_task_yield`）一直写这两格，
**只有中断帧漏了** ⇒ 读出来是栈上的旧内存。真板现场完全对得上：

```
windowstart = 0x00000200     ← 栈上的垃圾
windowbase  = 0x00000009     ← 奇数！非法
a1          = 0x0008437f     ← 垃圾 sp（`wsr windowstart` 把窗口重映射后 a1 就废了）
```

已补上 `rsr windowstart/windowbase` → `s32i 68/72`。

**当前剩下的最后一道坎**（下一步）：`[give] t=2 sem#1` → `b` 之后卡住，
活寄存器 `exccause=1c(LoadProhibited)`、`epc1=42018932`、
`a0=0x3fcb9d89`、窗口状态仍然是 `wb=9 / ws=0x200`。
`a0=0x3fcb9d89` 是 `__c99_d2` 段里的地址（`os_core_ready` 附近），被当成
指针解引用 ⇒ 说明**某个函数的 a0（返回地址）被数据值顶掉了**，这类现场
仍然指向"寄存器窗口内容与 WINDOWBASE 不匹配"。

## §376 中断帧**不再记成任务保存点**（默认行为改掉，`-DS3_ISR_SAVE_CTX` 才开）

`port_irq_fill()` 以前把中断帧记进 `c->sp`。但中断帧和 `port_task_switch`
的帧**语义不同**：

| 字段 | 中断帧（`_vec_level1`） | 任务帧（`port_task_switch`） |
|---|---|---|
| 偏移 64 (PS) | 被打断者的 PS **去掉 INTLEVEL** | 任务的 PS（含 CALLINC） |
| 偏移 68/72 | 被打断那一刻的 WS/WB | 任务自己保存的 WS/WB |
| 帧长度 | **128** | **80** |

混用会让任务恢复后的窗口基址与寄存器内容不配套。现在默认不记，
只有 `-DS3_ISR_SAVE_CTX`（将来做抢占时）才记。

## §377 剩下的坎：**库的 windowed 帧被切走时 a0 还带窗口标签**

排除上面所有错误之后，现场稳定成这一副：

```
trap 记录（0x50000140，第一次异常，没有被打印路径污染）：
  cause=0x14 (InstrFetchProhibited)   a0=0x8204df12  a1=3fcbebc0  PS=00060c00
  → 0x8204df12 & 0x3FFFFFFF = 0x4204df12，落在 **bb_init+0xb6**（闭源 PHY 库）
实时：exccause=1c(LoadProhibited) epc1=42018932 windowbase=9 windowstart=0x200
      a0=3fcb9d89  a1=0x0008437f
```

解读：`a0` 的高 2 位是 **CALLINC=2**（call8 的窗口标签），说明这是**库的
windowed 函数还没 retw** 时被切走/被中断的现场。`rfe` 回到 `EPC` 时把
`0x82xxxxxx` 这种带标签的地址直接写进 PC ⇒ **InstrFetchProhibited**。

我们已经有的防护是 `port_task_switch` 里 `slli/srli` 清高 2 位，但它只覆盖
"**我们自己发起**的切换"。**中断路径（`_vec_level1`）存进帧+60 的 EPC 是
`rsr epc1`**，那本来就不带标签 —— 所以问题不在那里。

真正缺的是：**被中断/被切走的库函数，其 a0 的窗口标签必须原样保留到它自己
`retw` 那一刻**，而这依赖 **WINDOWBASE 在切换前后完全一致**。现在
`windowbase=9`（奇数、非法）说明**有人的 rotw 步进没归位**。
下一步就从这里查：谁在把 WINDOWBASE 弄成 9（`s3_win_spill` 的
`call4/8/12` 嵌套、`_win_ovf*` 的 `rfwo`、还是库自己的 `entry`）。

## §378 **重大更正**：`WINDOWBASE=9 / WINDOWSTART=0x200` 是**合法的**，不是 bug

我在一个**已知良好**的状态下（`s3_win_wifi_main` 断点处）读到：

```
pc=0x40379ea8  windowbase=6  windowstart=0x40  ps=0x00060010
```

`WS == 1<<WB`，且 `WB=6` 也不是 4 的倍数。⇒ ESP32-S3 上 WINDOWBASE 就是
按"窗口序号"编码的（call12 步进 3 个单位），**6 和 9 都是合法值**。
之前十几轮里"WINDOWBASE=9 非法 ⇒ 寄存器映射全错"的推论**是错的**，
围绕它的那些实验（§344 的 WB 探针、§357 的按任务 WB 恢复…）都建立在
错误前提上。

## §379 这一轮真正修掉的三处"帧压溢出槽"（都是真 bug）

windowed ABI 把 **`[sp-48, sp-4]`** 当作"被调用者把 caller 的 a0..a11
溢出到栈上"的槽位区（call4 用 -16..-4，call8 用 -32..-4，call12 用 -48..-4）。

| 位置 | 原来 | 现在 | 后果 |
|---|---|---|---|
| `_vec_level1` 中断帧 | `-80`（占 `[sp-80,sp)`） | `-128`（占 `[sp-128,sp-48)`） | §361 已修；**这一条是对的** |
| `port_task_switch` 帧 | `-80` ← **盖住整个槽位区** | `-128` | 我们存的 a0..a15 与硬件溢出槽互相覆盖 |
| `port_task_yield` 帧 | `-80` ← 同上 | `-128` | 同上 |
| `port_ctx_create` 初始帧 | `-80` | `-128` | 同上 |
| `W2C` 壳的 spill 死区 | `-128` | **`-768`** | `s3_win_spill` 的级联调用（call8 + 4×call12 + 若干 entry）在壳 sp 下方压约 **480 字节**溢出槽，原来只降 128 就 `callx0` 进内核 ⇒ **内核帧直接盖掉刚 spill 出来的祖先窗口** |

⚠️ 顺手记一个坑：`addi` 的立即数上限是 **127**，写 `addi a1,a1,128` 汇编直接
报错，而 `make` 的错误被 grep 过滤后看起来像"构建成功"，于是有几轮实际在跑
旧镜像。回退 -768 必须用 `movi a9,768; add a1,a1,a9`。

## §380 仍然卡在同一处，且**不是**窗口/栈帧问题

修完上面全部五处之后，串口依然停在 `[give] t=2 sem#1` + `b`。活寄存器：

```
pc=0x40379ea8 (自旋)   exccause=1c(LoadProhibited)   epc1=os_task_start_core
a0=0x3fcb9d89(_data_start)   a1=0x000845bf  ← **恰好等于 flash text 段大小**
windowbase=9  windowstart=0x200
```

`a1` 等于一个**字面量池里的常量**（`flash_diag` 打印过的 text size）⇒ 某个
"a1"读到的物理寄存器里装的是我们的字面量。

已加两道闸门来定位，**都没有触发**：
- `RESTORE_FRAME` 的 EPC 闸门（0x50001880）
- `RESTORE_FRAME` 的 sp 闸门（0x500018E0）

⇒ 说明崩溃**不在恢复路径里**，而是恢复之后、某段代码自己把 a1 弄丢了。

真实结构（本轮新认识）：`cause=1c` 发生在 `os_task_start_core` 里
`a1` 被用作基址的那条 `l32i` 上；而 `default` 记录里的 `a0`/`a1` 是
"调度器主循环的帧指针已经不对"。下一步应该直接在
`port_task_yield` 返回点（`os_task_block_wait` 里 `call0 port_task_yield`
的下一句）下硬件断点，看**返回时 a1 是不是还等于调用前的值**。

## §381 §379 之后：**不再是我们自己的崩，而是闭源 PHY 库崩**

修掉五处"帧压溢出槽"之后，现场变成了完全不同的一类：

```
trap（0x50000140，第一次异常，未被打印路径污染）：
  cause=0x1c LoadProhibited   vaddr=0x000001a8
  epc1=0x42055659  →  pbus_rx_dco_cal_1step   （闭源 PHY 库！）
  a0=0x82055bbc    →  0x42055bbc = set_rx_gain_cal_dc 的调用点，**带 CALLINC 标签**
  ps=0x00060010    frame=3fcbe880
dbg2（0x50001800）：最后一次切换**完全干净**
  switch_out_sp=3fcd8e90 → target_ctx=3fcc2e40 → restored_sp=3fcbedc0
```

解读：`vaddr=0x1a8` 是"拿一个小整数当指针解引用"——PHY 的一次增益校准
（`set_rx_gain_cal_dc` → `pbus_rx_dco_cal_1step`）里某个参数/指针变成了
`0x1a8`。这是**库内部状态被破坏**，不是我们栈帧错位。

结合 §367/§369 已修的"W2C 壳没保存 a12..a15"，现在剩下的最可能嫌疑是：
**库的 windowed 调用链在被切走时，`a4..a7`（callee-saved）在某个桥里仍会被
脏掉**，而 PHY 校准链正是长时间跑、频繁跨桥的那段。

下一步（下一轮）：
1. 给 `abi_thunk.S` 和 `win_bridge.S` 的每条桥加"a4..a7 往返一致性"断言
   （进桥时把 a4..a7 存一份到 RTC，出桥前比较），一次上板就能定位是哪条桥。
2. 或者在 PHY 校准前后各做一次"全寄存器指纹"对比（`s3_win_stress` 的
   phase2 已经验证过纯自己的深调用没问题）。

## §382 阶段 3 被精确隔离（`tests/t_winabi.c6` 真板结果）

把原来的阶段 3 拆成三个隔离项（标记在 0x50000A40 的 d[10]/d[11]/d[12]）：

| 子项 | 形态 | 结果 |
|---|---|---|
| **3a** | 深递归**做完**、栈已弹空 → 再 `delay`（切换） | **通过** `d[10]=0x101` |
| **3b** | 先 `delay` → 再做深递归 | **通过** `d[11]=0x201` |
| **3c** | 递归**里**调 `delay`（`s3_win_stress`） | **dep=1 就卡死** `d[12]=1` |

⇒ 只要"切换那一刻寄存器窗口里还有祖先帧活着"，就一定坏。

## §383 **找到根因：`g_s3_win_spill_en` 在隔离测试里一直是 0**

`g_s3_win_spill_en` 只在 `s3_wifi_osi_init()` 里被置 1（`wifi_glue.c:1240`），
而 `tests/t_winabi.c6` 这条路径**根本不调用它** ⇒ 一直是初值 0
⇒ `W2C` 壳里的 `WIN_SPILL` **被整个跳过** ⇒ 切换时祖先窗口没有被存到栈上。

这与 §382 完全吻合：3a/3b 切换时窗口要么已弹空、要么还没压深，所以"不 spill
也没事"；3c 切换时窗口正压着，不 spill 就永久丢失。

已在 `s3_win_stress_task()` 开头显式 `g_s3_win_spill_en = 1u`。
（重跑后 3c 仍然在 dep=1 卡住 —— 说明除了这个开关，`WIN_SPILL` 本身或
它和 `port_task_switch` 的配合还有问题，下一步继续查。）

## §384 一次失败的插桩（记下来避免重犯）

想在 `WIN_SPILL` 宏里记"spill 前后的 WINDOWSTART/WINDOWBASE"，
于是内联写了 `.align 4` + `4: .word 0x50001860` + `l32r a8, 4b`。
**这个宏是内联展开在指令流中间的**（不像 `W2C` 的 `9:` 在函数标签之前），
插进去的数据字会被 CPU 顺序执行到 ⇒ 真板 `TRAP pc=s3_w2c_queue_init+..`、
`a0=0x8037c6c1`（带窗口标签）。
**规则**：`WIN_SPILL`（以及任何内联宏）里只能放指令，字面量必须放到
函数体外或用 `movi`（≤ 16 位）。

## §385 **真 bug（比 §367 更致命）：W2C 壳没有保存 a8..a11**

`call8` 进壳时窗口轮转 8 个寄存器：

```
壳的 a0..a7   = 调用者的 a8..a15
壳的 a8..a15  = 调用者的 a0..a7
```

⇒ **壳的 a8/a9/a10/a11 就是调用者的 a0（返回地址）/a1（栈指针）/a2/a3**。
而壳里到处拿 a8/a9 当临时（`rsr a8, ps`、`l32r a8, 9b`、`l32i a9, a8, 0`），
`callx0` 进 call0 内核更会把 a8..a11 全冲掉 ⇒ **库拿回的"自己的返回地址"
是个字面量地址**。

真板证据（修之前）：
```
TRAP pc=42013c4d(s3_w2c_queue_init) a0=8037c6c1 callinc=2
  → 0x4037c6c1 = osi_wifi_create_queue，带 call8 的 CALLINC=2 标签
```
修完（`entry a1,160` + 开头就 `s32i a8..a11, a1, 40/44/48/52` + `retw` 前
最后一步还原）之后：**`u1u2` 主循环正常跑起来了**，`d[4]=0xC0DE5678`、
`d[6]=74017` 都对。

`d[5]`/`d[7]` 仍然是垃圾（那是 delay 计数与 stress 累加），阶段 3c 仍然
在 dep=1 卡住 —— 但**第一个真正的"壳把库的返回地址毁掉"的 bug 已经解决**。

## §386 `wsr windowstart` 之后不能再直接用 a1（但也别保护过度）

从 `port_task_yield` 的现场推出来：`wsr windowstart` 同样会重映射寄存器文件
（含 `a1`），而代码里只有"WS==0 归一化"那条分支用 `.Ltmpsp` 藏了帧指针，
WS 非 0 的正常分支写完就直接用 a1 读帧。真板现场：
`vaddr=0x000603f4  a1=0x00060400  epc1=×`（a1 已经换成别的寄存器内容）。

**但把保护扩到整段是错的**：实测连 `T1` 都打不出来（卡在调度器启动后）。
原因是我们借用的 `.Ltmpsp`（`g_ctx_tmp_sp`）是**全局单槽**，恢复过程里
被多任务/中断共用；一旦把正常分支也塞进去，两条路径会互相覆盖。
⇒ 保持原样（只有归一化分支用全局），这条记为**已知残留风险**，
真要修得给每个任务一份 tmp 槽，不是本轮能顺手做的。

## §387/§388 两次"恢复时写 WINDOWBASE"都失败 —— 结论：这条路走不通

假设是：`wsr windowstart` 不更新 WINDOWBASE，而下面 `l32i a0..a15` 用的是虚拟
寄存器，映射由 WB 决定，所以必须在装寄存器**之前**先写 WB。

两次实现都被真板否掉：
1. 把 `wsr windowstart` 也塞进 `.Ltmpsp` 保护区 → **连 `T1` 都不打**；
2. 先 `wsr windowbase,<帧+72>` 再写 WS → **`c6os s3 boot` 之后直接停**。

原因：`g_ctx_tmp_sp` 是**全局单槽**，恢复流程里被多任务/中断共用；而且
`wsr windowbase` 会立刻重映射 a1，任何"用全局藏帧指针"的写法在这条路径上
都和别的路径互相覆盖。要真正做成，得给每个任务一份 tmp 槽（放在 ctx 结构里），
那是结构性改动，不在本轮。

⇒ **恢复路径维持现状**（只写 WS，让 `rfe` 用 PS.OWB 设 WB）。
这条记为**已知残留风险**，也是目前所有"恢复后寄存器错位"类现场的最可能来源。

## §389 EXCSAVE3 当可重入 tmp 槽 —— 思路对，但实测仍然起不来

用特殊寄存器 `wsr a1, excsave3` 藏帧指针（不受窗口重映射影响、天然可重入），
绕开全局单槽 `g_ctx_tmp_sp` 的不可重入问题。仍然卡在 `c6os s3 boot` 之后。
⇒ `EXCSAVE3` 在这颗芯片上要么被别处占用，要么 `wsr windowbase` 之后
`rsr a1, excsave3` 的行为和预期不同。**已回滚**。

恢复路径的"先写 WB 再装寄存器"这条路，本轮试了三种写法（全局槽、扩大保护区、
EXCSAVE3）全部失败。**结论：在当前结构下不要动这一步**。
要真正解决需要结构性改造（每任务一份 tmp 槽 + 彻底重写 RESTORE_FRAME 的
窗口处理顺序），这是下一阶段的工作量，本轮到此为止。

## §390 补齐 W2C 壳的 `a4..a7`（= 调用者的 a12..a15）

到 §385 为止，默认壳存了 a8..a15（调用者的 a0..a7），但**漏了 a4..a7**
—— 在 `call8` 下它们正是**调用者的 a12..a15**，而 a12..a15 在 windowed ABI
里是 callee-saved（GCC 用它放长期存活的值）。壳里 `callx0` 进 call0 内核后
这 4 个寄存器会被随便改。

已在 `entry` 之后、任何指令改变它们之前存到 `[a1+96..108]`，`retw` 前还原。
现在壳对调用者**完全透明**：a0..a15 全部原样保留。
（同一套理由适用于 `W2C_TRACE` 和 `osi_queue_recv` —— 它们也补齐了。）

真板结果：行为与 §385 之后一致（到 `[give] t=2 sem#1` + `b`），没有回退，
但也没有新进展 ⇒ **切换路径本身已经不是瓶颈了**，剩下的坎在恢复时的
窗口状态（§387-§389 三次尝试都失败的 `wsr windowbase` 问题）。

## §391 "两个分支都写 WINDOWBASE" —— 再次失败（第四次）

把 §387/§388/§389 的教训吸收后重写：两个分支都写 WB，且 `g_ctx_tmp_sp`
只在同一段关中断代码里**存取各一次**（不像 §387 那样扩到正常分支中途重写）。
真板：`u1u2` 能跑，但压力测试从"3a/3b 通过、3c 卡"退化成 **3a 就卡**
（`d[10]=1`、`d[11]=d[12]=0`）。**已回滚。**

四次尝试（全局槽扩保护 / EXCSAVE3 / 先写 WB 再装寄存器 / 两分支都写 WB）全部
失败 ⇒ 恢复路径里"自行写 WINDOWBASE"这条路在当前结构下不可行。
`rfe` 用 PS.OWB 设 WB 是唯一能工作的方式，而**装寄存器时用的是哪个窗口**
这个理论问题至今没有实证解释，但改成任何变体都只会更差。

**当前保持的最佳状态（HEAD）**：`[give] t=2 sem#1` → `b`，
压力测试 3a/3b 通过、3c 卡在 dep=1。

## §392 3c 的精确形态：**切换发生在递归链中间**

把 3c 再拆一层（新标记 `d[13]`）：

```
d[13] = 0x5xx → 先深递归（deep12(8) → 74）→ **递归完全返回后** delay → 再深递归
d[13] = 0x6xx → 上面两步都通过了
d[12] = dep   → 然后跑真正的 `s3_win_stress(dep, dep)`：**在递归链中间** delay
```

真板：`d[13]=0x601`（通过）、`d[12]=1`（卡在 dep=1）。

⇒ 唯一的差别就是"delay 发生在**递归还没返回**的时候"。
此时：

- 寄存器文件里有多层**活着的**祖先窗口；
- `W2C` 壳会先 `WIN_SPILL` 把它们全压到栈上（这一步 §383 已确认开关是开的）；
- 然后切走；回来时硬件从栈上**下溢**恢复那些窗口。

失败时 `a1=0`，说明恢复出来的 sp 是 0 —— 即**某个帧的偏移 76 是 0**
（§392 的零槽闸门没触发，说明它读的是有效帧，只是那个槽确实是 0）。

**下一步**：重点查 `s3_win_spill` 在"递归链中间"这个特定时刻的行为 ——
它把窗口压到了**谁的栈上**？如果压到的是**递归帧的溢出槽**（而不是壳自己的
死区），那么回来时读到的就是递归函数的局部量。

## §393 死区 768 → 512（栈预算）

`s3_win_spill` 实测栈开销约 480 字节，加壳自己 512 后仍在任务栈预算内。
行为无变化（3a/3b 仍过，3c 仍卡），保留 512 以省栈。

## §394 3c 的**唯一**失败条件（一次上板就定死了）

把 3c 再拆一层（新标记 `d[13]`），真板结果：

| 子项 | 形态 | 结果 |
|---|---|---|
| 3c-1 | 深递归 → **递归完全返回后** delay → 再深递归 | **通过** `d[13]=0x601` |
| 3c-2 | `s3_win_stress(dep,dep)`：**在递归链中间** delay | **卡在 dep=1** `d[12]=1` |

⇒ 唯一差别：**delay（切换）发生在递归还没返回的时候**，也就是
"寄存器文件里有多层活着的祖先窗口"的那一刻。

## §395 插桩又踩了同一个坑（第二次）

想在 `WIN_SPILL` 里记 sp，先试 `.word` 字面量（§384 的老坑，`l32r` 越界）、
再试 `movi 0x1840`（超 12 位立即数范围）。**结论：`WIN_SPILL` 宏内部绝对
不能放任何需要字面量池的东西**，只能放纯指令。要诊断就把值写进帧自己的
某个偏移，由调用方去读。

## §396 `s3_win_spill` 的真实栈耗：**784 字节**（实测反汇编）

`42014ca4 <s3_win_spill>`：

```
entry a1, 48
bbci a0,31 → call4 链（只要 112 字节）
bbsi a0,30 → call12 链（最重）
中间     → call8 链
```

最坏是 **4 层 call12**，每层 `entry a1,48` + 窗口溢出槽 `[sp-48,sp-4]`
= 96 字节，4 层 384，再加 entry 与槽的 32 ⇒ **约 784 字节**。

之前 W2C 壳的死区是 128（§378 改到 768，§393 又降到 512）—— **512 在
"栈上还有递归帧"时会把栈压穿**，正好是 §394 定死的 3c 唯一形态。

已改成 **896**（784 + 112 余量）。真板结果：**仍然卡在 3c dep=1**
⇒ 栈预算不是 3c 的根因（但 896 是正确的、按实测算出来的值，保留）。

## §397 本轮结论

按 §394 的精确条件逐个排除：
- 不是 WS 存的值（§396 试了存窄化后的 `1<<WB`，无变化）；
- 不是死区太小（512 → 896 无变化）；
- 不是切换本身（3a/3b 一直过，`d[13]=0x601` 也过）。

剩下唯一未验证的假设：**`s3_win_spill` 在"递归链中间"会把窗口压到
递归帧自己的溢出槽里**（而不是壳的死区），回来时读到的是递归函数的局部量。
验证需要读 spill 前后的 sp 对比 —— 但 `WIN_SPILL` 宏里**不能放字面量**
（§384/§395 两次踩坑），只能把值写进帧的固定偏移再由调用方读回。

## §398 896 死区在 WiFi 主构建上引出新的 PHY 崩溃

把死区从 512 提到 896 之后，**隔离用例 `t_winabi` 行为不变**，但 **WiFi 主构建
倒退了**：

```
W0 phy start → [phy] calling register_chipv7_phy(mode=0)
*** TRAP sp=3fcbea60 cause=0000001c pc=42058a82 ps=00060a10
         vaddr=0000000c callinc=00000002 a0=82053cd5
  0x42058a82 = get_tx_gain_value
  0x42053cd5 = ram_wifi_get_tx_gain   (a0 带 call8 标签 → 又是"返回地址被毁"型)
```

比之前的 `pbus_rx_dco_cal_1step`（§381）**更早**失败 ⇒ 896 的死区把 PHY 的
栈压穿了（PHY 调用链本身很深）。**回退到 512。**

教训：死区**不是越大越好** —— 它吃掉的是**调用者任务**的栈。784 是
`s3_win_spill` 自己的需求，但壳执行 spill 时调用者栈上还有它自己的活帧，
两边加起来才是总预算。512 是"够 spill 用又不压穿 PHY"的折中值。

## §399 排除：`wsr ps`（OWB）**不会**重映射寄存器文件

怀疑点是 `RESTORE_FRAME` 里 `wsr a2, ps` 那句 —— 帧里的 PS 带 OWB(bits 8-11)，
而 OWB 就是 WINDOWBASE，理论上这一句之后所有虚拟寄存器（含 a1）会被重映射，
但紧接着就是 `l32i a0, a1, 0` … 如果 a1 被换掉，恢复出来的全是垃圾。

**实测（诊断写 0x500018A0，`wsr ps` 前后各记一次）：**

```
之前: a1=3fcbedc0  wb=0  ws=00000001  ps=0006080f
之后: a1=3fcbedc0  wb=0  ws=00000001  ps=00060010
```

`a1` **没有改变**。原因是硬件在 `wsr ps` 时不会立即改 WINDOWBASE，只更新
PS 里的 OWB 字段；真正的 WB 切换发生在 `rfe` 上。⇒ 恢复顺序本身是对的。

这条排除掉之后，"恢复后 a1 变成垃圾"就只剩一个来源：
**切换那一刻存进帧里的 sp 本身就已经不对了**（即 `port_task_switch`
拿到的是别的东西），或者任务栈真被压穿了。

## §400/§401 OWB 与 WINDOWSTART 不配套 —— 假设对，但改这两处**无效**

切换环（0x50000500）里反复出现：`WS=0x00000200`（WB=9）而 `PS=0x00060800`
（OWB=8），差 1。按 Xtensa 约定 `WS = 1 << (OWB+1)` 应该配套，而 `rfe`
用 OWB 装 WINDOWBASE、WS 是单独写的 ⇒ 理论上恢复后整体错位一格。

两次修法都**无效**（行为完全一样）：
1. §400 在**恢复时**按帧里的 WS 重算 OWB 再 `wsr ps`；
2. §401 在**保存时**就把 OWB 对齐到 WS。

无效的原因已经查明：**切换环里那个 PS 是 `rsr ps` 的原值，不代表恢复时用的值**
（恢复用的是帧偏移 64，而这两处我都改到了）。真正卡住的 3c 不是这条路径。

**另一个重要的反向证据**：IDF 自己的 `xtensa_vectors.S` **从不主动写
`WINDOWBASE`**，全靠 `rfe`。我们 §387-§391 五次"自己写 WB"的尝试全部失败，
与这一点一致。⇒ **不要碰 WINDOWBASE，维持 rfe 的默认行为。**

## §402 决定性证据：`WINDOWBASE=9` 而 `WINDOWSTART` 的 bit 9 **没置位**

3c 卡死时的硬件寄存器实读：

```
windowbase  = 9
windowstart = 0x200        ← bit 9 没置位
a1          = 0x00084c1f   (垃圾)
epc1        = 0x420191d2   (os_task_start_core 里 l32i a10, a1, 24)
```

Xtensa 的**硬性不变式**是 `(WINDOWSTART >> WINDOWBASE) & 1 == 1`
（当前窗口必须 live）。不满足时寄存器映射非法 —— 这就是所有
"恢复后 a1 变成垃圾/0" 的统一解释。

**但补那一位没用**：`wsr` 之后 `windowstart` 确实变成 `0x251`（含 bit 9），
3c 仍然卡在同一个地方、`a1` 仍然为 0。

⇒ 错位发生在**更早**：`rfe` 按 PS.OWB 设 WINDOWBASE 那一刻，
`a0..a15` 已经用**旧的** WB 映射装好了。补 WS 只是把硬件状态弄合法，
补救不了已经装错的寄存器。

**结论**：必须在**装 a0..a15 之前**让 WB 变成最终值。而 §387-§391 五次
"自己写 WINDOWBASE"的尝试都因为 `wsr windowbase` 把 a1 一起重映射而失败
（全局 tmp 槽不可重入）。**唯一没试过的办法**：把整个恢复序列做成
"先算好帧指针的物理位置、用绝对地址存取"，或者干脆**让每个任务只有一个
固定窗口基址**（启动时一次性设定，切任务时不改 WB）。

## §403 "装完寄存器后再对齐 WB" —— 更糟，已回滚

思路：既然 §402 查明错位发生在 `rfe` 用 PS.OWB 设 WB 那一刻，那就等
a0..a15 **全部装好、a1 也用完了**，再写 `wsr windowbase` + 更新 PS.OWB。
理论上此时打乱 a1 无所谓。

实测：**压力标记全变成垃圾**（`d[9]=195720816` 等），`a1=0x00060010`
（正是 PS 的低位），`pc` 落在 `_vec_default`。

原因：`wsr ps` 本身会改 PS 的低位（INTLEVEL 等），而且在 `rfe` 之前
再 `wsr ps` 会让 PS 与 EPC/WS 的组合重新错位；更糟的是 `wsr windowbase`
之后**紧接着**几条指令自己就在被重映射的窗口里跑。

⇒ **确认：在 RESTORE_FRAME 里任何位置写 WINDOWBASE 都会失败**，
六次尝试（§387/§388/§389/§391/§400/§403）覆盖了"开头、中间、结尾"和
"全局槽、EXCSAVE、帧内槽"所有组合。

**§402 的不变式违反是既存事实，但它是 `rfe` 之前那一刻的瞬时状态**，
`rfe` 自己会把它修正 —— 我们观测到的 `wb=9/ws=0x200` 很可能是
**陷阱处理体自己的状态**，不是被恢复任务的。下一步要从这个角度重新看。

## §404 ★★★ 决定性修复：**删除所有主动写 WINDOWSTART / WINDOWBASE**

### 怎么找到的
这一轮不再试自己的写法，直接读 IDF 的权威实现
（`components/xtensa/xtensa_context.S`、`xtensa_vectors.S`）：

```asm
_xt_context_restore:            # 恢复 a2..a15 / SAR / LCOUNT ...
    ...                         # **一行都不写 WINDOWSTART / WINDOWBASE**

_xt_user_exit:
    l32i a0, sp, XT_STK_PS   ; wsr a0, PS
    l32i a0, sp, XT_STK_PC   ; wsr a0, EPC_1
    l32i a0, sp, XT_STK_A0
    l32i sp, sp, XT_STK_A1   ; rsync ; rfe
```

**窗口状态的恢复完全由 `rfe` 按 PS.OWB 完成。** 我们却额外做了
`wsr windowstart`（§296/§311 引入）和"新建任务归一化"的 `wsr windowbase`
（§345 引入）—— 正是这两处制造了 §402 实测到的非法组合
（`windowbase=9` 而 `windowstart=0x200`，bit 9 未置位）。

### 怎么改的
`RESTORE_FRAME` 里把 `wsr windowstart` / `wsr windowbase` 整块删掉，
只保留 `rsil a5, 15`、恢复记录环、EPC/PS 写入、a0..a15 装载、`rfe`。
新建任务的初始 PS 里 OWB 已经是 0（ctx.c 已清），`rfe` 会自己把 WB 设成 0。

### 真板结果（隔离用例 `t_winabi`）

| 标记 | 修复前 | **修复后** |
|---|---|---|
| `d[10]` 3a | `0x101` 卡住 | **`0x105` 完成** |
| `d[11]` 3b | `0x201` | **`0x205` 完成** |
| `d[12]` 3c | `1` **卡住** | **`0x305` 完成** |
| `d[13]` 3c 子项 | `0x601` | **`0x605` 完成** |
| `d[9]` 循环轮次 | — | **1029 轮** |
| 卡住时 `a1` | `0x00084c1f` 垃圾 | **`0x3fcd8da0` 合法栈地址** |

**3c（在递归链中间做任务切换）从"永远卡死"变成"1029 轮稳定通过"。**
这同时解释了 §387-§391/§400/§403 六次"自己写 WINDOWBASE"为什么全失败 ——
这条路本来就不该走，正确做法是**什么都不写，交给 `rfe`**。

## §405 把 W2C 壳的 `wsr ps` 挪到 `retw` 前 —— 引入新崩溃，已回滚

理由：`wsr ps` 会把 PS.OWB 写进去，OWB 就是 WINDOWBASE，而它后面**还要用
a1 取 8 个寄存器**，窗口映射变了就会取错窗口。按 IDF `_xt_user_exit`
"写 PS 之后立刻 rfe、中间不碰 a 寄存器"的风格，应该挪到最后。

实测：**反而更早崩** —— `[pp] pp rom version` 之后
`TRAP cause=3 pc=4037c9ff(osi_calloc) vaddr=40000000 a0=8037cb08`
（`a0` 又是带 CALLINC 标签的地址）。回滚后恢复到 `[give] t=2 sem#1`。

**注意**：这说明"恢复 PS 的时机"确实敏感，但**不是这条链上的主要问题**。
壳里的 `wsr ps` 保存的是**进入壳之前**的 PS（带 CALLINC=2），
`retw` 本身要用这个 CALLINC 决定回退几个窗口 —— 挪到最后会破坏这个搭配。

## §406 §404 复现确认（第二次上板）

隔离用例 `t_winabi` 重新构建上板，标记完全一致：

```
d[6]  = 74017   (阶段 1/2 一直通过)
d[9]  = 1029    (压力循环跑了 1029 轮)
d[10] = 0x105  3a = 5 ✓
d[11] = 0x205  3b = 5 ✓
d[12] = 0x305  3c = 5 ✓
d[13] = 0x605  全部子项 = 5 ✓
```

**§404 是本轮唯一的实质性突破**：删掉 `RESTORE_FRAME` 里主动写
WINDOWSTART/WINDOWBASE 的两段之后，"在递归链中间做任务切换"（3c）
从**永久卡死**变成 **1029 轮稳定通过**。

## §407 当前卡点（本轮结束位置）

WiFi 主构建在 §404 之后：

```
W0 phy start → register_chipv7_phy rc=1 ✓  (之前 §398 的 PHY 崩溃已消失)
W1 bringup → B0/B1/B2 ✓ → [lock]/[lockok] ✓ → [pp]/[net80211] ✓
→ [unlock] ✓ → [mtxnew]#1 #2 ✓ → [semnew]#1 ✓
→ [task] 库建任务 → [give] t=2 sem#1 → 停
```

停在 `s3_wifi_bringup`（stage=0x1005）里、闭源库建完第一个任务之后。
`a1` 在被 `s3_win_call_wifi_main` 调用返回后变成 0 —— 这是 W2C 壳的
返回路径问题（§405 试过挪 `wsr ps`，引入新崩溃后回滚）。

**下一步**：查 W2C 壳 `retw` 之前 `[a1+4]`（硬件存的调用者 sp）是否被
`WIN_SPILL` 或 512 死区写过。`entry a1,160` 的帧是 `[a1, a1+160)`，
死区是 `[a1-512, a1)`，理论上不重叠 —— 但 `s3_win_spill` 的窗口溢出槽
落在"被 spill 窗口的**调用者**的 sp 下方"，那个 sp 可能就在壳的帧里。

## §408 诊断：最后一次恢复用的帧里 `WS=0` —— 新任务帧没有被归一化

在 `RESTORE_FRAME` 读 `a1sl` 之前把 (帧, a1sl, EPC, PS, WS, WB) 记到
`0x50001820`。真板读数：

```
最后一次恢复: 帧=3fcbedc0 a1sl=3fcbee40 EPC=403798cb
              PS=00060000  WS=00000000  WB=00000000
当前硬件:      a1=0xffffffe0  wb=15  ws=0x8000
```

`a1sl` 合法、PS/WS/WB 全 0 ⇒ 这是**新建任务的帧**（ctx.c 只清了 PS.OWB，
没写 WS）。§404 把"新建任务归一化"一起删掉了，而 `rfe` 只按 PS.OWB 设 WB，
**不会清掉 WS 里残留的 `0x8000`**（别的任务留下的，base=15）。

于是 WB=0 而 WS 仍含 bit15 ⇒ 寄存器映射错位 ⇒ `a1` 从 0 变成 `0xffffffe0`
（入口 `addi a1,a1,-64` 之后 = -32）。

## §409 ★ 修复：**只对 WS==0 的新任务帧**做窗口归一化

```asm
    l32i    a3, a1, 68
    bnez    a3, 11f            ; 已有任务帧（WS 有效）→ 完全不碰（§404 结论）
    ... wsr windowbase 0 / windowstart 1，用 g_ctx_tmp_sp 护 a1 ...
11:
```

已有任务的帧仍然**一行都不写窗口状态**（§404 的决定性修复保持不变），
只有新建任务这一种情况补上"干净起点 WS=1、WB=0"，与 ctx.c 里已清 0 的
PS.OWB 三者一致。

### 真板结果

| | §404 单独 | **§404 + §409** |
|---|---|---|
| 隔离用例 3a/3b/3c | 5/5/5 ✓ | **5/5/5 ✓（1029 轮）** |
| WiFi PHY | `rc=1` ✓ | **`rc=1` ✓** |
| WiFi bringup | `[give] t=2 sem#1` | **`[give] t=2 sem#1`** |
| 新任务 a1 | `0xffffffe0`（坏） | — |

两项修复互补：§404 让"递归链中间切换"不再永久卡死（3c → 1029 轮通过），
§409 让"新建任务第一次被调度"时的窗口状态干净。

## §410 当前卡点（本轮结束）

WiFi 主构建（`§404 + §409`）走到：

```
M1..M5 → T1 call0 任务进入 windowed 主流程 → [wifimain] id=1
→ W0 phy start → register_chipv7_phy rc=1 ✓
→ W1 bringup → B0/B1/B2 ✓ → [lock]/[lockok] ✓
→ [pp]/[net80211] ✓ → [unlock] ✓
→ [mtxnew] #1 #2 ✓ → [semnew] #1 ✓
→ [task] 库建任务 fn=40381a28 id=2 stack=6144 prio=29
→ [give] t=2 sem#1 → 停
```

**PHY 阶段已经完全打通**（这是之前多轮都过不去的地方），现在卡在
`s3_wifi_bringup` 里闭源库建完第一个任务之后。

切换计数 `switch=1 yield=1` —— 库任务建立后**只切了 1 次**就停住，
说明新任务第一次被调度时仍然没跑起来，或者它阻塞在一个没人给的对象上。

**下一步方向**（按价值排序）：
1. 读 `0x50000A00`（tramp 记录：启动次数/入口/起始 sp）确认库任务
   到底有没有进入过；
2. 读 `0x50001A00`（阻塞记录）看是谁在等什么；
3. 如果库任务确实进了但立刻阻塞，就是 OSI 的 sem/queue 语义还没对上
   （`g_dbg_sync` 打开看 `[take]`/`[give]` 配对）。

## §411 手动推进 WINDOWSTART 代替 `call8` —— 失败，已回滚

`s3_task_tramp` 里用 `callx8` 进 windowed 入口。`call8` 的语义不只是
"编码 CALLINC"，还会**把 WINDOWSTART 推进一格**；`callx8` 不做这一步。
于是被调用者拿到的"自己的 a1"还是 tramp 的 a1，第一条 `s32i a0, a1, 0`
就写到 `0xffffffe0`（真板实测）。

试着手动补这一步（`WS += 1<<WB` 再 `wsr`，用 `g_ctx_tmp_sp` 护 a1）：
**崩在 `ctx.S` 内部**（`a1=0`、`a0=1` = 我们的 `movi`），已回滚。

⇒ `wsr windowstart` 在 tramp 这个位置同样会毁掉 a1，
**和 §387-§391 在 RESTORE_FRAME 里遇到的是同一个问题**。

## §412/§413 ★ 修复：`callx8` 缺 `call8` 的窗口轮转 —— 加一个近处 windowed 跳板

### 症状
`s3_task_tramp` 用 `callx8` 进 windowed 入口，被调者拿到的"自己的 a1"
还是 tramp 的 a1，第一条 `s32i a0, a1, 0` 写到 `0xffffffe0`：

```
TRAP epc1=0x4037af8b (s3_win_call_wifi_main 第一条 s32i)
     a1=0xffffffe0  exccause=0x1d (StoreProhibited)
```

### 为什么
`call8` 与 `callx8` 的差别**不只是**"编码 CALLINC"——硬件 `call8` 会把
WINDOWSTART 推进一格，真正把 8 个寄存器让给被调用者；`callx8` 不做这一步。

### 怎么修
入口在 flash（`0x42xxxxxx`），`call8` 的 128KB 相对位移够不着。
所以 `call8` 到一个**同段内**的 windowed 跳板 `s3_win_tramp`，
由它 `callx8` 远端入口：

```asm
    .align  4
s3_win_tramp:
    entry   a1, 16               ; ← 必须有！见 §413
    s32i    a0, a1, 0
    mov     a8, a4               ; call8：壳的 a4 = tramp 的 a12 = 目标地址
    callx8  a8
    l32i    a0, a1, 0
    retw
```

`call8` 轮转规则（§385 已实测）：壳的 `a0..a7` = 调用者的 `a8..a15`，
壳的 `a8..a15` = 调用者的 `a0..a7`。

### §413 第一版忘了 `entry`
第一版是 `mov a8,a4 / callx8 a8 / retw`，结果
`TRAP pc=00000001 a0=00000000`。原因：`retw` 靠 PS.CALLINC 决定往回轮转
几个窗口，而被调者自己的 `retw` **已经把 CALLINC 归零**，跳板再 `retw`
就是轮转 0 个窗口、用当前窗口里的 0 当返回地址。**`entry` 才是建立
"被调者帧 + CALLINC 记账"的指令。** （另外 `entry` 必须 4 字节对齐，
否则汇编报 `unaligned entry instruction`。）

### 真板结果
WiFi 主构建：`[task] 库建任务 → [give] t=2 sem#1`，PHY 全程 `rc=1`。
隔离用例 `t_winabi`：`3a=5 3b=5 3c=5 sub=5`、`d[9]=1029` 轮，无回归。

## §414 本轮小结（三项真板验证过的代码修复）

| # | 修复 | 真板效果 |
|---|---|---|
| §404 | 删掉 `RESTORE_FRAME` 里所有主动写 WINDOWSTART/WINDOWBASE | **3c（递归链中间切换）从永久卡死 → 1029 轮稳定通过** |
| §409 | 只对 `WS==0` 的新任务帧做窗口归一化 | 新任务第一次被调度时窗口状态干净 |
| §412/§413 | tramp 用 `call8` 进近处 windowed 跳板（`callx8` 缺 `call8` 的窗口轮转） | windowed 入口的 `a1` 不再变成 `0xffffffe0` |

隔离用例 `t_winabi`：`3a=5 3b=5 3c=5 sub=5`、`d[9]=1029` 轮，三项修复叠加后无回归。

## §415 当前卡点

WiFi 主构建稳定走到：

```
M1..M5 → T1 → [wifimain] id=1 → W0 phy start
→ register_chipv7_phy rc=1 ✓   (PHY 完全打通，多轮未过的地方)
→ W1 bringup → B0/B1/B2 ✓ → [lock]/[lockok] ✓
→ [pp]/[net80211] ✓ → [unlock] ✓ → [mtxnew]#1#2 ✓ → [semnew]#1 ✓
→ [task] 库建任务 id=2 → [give] t=2 sem#1 → 停
```

**偶发**：有一次在 `stage=0x1003`（PHY 使能）崩在
`ram_write_pll_cap`（cause=0x1c），重跑即恢复。记为时序敏感，不作为主线索。

下一步：`tramp n=1` 说明库任务（`fn=40381a38`）**还没被启动过**。
查 `switch=1 yield=1` 那一次切换给了谁，以及 `[give]` 之后主任务阻塞在
哪个对象上（`0x50001A00` 阻塞记录）。

## §416/§417/§418/§419 `s3_win_call_wifi_main` 的窗口污染

### 症状（真板）
`epc1=0x40378763` = `wifi_call0_task` 里 `call0 s3_win_call_wifi_main`
的**下一条指令**，`a1=0`；中断进来后在 `RESTORE_FRAME` 的 sp 闸门
（`0x500018E0`）报 `0xBAD4` 并死循环（`pc=0x4037972c`）。

### 为什么
这个桥是 **call0** 的（`addi a1,a1,-32` 开帧、`ret` 返回），却要调
windowed 的 `s3_win_wifi_main`。原来的代码直接 `callx8`，
`call8`/`callx8` 会轮转寄存器窗口 ⇒ 返回后当前窗口里的 `a1`
已经**不是**我们那个帧基址了。

### 试过的
| # | 做法 | 结果 |
|---|---|---|
| §416 | 改成 `call8` 到近处 windowed 跳板（`entry` 开场 + `retw`） | 过了，但 `a0` 带 CALLINC 标签，`pc=0x10` |
| §417 | 修正跳板的寄存器映射（`a10`→壳 `a2`，返回值壳 `a10`→`a2`） | 回到 `[give] t=2 sem#1`，仍未过 |
| §418 | 帧基址存进全局 `g_win_bridge_sp`（a 寄存器全不可靠） | 干净地走到 `[give]`，`[mtxnew] #2` 不再混入 `3fcef000` |
| §419 | 跳板里 `callx8` 前存 PS、之后 `wsr ps` 恢复 CALLINC=2 | **退化**：卡在 `T1` 之后，连 PHY 都没出来，已回滚 |

### 认识
`call8`/`callx8` 配对的是 `retw`；`ret` 是 call0 的。
被调者自己的 `retw` 会把 PS.CALLINC **清零**，所以跳板的 `retw`
不会轮转窗口 —— 和 §412/§413 在 `ctx.S` 里踩的是同一个坑。
§412/§413 那边侥幸没事，只是因为跳板之后不再依赖窗口状态。

### 下一步方向（换路子，不再在 call0 里调 windowed）
**把 WiFi 主流程做成一个真正的 windowed 任务**，走已经用真板验证过的
`s3_task_tramp → call8 s3_win_tramp → callx8 入口` 那条路径
（§412/§413 已证明它能把库任务正确启动起来：
`tramp n=2 entry=40381a38`）。这样 call0 ↔ windowed 的转换点就只有
tramp 一个，而它是修好并验证过的。

## §420 ★★ 真 bug：新任务启动帧被自己的栈覆盖（`f[19]` 应为 `top`）

### 症状（真板）
`port_task_switch` 的 sp 闸门（`0x500018E0`）写 `0xBAD4` 并停在
`pc=0x403796f0`（`j .` 死循环）。dump `ctx[0].sp` 指向的帧，发现它
**根本不是切换帧**：

```
+00: 8037ac85 3fcbedf0 50000a10 00004100   ← 0x50000a10 是 WSTAGE 地址
+60: 00000001 803798c8 3fcbee30 00000000   ← EPC=1、WS=栈地址、a1sl=0
```

### 根因
`port_task_ctx_create` 里：

```c
top = ((u32)(g_stack_pool + g_pool_off) & ~15u) - 128u;  /* 帧 [top, top+128) */
f[19] = top + 128u;    /* ← 错！任务启动时 a1 = top+128 */
c->sp = top;
```

Xtensa 的栈**向低地址增长**，帧是 `[a1, a1+size)`。`a1 = top+128` ⇒
任务第一条 `entry a1,32` 就把帧写到 `[top+96, top+128)`——**盖在自己的
启动帧上**；栈再长一点就盖到偏移 76（`a1sl`）。于是 `RESTORE_FRAME`
读到 `a1sl = 0`，命中 `beqz a1, 83f` 分支。

### 修法
```c
f[19] = top;   /* 任务从 a1 = top 起跑、往下用栈 */
```
启动帧 `[top, top+128)` 落在**栈顶之上的保留区**里，永远不会被自己的栈覆盖。

### 真板结果（决定性进展）
- 之前：`[give] t=2 sem#1` 之后**永久卡死**在 `port_task_switch` 的死循环里。
- 之后：`esp_wifi_init_internal` **正常返回**！

```
[phy] register_chipv7_phy rc=0
W1 bringup → B0/B1/B2 ✓ → [lock]/[lockok] ✓ → [unlock] ✓
[v2]wifi|init: init is in progress
[wifi] esp_wifi_init_internal -> 12307
[wifi] bringup rc=12307
W2 bringup returned
```

留下的新问题：`esp_wifi_init_internal` 返回 `12307 (0x3013)`，
且库自己打了 `wifi|init: init is in progress` —— 需要查这个
"已经在初始化中" 是从哪来的（可能是重复 init，也可能是前一次调用
留下的状态没清）。

### 顺带确认
去掉 `-DS3_WIFI_TASK_CALL0`（让 WiFi 主流程跑在**真正的 windowed 任务**里，
走 §412/§413 修好的 `s3_task_tramp → call8 s3_win_tramp → callx8 入口`
路径）与 call0 模式在上面的症状上完全一致 ⇒ 说明这条路本身是对的、
问题一直在 ctx 层。

## §421 `esp_wifi_init_internal` 返回 0x3013 —— 库的 init 状态机被 PHY enable 污染

### 症状
```
[wifi] init 前的库状态 @3fcba3f0 = 2
[v2]wifi|init: init is in progress
[wifi] esp_wifi_init_internal -> 12307      (0x3013 = ESP_ERR_WIFI_INIT_STATE)
```

### 反汇编定位（`esp_wifi_init_internal`）
```asm
4203dc61: l32r a5, (3fcba3f0)     ; = g_wifi_osi_funcs_md5 + 4
4203dc66: l32i a3, a5, 0          ; a3 = 状态
4203dc69: beqi a3, 1, ...         ; 1 = 未初始化 → 继续 init ✓
...       bnei a3, 2, ...
          l32r a13, "init: init is in progress"
          l32r a2, 0x3013         ; ESP_ERR_WIFI_INIT_STATE
```
镜像里这个变量的 `.data` **初值是 1**（正确）。

### 是谁改成了 2
分阶段打点：
```
[wst] entry            = 1
[wst] after phy_cal    = 1
[wst] after phy_enable = 2      ← s3_win_phy_enable() 之后变成 2
```
我们手动做 PHY enable（`esp_phy_enable`），库的 wifi 状态机因此残留成
"正在初始化"，于是后面真正的 `esp_wifi_init_internal` 一进去就撞状态机。

### 实验性修法（有效）
调 `esp_wifi_init_internal` 之前把状态复位成 1：
```c
if (*wst != 1u) { *wst = 1u; }
```
（`wst = (volatile u32 *)(g_wifi_osi_funcs_md5 + 4)`，
**必须用符号 + 偏移，不能硬编码地址** —— 加一行打印就会让 `.data`
整体挪位，§421 第一次就是踩了这个坑，白读了一个代码指针。）

### 真板结果 —— init 真的跑起来了
```
[lock]/[lockok] ✓ → [pp] pp rom version ✓ → [net80211] net80211 rom version ✓
→ [unlock] ✓
→ [v2]wifi|create wifi task: queue not null
→ [semnew] #2 → [task] 库建任务
→ TRAP cause=0x1c(StoreProhibited) pc=0x4037d338(osi_queue_recv_body)
       vaddr=0x18  a0=0x80043d06
```
`pc` 处是 `l32i a9, a4, 24` ⇒ **`a4`（队列句柄）= 0**，
与库那句 `create wifi task: queue not null` 对得上：库认为队列已经存在、
于是没建，后面 recv 就拿到 NULL。

### 下一步
查 `osi_queue_recv_body` 的 `a4` 从哪来，以及 `[v2]...queue not null`
是库检查了哪个全局 —— 很可能是我们的 `osi_queue_create`/`osi_queue_recv`
实现或 OSI 表槽位填错，让库以为队列已存在。

## §422 排除假线索：镜像与 `.data` 加载都是正常的

曾怀疑 `esptool elf2image` 把 `.data` 拆成两段时错位了 8 字节
（`wst` 在 bin 里读到 `0x0F00BF9D`、在 ELF 里读到 `1`）。**这是我自己算错了
段内偏移**（把段头的 8 字节也多算了一次）：

```
seg1: vaddr 0x3fca0000 len 0xfec4  data@0x134
seg2: vaddr 0x42010000 len 0x84cc8 data@0x10000   (.data 被 64KB 边界切开)
seg3: vaddr 0x3fcafec4 len 0xb83c  data@0x94cd0   ← 数据从 0x94cd0 开始
seg4: vaddr 0x40378000 len 0xfe60  data@0xa0514
```
按 `data@0x94cd0` 重新取 `0x3fcba3f0`，bin 与 ELF **都是 1**。
运行时对比也证明 `.data` 加载正常：`0x3fcba3f4` 始终等于初值 `0x00010004`。
软复位后 `wst` 立刻回 1 ⇒ `.data` 被 bootloader 正确重载。

## §423 `wst` 变成 2 是时序相关，不是稳定的因果

同一个镜像两次捕获：

| 运行 | `[wst] entry` | `after phy_cal` | `after phy_enable` |
|---|---|---|---|
| 刚烧写完 | 1 | 1 | 1 |
| 另一次 | 2 | 2 | 2 |

⇒ 有时进 `s3_win_wifi_main` 之前就已经是 2 了，**不总是 `s3_win_phy_enable`
的锅**。用 halt 打断再读也是 1。判为时序相关，暂不深挖。

**但 `if (*wst != 1u) { *wst = 1u; }` 这条强制复位仍然有效且必须保留** ——
它保证不管进来时是什么值，`esp_wifi_init_internal` 都能走完整初始化流程。

## §424 当前卡点（本轮结束时的状态）

```
M1..M5 ✓ → [wifimain] id=1 → W0 phy → register_chipv7_phy rc=0/1 → W1 bringup
→ B0/B1/B2 ✓ → [wifi] init 前状态=2（已强制复位成 1）
→ [lock]/[lockok] ✓ → [pp] pp rom version ✓ → [net80211] net80211 rom version ✓
→ [unlock] ✓
→ [v2]wifi|create wifi task: queue not null      ← 库认为队列已存在
→ [semnew] #2 → [task] 库建任务
→ TRAP cause=0x1c(StoreProhibited) pc=0x4037d338 vaddr=0x18 a0=0x80043d06
```

`pc` 落在 `osi_queue_recv_body` 里一条 `l32i aN, a4, 24`，`a4 = 0`
⇒ **`queue not null` 和这个 NULL 队列是同一个问题的两面**：库以为 wifi
任务的队列已经建好了，于是跳过创建，后面 recv 就拿到 NULL。

### 下一步
1. 查 `[v2]...queue not null` 是库检查了哪个全局（大概率是
   `wifi_global` 里的一个 queue 指针）。
2. 对齐 IDF 的真实顺序：**不要让 PHY 在我们这边单独 enable**，
   而是让 `esp_wifi_init()` 内部通过 OSI 的 `_phy_enable` 回调去做。
   `S3_PHY_VIA_OSI` 已经把这个开关写好了（`win_island.c`），
   但它现在**没有真正阻止 PHY 提前初始化**——因为 `s3_win_phy_cal()`
   （W0 阶段）本身就会调 `register_chipv7_phy`。要一起处理。

## §425 ★★ ROM 变量区跨复位不初始化 —— `s_wifi_queue` 带着上一轮的失效指针

### 发现
`port/s3/rom/esp32s3.rom.ld` 里有 **146 个变量是绝对地址定义**：

```
s_wifi_queue  = 0x3fcef918;
g_osi_funcs_p = 0x3fcef940;
...
```
全部落在 **0x3fcef81c .. 0x3fcefffc**（约 2KB）。而 `link.ld` 的 DRAM 区域
上界正好是 **`0x3FCEF000`** ⇒ 这 2KB

* 不在 `.bss` 里 ⇒ 我们的启动清零碰不到
* 不在 bootloader 加载的段里 ⇒ 复位也不会重写

⇒ **复位后原封不动地保留上一轮的值**。JTAG 实测：

```
复位后 0x3fcef918 (s_wifi_queue)  = 0x3fcc45b0   ← 上一轮的堆指针，早就失效
写 0xcafebabe 再读回             = 0xcafebabe   ← 区域本身可读写，没问题
0x3fcef940 (g_osi_funcs_p)       = 0x3fcc3cb4   ← 同样有效指针
```

libpp 拿到非空的 `s_wifi_queue` 就打出
`create wifi task: queue not null`，然后拿这个**已失效**的指针去 delete /
复用 ⇒ `TRAP cause=0x1c(StoreProhibited) vaddr=0x18 a4=0`。

### 试过：清整片 2KB（失败）
在 `start.S` 的清 bss 之后加 `0x3FCEF800..0x3FCF0000` 全清零
⇒ **退化**成 `TRAP sp=3fcd8ed0 cause=0x1c pc=0x4004eabc`（ROM 自己的状态也在
那 2KB 里）。已回滚。

### 修法：一个一个清，只清确认需要的
在 `s3_wifi_bringup()` 开头：

```c
{ extern void *s_wifi_queue; s_wifi_queue = 0; }
```
（`rom.ld` 里是绝对符号，C 里这样写会生成 `l32r + s32i`，直接落到
`0x3fcef918`，正是想要的位置。）

### 真板结果 —— 又前进一大截
```
[pp] pp rom version ✓ → [net80211] net80211 rom version ✓ → [unlock] ✓
→ （不再有 "queue not null"）
→ [semnew] #2 ✓
→ [task] 库建任务 fn=40381b10 id=3 stack=6144
→ [give] t=3 sem#2 ✓
→ [wifimain] id=1 tick=30      ← ★ s3_win_wifi_main **第二次**被进入
→ W0 phy start → register_chipv7_phy
→ TRAP cause=0x02(IllegalInstruction) pc=0x3fcef000 vaddr=0x3fcef000 a0=0x80035810
```

### 下一步（新线索，优先级最高）
**`[wifimain] id=1` 出现了两次**（tick 分别是 0 和 30）。任务入口
`s3_win_wifi_main_task` 是 `rc = s3_win_wifi_main(); ... for(;;)`，
正常绝不会重入 ⇒ **任务被切走再切回来时，PC 回到了函数开头**，
即 `port_task_switch` 的恢复帧不对。

`cause=0x02 pc=0x3fcef000`（正好是 DRAM 上界）也印证"返回地址被写坏"。

要查：`port_task_switch` 保存/恢复时 `c->sp` 是否真的指向这次保存的帧
（之前的观察：`ctx[0].sp` 指向的是一段普通函数帧、不是切换帧）。

## §426 新线索定位：`s3_win_wifi_main` 在同一任务里被"重入"

真板日志（多次运行一致）：

```
[wifimain] id=1 tick=0     ← 第一次
...
[wifimain] id=1 tick=16    ← 第二次（另一次运行是 tick=30）
```

* 任务号 `id=1` **相同** ⇒ 不是"又建了一个任务"，是**同一个任务**再次执行
  函数开头的 `ets_printf`。
* `s3_win_wifi_main_task` 的结构是
  `rc = s3_win_wifi_main(); *(u32*)0x50001200 = rc; for (;;) { }`，
  正常绝不可能回到开头。
* 结合 `TRAP pc=0x3fcef000`（正好是 DRAM 上界，即"返回地址被写成数据区地址"）
  ⇒ **任务栈帧被破坏了**。

### 现场（JTAG，halt 后读 `g_ctx`）
`g_ctx` 当前在 `0x3fcc2ed0`（**注意：地址随构建漂移，必须每次用 nm 取**）

```
ctx[0] sp=3fcbee50 栈=[3fcbced0,3fcbeed0) 4096  EPC=40379896(s3_task_tramp)  ← 新帧，从未跑过
ctx[1] sp=3fcc0300 栈=[3fcbeed0,3fcc06d0) 6144  EPC=42017f58(os_task_block_wait)
ctx[2] sp=3fcc1b00 栈=[3fcc06d0,3fcc1ed0) 6144  EPC=42017f58(os_task_block_wait)
ctx[3] 空
```

`ctx[0]` 的 `a1sl(+76) == sp == top` ⇒ 它是 `port_task_ctx_create` 造出来、
**还停在 `s3_task_tramp` 入口没启动**的新帧。

而栈分配诊断 `0x50000460` 显示 wifi 任务的栈基址是 **`3fcd8e90`**
（贴近 `_stack_top = 0x3FCD9000`），**不是** `g_stack_pool(0x3fcbced0)` 那片。

⇒ **`g_ctx` 里的槽和实际在跑的任务对不上**，这一条要先弄清楚。

### 下一步
1. 把 `0x50000460` 的分配记录用**正确的列宽**重读（`mdw` 输出按行折行，
   现在的解析把槽号/栈/大小错位了），确认每个任务拿到的栈范围和大小。
2. 确认 wifi 任务的栈到底是 8192 还是被削到 4096 —— 若是后者，
   `s3_win_wifi_main` 里 PHY 那段深调用必然溢出，正好解释"帧被破坏 /
   PC 跳回开头 / PC 落到 0x3fcef000"。
3. 若确认溢出，加大该任务栈，或在 `port_task_ctx_check` 里加栈水位记录。

## §427 注意：栈分配诊断区 `0x50000460` 已被恢复环覆盖，不可用

`port_task_ctx_create` 把每次分配的 (槽号, 栈基址, 大小) 写到 RTC 的
`0x50000460`。逐字精确读出来是：

```
#1 槽=1    栈=0x3fcd8e90 大小=0x420191c0   ← 大小是个**代码地址**
#2 槽=0x60800(PS 值!) 栈=0x1000    大小=12  ← 0x1000 是 WINDOWSTART 值
#3 槽=3    栈=0x3fcc06d0 大小=6144
```

`0x60800`、`0x1000` 分别是 PS 和 WINDOWSTART 的值 ⇒ **这块 RTC 已经被
`RESTORE_FRAME` 的恢复环（`0x50000400`）写穿了**。
`0x3fcd8e90` 也正是主栈的 sp，同属被覆盖的残留。

⇒ **以后不要再用 `0x50000460` 判断栈分配**。
下轮改成在 `port_task_ctx_create` 里直接
`ets_printf("[ctx] slot=%u stack=%08x size=%u\n", ...)`，
或者把诊断挪到 `0x50001E00` 这类没被其它环占用的位置。

## §429 ★★★ 决定性 bug：`port_task_switch` 把 sp 存进了 `g_sched[core]`，而不是当前任务的 ctx

### 症状链（全都能对上）
* `[wifimain] id=1 tick=0` … 过一会儿 `[wifimain] id=1 tick=16/30` **又打一次**
  —— 同一个任务重新执行函数开头
* JTAG 读 `g_ctx[0]`：`EPC = s3_task_tramp`、`a1sl(+76) == sp == top`
  —— 明明在跑，帧却还是"从未启动"的样子
* `TRAP pc=0x3fcef000`（返回地址被写坏）

### 根因
`port/s3/ctx.S` 的 `port_task_switch` 里：

```asm
movi    a6, 76
mull    a5, a12, a6
l32r    a3, .Lg_sched      ; ← 错！这是 per-core 的**一个**固定槽
add     a3, a3, a5
s32i    a1, a3, 64         ; 保存 sp
```

任务实际用的是 `ctx.c` 里 `static S3Ctx g_ctx[]`（由 `port_task_ctx_create`
分配、`g_s3_cur[core]` 指向）。**`g_sched` 从来没人当任务的 ctx 用** ⇒

> 被切走的任务，它的 `sp` **一次都没有被保存过**

⇒ 恢复时永远读 `port_task_ctx_create` 造的那个"新任务帧"
（EPC = `s3_task_tramp`、`a1sl == top`）
⇒ **任务每次被调度都从函数开头重新执行**。

### 修法
先取 `g_s3_cur[core]` 指向的那个 ctx，再写它的 `sp`(偏移 64)：

```asm
l32r    a3, .Lg_cur
slli    a5, a12, 2
add     a3, a3, a5
l32i    a3, a3, 0
beqz    a3, 81f            ; 取不到就跳过，别把 0 当指针写
s32i    a1, a3, 64
81:
```

### 真板结果（同一镜像前后对比）
| | 修之前 | 修之后 |
|---|---|---|
| `[wifimain] id=1` | 打 **2~3 次** | **只打 1 次** ✓ |
| `[wifi] init 前的库状态` | **2**（污染） | **1**（正常）✓ |
| `register_chipv7_phy` | 第二次调用时崩 | 干净一次 |
| `[give] t=2 sem#1` 之后 | 重入→崩 | 稳定停住 |

⇒ §421 里那个"`wst` 莫名其妙变成 2"的时序谜团**也解释了**：那是任务重入
导致 PHY enable 被跑第二遍。**不是时序，是重入。**

### 顺带确认
`port_enter_task()`（`console.c:82`）是**空函数**，而 `port_set_cur_ctx` 只在
`irq.c` 的 `#ifndef S3_NO_PREEMPT` 分支里调用。也就是说 `g_s3_cur` 只能靠
`port_task_switch` 自己维护 —— 这正是上面这个 bug 一开始没被发现的原因：
两条更新路径，一条是空的、另一条写错了地方。

## §430 `port_task_yield` 恢复的必须是**自己**的帧

`port_task_yield` 保存那半边是对的（`s32i a1, a4, 64`，`a4 = g_s3_cur[core]`），
但恢复那半边从 `g_sched[core].sp` 读 —— 那是"最近一次 `port_task_switch`
外出的 sp"，属于**别的**任务。§429 把 switch 的写入挪走后这里更明显。
改成 `l32i a1, a4, 64`（`a4` 保存前就已经是当前任务的 ctx）。

## §431 栈池 24576 → 26624：闭源库要建 **3 个** 6144 字节任务

真板诊断 `0x50000BE0` 精确读出：`需要=6144 已用=20480 池大小=24576`。

闭源库会建 ppTask / wifi task / net80211 三个 6144 的任务，加上我们自己的
wifi 主任务 8192：

```
8192 + 6144*3 = 26624 > 24576
```

于是第三次 `_task_create` 失败，库打印
`[v1]wifi|create wifi task: failed to create task`，
`esp_wifi_init_internal` 返回 **257 (0x101 = ESP_ERR_INVALID_ARG)**。

改成 26624 后第 4 个任务建成功（`[task] 库建任务 … stack=6144` + `[give] t=4 sem#3`）。

## §432 ★★ 真根因：**主栈只剩 146 字节**，一直在往 `.bss` 里写

这一条是这轮最要命的。

`https` 目标传 `S3_STACK_TOP=0x3FCD9000`，`_stack_limit = _bss_end + 0x800`。
而 `port/s3/wifi_glue.c` 里

```c
static u32 g_wifi_heap_mem[16384];      /* 64KB */
```

把 `.bss` 顶到 `_bss_end = 0x3fcd8c6e`。于是：

```
主栈可用 = 0x3FCD9000 - 0x3fcd8c6e - 0x800 = 0x92 = 146 字节
```

**146 字节连一次 `port_task_switch` 的帧都放不下。**
真板实测 `port_task_switch` 的外出 sp 是 **`0x3fcd9390`** —— 已经**低于**
`_stack_limit(0x3fcd946e)`，也就是说主流程从很早开始就在往 `.bss` 里压栈，
把自己进程内的全局变量（任务表、ctx、堆指针……）踩烂。任何怪现象都出得来。

### 修法
`g_wifi_heap_mem` 64KB → **32KB**（`[16384]` → `[8192]`）。

| | 修之前 | 修之后 |
|---|---|---|
| `_bss_end` | `0x3fcd8c6e` | **`0x3fcd0c6e`** |
| `_stack_limit` | `0x3fcd946e` | **`0x3fcd146e`** |
| 主栈可用 | **146 字节** | **32914 字节** ✓ |

### 诊断位置的两个坑（记下来免得再踩）
1. **`0x50000460`** 被 `RESTORE_FRAME` 的恢复环（`0x50000400`）写穿
   —— 读出来是 PS/WINDOWSTART 的值（§427）。
2. **`0x50001D00`** 被 `console.c` 的 `port_snapshot()` 占用，
   `sn[0] = 0x5A5A5A5A` —— 我一度把它当成"ctx 指针是 5a5a5a5a"的证据，
   实际那是快照魔数（§433）。
3. RTC 在崩溃复位后会被 `start.S` 整片清掉，**诊断数据活不过一次崩溃**。
   要跨崩溃只能靠串口。

## §434 当前剩余问题：`[wifimain]` 每 ~13 tick 周期性重入

修完 §429~§432 之后仍然存在：

```
[wifimain] id=1 tick=0
W1 bringup → [wifi] init 前状态=1 → [task] 库… → [give] t=2 sem#1
.bbbbbbbb…(120 个 b)
[wifimain] id=1 tick=18     ← 重入
W1 bringup → [wifi] init 前状态=2  ← 状态被第二次 PHY/init 污染
```

**已排除**：
* 不是 `port_task_switch` 没保存 —— §429 已修，且实测 `switch 次数 = 1`
* 不是 `port_task_yield` —— §430 已修，且实测 `yield 次数 = 1`
* `ctx[0].sp` 仍等于 `port_task_ctx_create` 造的 `top`，
  `EPC = 0x403798c2 = s3_task_tramp` ⇒ **这个任务的帧确实一次都没被保存过**

**矛盾点**：`switch` 只发生 1 次、`yield` 只发生 1 次，但重入 4 次
⇒ **重入既不经过 switch 也不经过 yield**。

**下轮从这里入手**：
1. 查 `[give] t=2 sem#1` 之后那串 `b`（120 个）是谁打的 —— 它紧贴在重入前面，
   很可能就是"任务被重新拉起"的那条路径（`kernel/task.c6` 的调度主循环）。
2. `t->state` 是否被误设成 `OS_TASK_DEAD` → `port_task_ctx_free(t->ctx)`
   → `g_ctx[0].stack = 0` → 下一次 `os_task_create` 复用该槽并**重置
   `c->sp = top`**，于是从 `s3_task_tramp` 重新开始。
   注意现在 `ctx[0].stack` 非 0（8192），但那是**最后一次**分配的结果，
   中间可能被 free 过又建回来。
   下轮在 `port_task_ctx_free` 里加串口打印就能确认。

## §435 回滚 §429/§430 —— 我搞错了 `g_sched` 的角色

**`g_sched[core].sp` 不是"当前任务的 ctx"，它是"调度器主循环的返回点"。**

架构是这样的（`kernel/task.c6:629-640`）：

```
调度器主循环:
    port_enter_task();          // 空函数
    port_task_ctx_check(t->ctx);
    port_task_switch(t->ctx);   // ① 存调度器 sp 到 g_sched[core].sp
                                // ② g_s3_cur[core] = t->ctx
                                // ③ 从 t->ctx->sp 恢复 → 任务开始跑
任务 t:
    ... os_task_block_wait() → port_task_yield()
                                // ④ 存 t 的 sp 到 g_s3_cur[core]->sp
                                // ⑤ 从 g_sched[core].sp 恢复 → 回到调度器
调度器:
    port_leave_task();          // g_s3_cur = 0
```

**两个函数分工是反的**：`switch` 存调度器、`yield` 存任务。
我按"switch 应该存任务"去改，等于把两半都改坏了。

### 决定性证据：`b` 的数量
`kernel/task.c6:343` 在 `os_task_block_wait` 里 `port_putc('b')`。
§430 之后串口刷出 **120 个 `b`**；回滚后变回 **1 个**。

原因：`os_task_block_wait()` 在 `state = BLOCKED` 之后**只调 `port_task_yield()`**，
它**依赖 yield 切回调度器**。yield 若原地恢复自己，阻塞就退化成忙等，
同一句话被重试上百次。

## §436 栈池 26624 → 36864：库要 **4 个** 6144 任务

真板读 `g_pool_off = 26624`（池满）同时 `0x50000BE0` 记着"需要=6144"
—— 还有第 4 个库任务建不出来。于是

```
8192（我们的 wifi 主任务）+ 6144 * 4（ppTask/wifi/net80211/还有一个）= 32768
```

给到 **36864** 留余量。之后 `g_pool_off = 32768`，5 个任务的 ctx 全部有效。

## §437 WiFi 堆 32KB → 48KB：32KB 会让库拿到空指针

§432 把 `g_wifi_heap_mem` 砍到 32KB 后，真板崩在：

```
TRAP sp=3fcbe880 cause=0x1c pc=42056f25 vaddr=0x168 a0=80035810
```

`0x42056f25` = `ram_chip_i2c_readReg` 里的

```asm
42056f21: l32i.n a8, a3, 0      ; a8 = *(void**)a3 = 0
42056f25: l32i   a8, a8, 0x168  ; ← 崩，vaddr = 0x168 = 0 + 0x168
```

即**某个 i2c 函数表指针没分配出来**。48KB 就能过 PHY 了。
（64KB 当然更稳，但 `.bss` 会顶穿主栈 —— 见 §432。）

## §438 ★★ `S3_NO_PREEMPT` 是 **make 变量**，不是 `-D`

```make
ifneq ($(S3_NO_PREEMPT),)
WIFI_CDEFS += -DS3_NO_PREEMPT
endif
```

而 `WIFI_CDEFS` 通过 `HTTPS_CDEFS = $(WIFI_CDEFS) …` 传给 **C 文件**。

**正确构建命令**：

```sh
make C6_ROOT=/home/winger/c6 S3_APP_BASE=0x20000 \
     S3_NO_PREEMPT=1 S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON" https
```

之前一直写成 `S3_C6FLAGS="-DS3_NO_PREEMPT"` —— 那只给 `.c6` 编译器，
**`port/s3/irq.c` 里的定时器抢占切换从来没被关掉**。

### 这与"重入"直接相关
`port_switch_trace` 环（`0x50001500`，8 项）实测**全是 `tag=2`**：

```
#0 tag=2 id=1 ctx=0x3fcc5ed0 n=73
#1 tag=2 id=1 ctx=0x3fcc5ed0 n=74
...
```

`tag=2` 就是 `kernel/task.c6:598` 的 **"ISR 抢占切换"**，而 `ctx=0x3fcc5ed0`
正是 `&g_ctx[0]`（当前构建 `g_ctx` 就在 `0x3fcc5ed0`，紧贴栈池末尾，**不重叠**）。

**⇒ 整个重入是"定时器中断抢占切换"造成的**：
ISR 路径只 `port_set_cur_ctx(next)` 就恢复 next，**从不保存被打断任务的 `c->sp`**
（`port_irq_fill` 受 §376 约束默认不写），所以 wifi 任务每次被抢占再回来，
都是从 `port_task_ctx_create` 造的那个 tramp 帧重新开始。

这也解释了为什么 `ctx[1..4]`（库任务，都是主动阻塞）帧都正常，
而 `ctx[0]`（被抢占的）永远是 `a1sl == sp == top` 的 tramp 帧。

### 待验证
`S3_NO_PREEMPT=1` 的新镜像**已经构建成功**，但板子的 USB 在半死状态
（`/dev/ttyACM0` 消失、`OSError: [Errno 71] Protocol error`、设备号已经
枚举到 `Device 081`），需要**断电重插**才能继续烧写/抓串口。

## §439 新镜像已构建并静态验证，卡在硬件：板子 USB 需要物理断电重插

### 镜像
`build/wifi/c6os.bin`（721744 字节），构建命令：

```sh
make C6_ROOT=/home/winger/c6 S3_APP_BASE=0x20000 \
     S3_NO_PREEMPT=1 S3_C6FLAGS="-DS3_NO_WDT -DS3_NO_MON" https
```

### 已做的静态验证（没法上板，先在这层确认）
1. `s3_timer_irq` 的反汇编里**已经没有任何** `os_sched_preempt_ctx` /
   `os_sched_*` 调用 ⇒ `-DS3_NO_PREEMPT` 这次真的编进去了。
2. `port/s3/irq.c:354` 在 `#ifdef S3_NO_PREEMPT` 分支里直接 `return 0`
   ⇒ 中断路径不会 `port_set_cur_ctx(next)`，也就不会在"不保存 `c->sp`"
   的情况下换任务。
3. 内存布局：`g_stack_pool = 0x3fcbced0` 长度 `0x9000`，`g_ctx = 0x3fcc5ed0`
   —— **紧贴但不重叠**；`_bss_end = 0x3fcd746e`，`_stack_limit = 0x3fcd7c6e`，
   `_stack_top = 0x3FCD9500`，主栈 ~6KB。

### 硬件状态（需要人工）
板子的 USB 已经半死，软件层面无解：

```
$ dmesg | grep 'usb 2-4'
usb 2-4: can't set config #1, error -71     ← EPROTO，连配置都设不了
```

* `/dev/ttyACM*` 不出现（cdc_acm 不 probe）
* `esptool --before no-reset flash-id` → `Failed to connect: No serial data received`
* pyserial 设 DTR → `OSError: [Errno 71] Protocol error`
* openocd → `libusb_get_string_descriptor_ascii() failed`，随后 segfault 退出

`unbind`/`bind`、`authorized` 翻转、`modprobe -r/+ cdc_acm` 都试过，
设备能重新出现在总线上（`Device 081`），但**配置阶段就失败**。

⇒ **必须断电重插板子**，之后直接用上面的命令烧写即可。

## §440 联网代码路径静态审计：全部完整，只等硬件恢复

趁 USB 恢复不了，把"硬件一好就能一次跑通"的整条链路静态核对了一遍：

| 环节 | 位置 | 状态 |
|---|---|---|
| 凭据 | `port/s3/wifi_creds_local.h` | `weforpay.com` / `weforpay` ✓ |
| 关联 AP | `wifi_glue.c:1482 s3_wifi_associate` | `set_storage(RAM)` → `set_mode(STA)` → `set_config` → `start` → `connect` ✓ |
| 等连接 | `wifi_glue.c:1574 s3_wifi_wait_connected(15000)` | ✓ |
| DHCP | `c6_lwip_start_dhcp()` + `c6_lwip_wait_ip(20000)` | ✓ |
| 打印 IP | `c6_lwip_print_ip()` | ✓ |
| 取页面 | `c6_https_demo()` → `https_get("qq.com", "/", page, 4096)` | ✓ |

调用顺序在 `win_island.c:805-838`，`WSTAGE` 信标 `0x1021/0x1022/0x1023`
分别对应"发起关联 / 关联成功 / DHCP 成功"，读 RTC `0x50000A10` 就能知道卡在哪一步。

### 硬件恢复后的完整操作（三条命令）
```sh
cd /home/winger/rtos-c6/.git/dsh-worktrees/session-5e5272fb-9503-4864-a426-33ae0205
pkill -x openocd
timeout 260 ./tools/flashbl.sh build/wifi/c6os.bin 30
timeout 55 sudo -n python3 tools/cap.py -t 28
```

**这次要重点看两件事**：
1. `[wifimain] id=1 tick=` 是否**只出现一次** —— 若是，说明 §438 的重入真因确实修好了；
2. 是否走到 `WSTAGE 0x1021`（发起关联）及其之后的 `[wifi] associate ssid=weforpay.com`。

## §441 ★★★ `S3_NO_PREEMPT=1` 生效 —— "wifimain 周期性重入"彻底消失

真板验证（`S3_NO_PREEMPT=1` 作为 make 变量传入）：

| | 修之前 | 修之后 |
|---|---|---|
| `[wifimain] id=1` 出现次数 | **5 次**（tick=0/18/31/44…） | **1 次** ✓ |
| `[wifi] init 前的库状态` | 第 2 次起变成 2 | **始终是 1** ✓ |
| TRAP | 有 | **无** ✓ |

**§438 的判断完全正确**：整个重入就是这个坑 —— ISR 抢占路径只
`port_set_cur_ctx(next)` 就恢复下一个任务，**从不保存被打断任务的 `c->sp`**。

## §442 关掉 `g_dbg_sync`：串口打印把库拖死了

`g_dbg_sync = 1u` 原来在 **`esp_wifi_init_internal` 之前**打开、之后才关
（`wifi_glue.c:1706` → `1739`）。而 `osi_mutex_lock/unlock`、`osi_semphr_take/give`
每个都打 1~3 行，库在 init 内部要做**上千次**同步操作。

115200 波特率、每行约 40 字节 ⇒ **每行 3.5ms** ⇒ 光打印就要好几分钟。
真板表现就是"卡在 `WSTAGE 0x1005` 不动"，实际库每推进一步都在等串口。

关掉之后日志立刻变干净（`[lock]/[lockok]/[unlock]` 全消失），
43 行就跑完了 `[pp]` → `[net80211]` → `[task] 库建任务`。

（`§254` 当初注释说"关掉它反而走得更短"——那时是因为打印改变了时序、
掩盖了 §438 的抢占 bug。抢占关掉之后这个前提不再成立。）

## §443 `g_prid_base` 初值 0xFFFFFFFF 与汇编判断不匹配（已修，防御性）

`ctx.S` 里 `port_task_switch` / `port_task_yield` 判断核心号的汇编是：

```asm
rsr  a5, prid ; l32r a3, .Lprid_base ; l32i a6, a3, 0
movi a11, 0   ; beq  a5, a6, 2f      ; movi a11, 1
```

**只比较 `[0]`，不相等就直接用索引 1** —— 没有 C 版 `port_core_id()`
那个"还没登记就返回 0"的兜底。而 S3 的 `prid` 是脏值（实测 **0x0000cdcd**），
初值 `0xFFFFFFFF` 永不相等。

改成 `{0x0000CDCDu, 0xFFFFFFFFu}`。实测行为没变化（说明 `port_core_bind_self()`
在此之前已经登记过），但作为防御保留。

## §444 当前卡点：`WSTAGE=0x1005`，两个任务死锁在 `os_task_block_wait`

修复后跑到 `esp_wifi_init_internal` 内部就停住。真板现状：

```
[pp] pp rom version: e7ae62f
[net80211] net80211 rom version: e7ae62f
.[task] 库建任务 fn=40381ab4 id=2 stack=6144 prio=29(->2)
bWu1bu1Wu2.b.............Wu1          ← 停在这里，110 秒无新输出
```

字符解码（`kernel/task.c6` 的 §250 诊断）：
`b`=任务阻塞(`:343`)、`u`=唤醒任务(`:76`)、`W`=give 匹配上并 unblock(`:454`)、
`.`=give 时没有匹配的等待者(`:451`)。所以 `.` **不是错误**（信号量计数递增）。

**JTAG 实测（halt 后）**：

| | 值 |
|---|---|
| `WSTAGE` | `0x1005` ⇒ 还在 `s3_wifi_bringup()` 里 |
| `ctx[0]`（wifi 主任务） | sp=`3fcbe980` EPC=`42017f58`(**os_task_block_wait**) a1sl=`3fcbea00`=sp+128 **正常帧** ✓ |
| `ctx[1]`（库任务） | sp=`3fcc0300` EPC=`42017f58` **a0=0 a1sl=0** ✗ |

**两个任务都阻塞在 `os_task_block_wait` ⇒ 真死锁。**

**关键约束**：`RESTORE_FRAME` 在 `a1sl == 0` 时会跳到 `83f` 闸门写 `0xBAD4`
并死循环（`ctx.S:214 beqz a1, 83f`）。而 `ctx[1].a1sl` 实测是 0 ——
**所以一旦调度到库任务就会死在闸门里**。但板子只出现过 1 次 `ESP-ROM` 启动头
（不是复位循环），且闸门诊断区 `0x500018E0` 是随机值（没命中），
说明库任务**根本没被恢复过**，两个任务就这么互相等下去。

### 下轮从这里入手
1. 读任务表确认库任务的 `state` / `wait_kind` / `wait_obj`。
   `g_tasks` 是 `static`，符号表里没有 —— 但在 `os_task_probe` 的反汇编里
   能取到它的地址（`l32r a6, … (3fcb971d <__c99_d2>)` 附近，注意那是奇地址，
   实际结构要按 4/8 对齐往后找 `OS_TASK_MAGIC`）。
   `OsTask` 偏移：`state`@34、`core`@35、`wait_kind`@36、`wait_obj`@40（32 位）。
2. 重点怀疑：库任务等的对象和主任务 `[send] qh=3fcc75b0` 发的队列**不是同一个**，
   或者 `wait_kind` 不匹配（`task.c6:442` 要求 `wait_obj == obj && wait_kind == kind`）。
3. `ctx[1].a1sl = 0` 本身也要查：`port_task_yield` 里 `addi a14, a1, 128;
   s32i a14, a1, 76` 不可能得 0，除非那次保存走的不是 yield 的正常路径。

## §445 摸清任务表内存布局（`g_tasks` 是 static，符号表里没有）

`nm` 找不到 `g_tasks` —— c6 编译器把它并进了 `__c99_d2`（`0x3fcb971d`）。
用 openocd `dump_image` 导出整片再搜 `OS_TASK_MAGIC(0xC6A50001)` 得到：

**`g_tasks` 基址 = `0x3fcb971d`，`sizeof(OsTask)` = `0x70` (112)。**

**注意字段顺序和 `include/c6os/task.h` 的声明**不一样**（编译器重排了）：

| 实际偏移 | 字段 | 校验 |
|---|---|---|
| +0 | `ctx` | `0x3fcc5ed0`=`&g_ctx[0]` / `0x3fcc5f1c`=`&g_ctx[1]` ✓ |
| +4 | `arg` | 0 ✓ |
| +8 | `entry` | `0x4037ac24` / `0x40381ab4` ✓ |
| +24 | `id` | 1 / 2 ✓ |
| +28 | `magic` | `0xC6A50001` ✓ |
| +32 | `stack_size` | `0x2000`(8192) / `0x1800`(6144) ✓ |
| +44 | `prio`/`base_prio`/`state`/`core` | 字节串 `03 03 01 00` / `02 02 02 00` |
| +92 | `name` | `"wifi"` ✓ |

**状态枚举**：`0=FREE 1=READY 2=RUNNING 3=DEAD 4=BLOCKED 5=SUSP`。

**JTAG 实测那一刻**：wifi 主任务 **state=1(READY)**、库任务 **state=2(RUNNING)**，
`g_s3_cur[0] = 0x3fcc5f1c = &g_ctx[1]`，`g_prid_base[0] = 0x0000cdcd`（与 `prid` 一致，
所以核心号算的是 0，没问题）、`g_sched[0].sp = 0x3fcd9390`。

### 重要澄清：`ctx[1].a1sl==0` **不是 bug**
`ctx[1]` 是库任务**第一次**阻塞时留下的旧帧（EPC 确实停在 `os_task_block_wait`）。
库任务被唤醒后又跑了一段，栈向下增长把它自己的旧帧覆盖了 —— 所以 `a0/WS/a1sl`
变成 0，而 `c->sp` 还停在旧位置。**真正的问题是它之后一直在跑、不再 yield**，
于是 wifi 主任务虽然 READY 却永远拿不到 CPU（`S3_NO_PREEMPT` 下没有时间片）。

## §446 ★ 当前卡点：`windowbase = 14`（非法）→ `s3_win_spill` 野写

halt 后读 CPU 寄存器：

| 寄存器 | 值 | 含义 |
|---|---|---|
| `pc` | `0x40379e54` | **卡在 `_vec_level1` 异常向量入口**（`_vec_level1` 从 `0x40379e58` 起） |
| `exccause` | `0x1d` | **StoreProhibited**（非法写） |
| `excvaddr` | `0x0005ffe0` | 野地址（像个小整数被当指针） |
| `epc1` | `0x42014ee7` | **`s3_win_spill+0x40`，一条 `call12`** |
| `a1` | `0x3fcc0550` | 库任务栈内（`[3fcbeed0,3fcc06d0)`）✓ |
| **`windowbase`** | **`0x0e` (14)** | **非法！必须 4 的倍数（0/4/8/…/60）** |
| `windowstart` | `0x4935` | 位模式异常 |

**根因链**：`windowbase` 漂移到 14（非法）⇒ `s3_win_spill` 里那条 `call12`
按错误的窗口基址分配 ⇒ 写到 `0x0005ffe0` ⇒ StoreProhibited ⇒ 进 `_vec_level1`
⇒ 而异常入口的 `rotw -1` 让 wb 更加错乱 ⇒ 永久卡在向量里
（这正是 §343 记录过的现象，只是这次触发点换成了 `s3_win_spill`）。

### 下轮方向
1. `port/s3/w2c.S` 的 `WIN_SPILL` 宏：spill 前后有没有动 `windowbase`？
   `s3_win_spill` 是用 `entry` + `call12` 实现的，**`call12` 会把 windowbase 推进 3**
   （12 个寄存器的窗口），如果 `retw` 的 CALLINC 与它对不上，wb 就会漂。
2. 重点查 `s3_win_spill` 的 `retw` 是否与 `call12` 配对：**`call12` 必须配
   `retw`（CALLINC=3）**，若中间插了 `call8`/`call0` 把 CALLINC 冲掉，
   `retw` 就会退回错误的窗口数 ⇒ wb 漂移。
3. 也可以考虑：库任务在 `S3_NO_PREEMPT` 下长期不让出 CPU 本身也是个问题 ——
   即使窗口修好，wifi 主任务仍可能饿死。可考虑在 `s3_w2c_*` 壳里定期
   主动 yield，或恢复抢占但**同时**在 ISR 里正确保存 `c->sp`（这才是 §376 想做的事）。

## §447 ★★ `g_s3_win_spill_en` 从来没生效过 —— 修掉后崩溃消失

**发现**：`WIN_SPILL` 的 3 个调用点（`w2c.S:240/325/451`）本来就写了开关检查：
```asm
l32r a8, 12b ; l32i a9, a8, 0 ; beqz a9, .Lno_spill ; WIN_SPILL
```
**但 `s3_wifi_osi_init()` 里 `#ifdef S3_NO_WIN_SPILL` 的 `#else` 分支把值设成 `1`**
（默认开），所以这个开关**从加进来那天起就没有关过一次**，spill 一直在跑。

**修法**：
1. `wifi_glue.c`：`#ifdef S3_NO_WIN_SPILL` → `#ifdef S3_WIN_SPILL`，**默认 0**
   （需要做对照实验时用 `-DS3_WIN_SPILL` 打开）；
2. `w2c.S` 的 `WIN_SPILL` 宏内部再加一道守卫（`l32r a8,12b / l32i a8,a8,0 /
   `beqz a8,5f`，用本文件唯一空闲的数字标号 `5`），让宏自身也是完整的。

**真板效果（决定性对比）**：

| 项目 | 修前 | 修后 |
|---|---|---|
| `pc` | `0x40379e54` 卡在 `_vec_level1` 向量里 | `0x40379037` **在 `port_idle` 空闲循环** |
| `exccause` | `0x1d` StoreProhibited | `0x04`（普通 level-1 中断） |
| `excvaddr` | `0x0005ffe0` 野写 | **`0`** |
| `epc1` | `0x42014ee7` = `s3_win_spill+0x40` 的 `call12` | 不再落在 spill 上 |
| `windowbase` | `0x0e` | `0x0a` |
| `windowstart` | `0x4935`（位模式异常） | `0x0555`（`0b010101010101`，合法） |
| `a1` | `0x3fcc0550`（库任务栈） | `0x3fcd9400`（**主栈**） |

**§446 那条根因链确认成立**：spill 把 WINDOWBASE 推满一圈 → 触发窗口溢出异常 →
溢出处理体读 `[a1-12]` 帧链（call0 内核帧里没有这条链）→ 脏 a9 写飞 →
进 `_vec_level1` 且 wb 越转越乱 → 永久卡在向量里。

注：`ctx.S` 里的 `S3_SPILL_ALL` 宏（§304）**只定义、从未被引用**，是死代码，不是源头。

## §448 真正的死锁：两个任务在同一个 WiFi 内部对象上 `Q_WAIT_GET`

崩溃修掉之后，串口停在 `bWu1bb`，JTAG 看到 CPU 在 `port_idle`（`a0=0x4037902c`）
空转 —— 纯"所有任务阻塞"。

任务表实测（两个任务，`g_tasks=0x3fcb971d`，间距 `0x70`：

| 任务 | id | prio | state | wait_kind | wait_obj |
|---|---|---|---|---|---|
| wifi 主任务 | 1 | 3 | `4` BLOCKED | **`6` = `Q_WAIT_GET`** | `0x3fcc75b4` |
| 库任务 | 2 | 2 | `4` BLOCKED | **`6` = `Q_WAIT_GET`** | `0x3fcc75b4` |

**⇒ 两个任务在同一个对象上等"接收"，没有生产者 ⇒ 真死锁。**
`WSTAGE` 仍是 `0x1005`（`s3_wifi_bringup()` 内部）。

### 布局修正（重要，之前偏移算错）
`wait_obj` 是 `void*`，c6 按 **8 字节对齐**排到了 **`+56`**（不是 `task.h` 里的 40，
也不是我先前猜的 52）。`wait_kind` 在 **`+48`**（1 字节）。所以：
```
+44 = prio|base_prio|state|core   (0x00040303 ⇒ prio3 base3 BLOCKED core0)
+48 = wait_kind                   (6 = Q_WAIT_GET)
+56 = wait_obj                    (void*, 8 字节对齐)
+92 = name
```

### `0x3fcc75b4` 不是我们的 `OsQueue`
dump 该片（`0x3fcc75a0..`）：
```
0x3fcc75a0 = 0x57484646   "FFHW"（库内部 magic）
0x3fcc75a4 = 0x00000030   size=48
0x3fcc75a8 = 0x00000001
0x3fcc75b0 = 0x3fcc75b0   自指（空链表头）
0x3fcc75b4 = 0x3fcc75f0
```
我们的 `OsQueue` magic 是 `0x51E5C6A5`（`queue.c6:4`），**这一片里没有** ⇒
`0x3fcc75b4` 是 WiFi 库自己管理的对象（在 `g_wifi_heap_mem` 即 `0x3fcc7060` +
0xc000 范围内），不是我们 `s3_w2c_queue_create` 出来的。

### 顺带澄清：tick 是正常的
`0x50000700`(INR)=**13568**、`0x50000708`(tick)=**53** —— 那个 tick 槽是
"每 256 次才记录一次"的诊断（`irq.c` 里 `if ((st_n & 0xFFu)==0u)`），
**53×256 = 13568，精确吻合** ⇒ 定时器中断一直在跑，delay 逻辑没问题。
`S3_NO_PREEMPT` 的 `return 0` 在 `os_sched_tick()` **之后**，不影响 tick。

### 下轮方向
1. 查 `s3_wifi_bringup()`（WSTAGE 0x1005 之后那段）在等什么：
   `esp_wifi_init_internal` / `esp_wifi_set_config` / `esp_wifi_start` 哪个会在
   **库内部对象**上 `queue_recv` —— 那个对象的生产者是谁（WiFi 事件任务？）。
2. 怀疑点：WiFi 事件的投递依赖 `s_wifi_queue`（ROM `0x3fcef918`）或者库内部的
   event task。如果事件任务的入口没被调度起来，就永远没有生产者。
   日志 `bWu1bb` 里那个 `W` 是 give 命中了一个 `Q_WAIT_GET` 等待者（`task.c6:454`），
   说明确实有过一次投递，之后就没再有。
3. 也值得确认：库任务自身也阻塞在 `Q_WAIT_GET` 上是否**正常**（很多 IDF 内部
   任务就是这样等事件队列的），如果是正常的，那问题就纯粹是"上游没喂事件"。
