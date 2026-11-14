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
