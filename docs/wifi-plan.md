
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
8. **flash 内容**：`esptool read-flash` + `cmp` 逐字节校验过。

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
