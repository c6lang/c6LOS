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
