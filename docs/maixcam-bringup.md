# MaixCam（SG2002 / C906 大核）裸机点灯计划

> 状态：**设计稿 + 待上板实测**。目标：在 MaixCam 的 1GHz RISC-V `C906` 大核上裸机
> 跑起 C6OS，并在其上开 Linux ABI personality（`rv64-linux`，跑未修改的 riscv64
> Linux 程序），同时把除 WiFi / 视频编解码 / NPU / ISP 之外的全部板载外设点亮。
> 关联：[linux-abi-portable.md](linux-abi-portable.md)（平台无关设计）、
> [linux-abi.md](linux-abi.md)、[linux-abi-complete.md](linux-abi-complete.md)、[todo.md](todo.md)
>
> 标注约定：**[已核实]** = 公开资料/厂商 wiki 可查；**[待实测]** = 必须上板或读 TRM 才能确认。

---

## 1. 板子与芯片事实

| 项 | 值 | 来源 |
|---|---|---|
| SoC | SOPHGO **SG2002**（MaixCam = LicheeRV-Nano 核心板 + 底板） | [已核实] |
| 大核 | 1GHz **RISC-V C906**（RV64GCV，**Sv39 MMU**，M/S/U 特权级），跑 Linux | [已核实] |
| 小核 | 700MHz C906，**无 MMU**，跑 RTOS | [已核实] |
| 协处理器 | 8051 @ 25–300MHz（低功耗域） | [已核实] |
| 备选大核 | 1GHz Cortex-A53（与大核 C906 **二选一**，不是同时用） | [已核实] |
| 内存 | **256MB DDR3**（SiP），基址按 CV180x 惯例 `0x8000_0000` | [已核实]/基址 [待实测] |
| 显示 | 2.3″ IPS 552×368，**2 lane MIPI DSI**，屏 IC 属 **ST7701 系**（vendor 驱动名 `st7701_hd228001c31`） | [已核实] |
| 触摸 | 独立 6-pin 座，I2C 触摸 IC（型号待读板） | [待实测] |
| 相机 | 4 lane MIPI CSI（可拆 2+2），GC4653 / OS04A10 一类 5MP sensor | [已核实] |
| 音频 | 板载 mic + 1W PA | [已核实] |
| 存储 | TF 卡 / SD NAND 启动 | [已核实] |
| 网络 | 板载 WiFi6 + BLE5.4 模组（SDIO 类，疑 AIC8800 系）+ 可选以太网配件（SoC 内置 10/100 MAC-PHY） | 模组型号 [待实测] |
| USB | Type-C USB2.0（device + host，DWC2 类） | [已核实] |
| NPU | 1 TOPS INT8（**需要厂商固件/编译器，本轮不做**） | [已核实] |
| 调试 | UART0；BootROM 阶段 128000 8N1，Linux 阶段 115200 | [已核实]/引出方式 [待实测] |

**结论：硬件条件满足 `C6OS_CAP_LINUX_ABI`（MMU|USER|IRQ_USER）** —— 大核有 Sv39、
有 S/U 特权级、能在用户态被时钟中断打断。所以"裸机 + Linux ABI"在硬件上是成立的，
缺的是 `port/sg2002` 与 `abi/rv64-linux`（后者见平台无关文档 §2）。

---

## 2. 启动链与我们插进哪一段

Sipeed/厂商公布的 SG2002 启动链 [已核实]：

```
BootROM (BL1, 掩膜 ROM，闭源)
  │  在 SD 卡第一个 FAT 分区找 fip.bin，加载到 0x0C00_0000 (SRAM) 并跳转
  ▼
FSBL (BL2 角色，源码 github.com/sophgo/fsbl 分支 sg200x-dev)
  │  初始化时钟 + DDR；再链式加载 fip 里的后续段
  ▼
OpenSBI → U-Boot（fip 内的 payload，带 32 字节 loader header，典型加载地址 0x8020_0000）[待实测]
  ▼
U-Boot 从 FAT 分区读 boot.sd（大核镜像）→ 校验 → 跳转
```

关键约束（决定了我们只有三种插法）：

- **DDR 初始化与 SD 初始化都在我们改不动的地方**：DDR 在 FSBL（源码可得），
  SD 完全由闭源 BootROM 的 ROM API 做（`sophgo/fsbl` 里没有 SD 驱动）。
  所以**不能自创启动格式**，只能复用 fip / boot.sd 的打包方式。
- **C906 的 M/S/U 全部由我们自己决定**，因为 OpenSBI 也是可替换的 payload。

### 2.1 两种特权形态（这里修订一次原先的"不要 OpenSBI"）

| 方案 | 内核跑在哪 | 启动链改动 | 内核要不要进用户页表 | 附带能力 |
|---|---|---|---|---|
| **A：M-mode 内核**（无 OpenSBI） | M | 要替换 fip 的 payload 段，风险最高 | **不要**（M-mode 不查 `satp`）→ `CAP_KERNEL_UNMAPPED` | 自己管 PMP/`mret`/CLINT/PLIC，最干净但活最多 |
| **B：S-mode 内核**（保留厂商 FSBL+OpenSBI+U-Boot） | S | **零改动**：我们的镜像就是 `boot.sd`／U-Boot `go` 的目标 | 要（S-mode 一直看 `satp`，跟 x86 的 CR3 一样） | OpenSBI 提供 console/timer/IPI，先跑起来最快 |

**修订后的建议：V0–V2 走 B，跑通后再评估 A。** 理由：

1. B 的 V0 只需要"把裸二进制放进 FAT，用 U-Boot 的 `go` 拉起来"，不碰 fip 打包、
   不碰 DDR 交接 —— 这是整个项目唯一的**硬阻塞风险点**，能推迟就推迟。
2. 两种形态在 `lx/` 与 `abi/rv64-linux` 层**完全等价**（都 Sv39 + U 态 + `ecall`）。
   差别只在 `port/sg2002` 的 trap 入口、MMU 布局和 `CAP_KERNEL_UNMAPPED` 的取值 ——
   正好验证平台无关设计是否真的无关。
3. 走 A 的唯一硬收益是"内核不入页表"，代价是 PMP + `mret` + 自管定时器/串口；
   等 B 把板级外设和 ABI 都跑通再切，切换面被限制在一个 port 文件集内。

> 若最终要完全摆脱厂商固件（M-mode），把 V9 作为可选项，不动 V0–V8 的验收。

### 2.2 上板前必须先确认的四件事 [待实测]

1. **payload 槽位的格式**：`boot.sd` 的 loader header（magic/偏移/校验）与加载地址。
   备选工具：厂商 fip 打包工具，或 `sophgo_sg200x_genfip` 一类第三方脚本。
2. **UART0 引出方式与波特率**：14-pin 排针 / 板载焊点；ROM 用 128000，Linux 用 115200，
   我们的 console 从 115200 起步并允许 `-DSG2002_UART_BAUD=128000` 覆盖。
3. **DDR 基址与可用区间**：确认 `0x8000_0000` 起 256MB 全可用，BootROM/FSBL 是否留了保留区。
4. **小核与 8051 的状态**：我们起来时它们是否已被放出来。若已跑，必须
   `satp=0` + 清中断 + `wfi` 停放（或写复位保持寄存器），否则会随机踩 DDR。

---

## 3. 地址空间（Sv39，方案 B 布局）

C906 只有 **Sv39**（39 位 VA，上限 `0x7F_FFFF_FFFF`），x86-64 那套 47-bit 常量
（`config.h` 的 `STACK_TOP=0x7fffffe000`、`MMAP_BASE=0x7ff000000000`）**全部越界**，
必须按 ABI 后端配置重排。

| 区域 | VA | 映射 |
|---|---|---|
| 内核镜像（S 态，identity） | `0x8020_0000` + `[0, 6MB)` | R/W/X，supervisor |
| 直接映射窗口（`port_phys_map`） | `0xC000_0000 + (PA - 0x8000_0000)`，256MB | 2MB 大页，NX |
| 用户区 | `0x0001_0000 .. 0x3FFF_FFFF`（1GB） | per-AS，4KB 页，U 位 |
| ─ 用户 ELF（PIE 基址） | `0x0004_0000` | |
| ─ 解释器（`ld-musl-riscv64`） | `0x0100_0000` | |
| ─ mmap 向下长起点 | `0x2000_0000` | |
| ─ 用户栈顶 | `0x3FFF_E000` | 栈上限 8MB + 1 页 guard |

方案 A（M-mode 内核）下可以把用户区放到 `0x7F_FFFF_FFFF` 顶：内核不占 VA，
`user_max_va` 直接给 Sv39 上限。两者都由 `abi/rv64-linux` 的布局字段声明，
`lx/` 不知道差别。

---

## 4. C906 的四个"不做就踩坑"的约束

1. **I/D cache 非一致，且没有 Zicbom**。T-Head 用私有 CMO 指令（Linux 侧的
   `ERRATA_THEAD_CMO` 就是干这个）。装载 ELF / 写完可执行页的正确序列：

   ```
   store 指令 → fence (w,w) → th.dcache.civa(范围) → th.sync.s → fence.i
   ```

   `fence.i` 在 C906 上是**整刷 I-Cache**（论文实测约 914 周期，U74 只要 28），
   所以只放在装载/`mprotect(X)` 这类冷路径上，不进 exec 循环。
   CMO 指令编码（`th.dcache.civa`=`0x0272800b`、`th.dcache.cipa`=`0x02b5000b`、
   `th.sync.s`=`0x0190000b`）从 Linux thead errata 抄 [待实测复核]。
   → port 落地为 `port_icache_sync()` / `port_dma_sync()`，`CAP_ICACHE_SYNC|CAP_DMA_NONCOHERENT`。

2. **设备与 DDR 的地址属性分开配**：TRM 里的 `sysmap` 段属性（SO/C/B 位）把
   CLINT/PLIC/DMA 缓冲配成 non-cacheable；DDR 走 cacheable write-back。
   漏了会表现成"中断只在第一次有反应""SD 读出来的数据是旧的"。

3. **PMP 必须放开 U 态**（方案 A 才需要，但先记下）：M-mode 内核进 U 态前要配
   `pmpaddr0` + `pmpcfg0`（NAPOT，R/W/X），否则 U 态第一次取指就是 access fault。

4. **FPU 是 lp64d 的硬需求**：C906 有 D 扩展，riscv64 Linux 的 C ABI 是 lp64d ——
   每个函数都可能用 `f0–f31`。所以 `lx_frame` 必须带 FP 区，
   `mstatus.FS` 要在 trap 进出时维护（Initial/Dirty），线程切换要换 FP 状态。
   RVV 只到 **0.7.1**（`v0.7.1`），而 riscv64 Linux 用户态默认不用向量指令，可以不管。

---

## 5. 外设清单与可行性（"完整点亮"的口径）

| 外设 | 目标 | 可行 | 备注/工作量 |
|---|---|---|---|
| UART0 | console + `ttyS0` | ✅ | V0 的第一个可见成果 |
| CLINT 定时 + PLIC 中断 | tick、抢占 | ✅ | 方案 B 可先用 SBI timer，后换 CLINT |
| GPIO / pinmux / WDT / RTC | `/dev/gpio`、按键、看门狗 | ✅ | 几百行；USER 键做中断演示 |
| I2C / SPI / PWM / ADC | `/dev/i2c-N` 等 | ✅ | 14-pin 排针外设的总入口 |
| SD 卡 | 块设备 → FAT → rootfs | ✅ | 复用 `fatfs.c6`；`os_blkdev` 接口 |
| DSI + 屏（ST7701 系，2 lane） | `/dev/fb0`，控制台搬到屏 | ✅ | 寄存器抄 TRM，初始化序列抄厂商 `panel=st7701_hd228001c31` |
| 触摸（I2C） | `/dev/input/event0`（evdev） | ✅ | 先读型号再写驱动 |
| I2S + PA / mic | 最小 PCM（`/dev/snd0` 或 OSS 式 `/dev/dsp`） | ✅ | 不上完整 ALSA |
| 以太网（可选配件） | 复现有 SAL 协议栈 | ✅ | 性价比最高的"联网"；MAC 寄存器在 TRM |
| USB 2.0 device | CDC-ACM 串口 | ✅ | DWC2 device 模式 |
| USB 2.0 host | 枚举 + U 盘 | ⚠️ | 要写枚举/类驱动，约 +1 周，V8 之后看需要 |
| MIPI CSI + sensor | `/dev/video0`（V4L2 最小集），屏上预览 | ⚠️ 部分 | 只能 RAW/YUV 抓帧；sensor 初始化序列抄厂商驱动 |
| **ISP（3A/去马赛克/降噪）** | — | ❌ | 厂商闭源固件，即使在 Linux 下也是 `.ko`+固件 |
| **硬件 H.264/H.265 编解码（VPU）** | — | ❌ | 同上；替代：软编小分辨率或不做 |
| **NPU（1 TOPS）** | — | ❌ | 需要 CVI 模型编译器 + 固件；不做 |
| **WiFi6 + BLE5.4** | — | ❌ | SDIO 模组 + 厂家固件 + 数万行驱动；裸机不现实 |

**口径**：除最后四项（明确写进文档、不做），其余全部点亮。四项不做不是"没时间"，
而是"需要厂商固件，项目里没有任何可参考的寄存器级实现"。

---

## 6. 设备模型与 `/dev` 面（与 ABI 无关）

沿用平台无关文档 §6 的对象模型，MaixCam 侧只需要填三张表：

```
dev/core/           驱动注册/probe/资源（mmio、irq、DMA 同步）
dev/st7701s.c       panel（DSI 命令 + 时序）
dev/gt911.c  等     触摸（型号确认后）
dev/sensor_gc4653.c 相机 sensor 初始化序列
dev/dwc2.c          USB
dev/sg2002_sd.c     SD 控制器
dev/sg2002_mac.c    以太网 MAC
board/maixcam/      接线表：引脚、I2C 地址、中断号、控制器基址、屏/相机型号
port/sg2002/        SoC：CLINT/PLIC/UART0/时钟复位/pinmux/DDR map/cache CMO
port/rv64virt/      qemu -M virt（A2 的 CI 门禁，先于板子）
```

用户态可见面（最小 ioctl 集，够跑"未修改"程序）：

| 节点 | 最小能力 |
|---|---|
| `/dev/fb0` | `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO`/`FBIOPUT_VSCREENINFO` + `mmap` → 屏上出图 |
| `/dev/input/event0` | evdev：`EVIOCGNAME`/`EVIOCGBIT`/`EVIOCGABS` + `read` → 触摸坐标 |
| `/dev/video0` | V4L2：`VIDIOC_QUERYCAP`/`REQBUFS`/`QBUF`/`DQBUF`/`STREAMON`/`S_FMT` → 抓帧预览 |
| `/dev/i2c-N` | `I2C_RDWR` |
| `/dev/ttyS0` | termios `TCGETS/TCSETS` + `TIOCGWINSZ` |
| `/dev/mmcblk0` | 走 VFS 挂 FAT，用户态不需 ioctl |
| `/dev/dsp` 或 `/dev/snd0` | 自定最小 PCM ioctl（先不上 ALSA lib） |
| 网络 | 不出 `/dev`：socket 面走 SAL（M18） |

---

## 7. 里程碑（每级都要能单独演示，上一级不退）

| 阶段 | 内容 | 验收（`make check-maixcam-vN` 或人工现象） | 前置 |
|---|---|---|---|
| **V0** | 接管启动（方案 B）：裸二进制经 U-Boot `go` 跑起来；串口 banner、内存图、1kHz tick | 串口打出 `c6os sg2002 boot` + `ddr=256MB free=...` + `tick=1000/s`（±1%）；按 USER 键打一行日志 | §2.2 四件事 |
| **V1** | 存储：SD 块设备 → FAT → `ls /`；`/data` 可写 | 串口 `ls /` 列出卡上文件；写文件后宿主可见 | V0 |
| **V2** | `rv64-linux` ABI：U 态 + 静态 ELF + busybox `sh` | `PASS: maixcam V2（未修改的 riscv64 静态 ELF 在 U 态跑通）`；`sh -c 'echo hi > /data/x; cat /data/x'` → `hi` | V1 + `abi/rv64-linux`（依赖 A2/A3） |
| **V3** | DSI + ST7701 屏 + 帧缓冲：控制台搬到屏 | 屏上滚 tty；`/dev/fb0` 跑一个未修改的 fb 程序（画色块） | V0 |
| **V4** | 触摸 → `/dev/input/event0` | 用户态读坐标，屏上画点跟手（`tests/linux/touchdot.c`） | V3 |
| **V5** | I2S 音频：mic 录 1s → PA 放回 | 用户态 `tests/linux/rec1s.c` 打印录到的 RMS，回放可听 | V0 |
| **V6** | 网络：以太网 MAC 接 SAL（无配件则 USB-dev 网卡） | qemu 侧同款：`ping`/`wget` 拿 200；板子上 `ping` 通 | V1 |
| **V7** | CSI + sensor 抓帧 → `/dev/video0` → 屏上实时预览 | 屏上 5–15fps 实时画面（无 3A，色彩/曝光靠固定寄存器） | V3 + V4 |
| **V8** | 动态链接 + 信号 + 线程 + 抢占在板上跑全（Alpine rootfs 在 SD） | `check-linuxabi-l4..l8` 的验收串在板上逐条复现；`top`/`ps` 有输出 | V2 + V6 |
| **V9**（可选） | 切方案 A：M-mode 内核（无 OpenSBI），`CAP_KERNEL_UNMAPPED` | V0–V8 全部不退，仅 port 内变化 | V8 |

排序理由：**V0 是唯一的硬阻塞**（启动链），所以排在第一位且不与其他线并行依赖；
V2 依赖平台无关化的 A2/A3（qemu 上先跑绿），V3–V7 是设备线，可以与 ABI 线并行。

---

## 8. 前置物料与工具链

本机现在缺的（`command -v` 实测）：

| 需要 | 用途 | 状态 |
|---|---|---|
| `sg2002_trm_en.pdf`（**TRM，不是 datasheet**） | 寄存器级唯一来源 | ❗未下载（datasheet 只有规格表） |
| `sophpi` 仓库（分支 `sg200x-evb`） | DSI/panel/sensor 初始化序列、时钟/PLL 配置 | ❗未下载 |
| `sophgo/fsbl`（分支 `sg200x-dev`） | DDR/时钟初始化、fip payload 约定 | ❗未下载 |
| LicheeRV-Nano 原理图 + MaixCam 底板图 | 触摸 IC / PA / codec / sensor 型号、引脚 | ❗未下载 |
| `gcc-riscv64-linux-gnu` | 编 riscv64 用户态测试程序（`tests/linux/*.c` 直接交叉编译） | ❌ 缺（装 `gcc-riscv64-linux-gnu`） |
| `qemu-system-riscv64` | `-M virt` 当 A2 的 CI 门禁（板子之前就跑绿） | ❌ 缺（只有 user-mode `qemu-riscv64`） |
| `clang`（19.1.4，已有） | 编内核（`--target=riscv64-unknown-elf`，与 x86 侧同套路） | ✅ |
| USB 串口 + SD 卡（4–8GB，FAT32） | 调试与启动介质 | 待确认；**建议 SDHC + FAT32**（BootROM 的 FAT 解析器对 SDXC/exFAT 支持不可靠） |

下载走本机代理（见 `local-proxy-2334` skill）。

---

## 9. 风险表

| 风险 | 影响 | 概率 | 缓解 |
|---|---|---|---|
| `boot.sd`/fip 的 payload header 格式弄错 | V0 直接起不来，且现象只有"没输出" | 高 | 先用厂商原版镜像验证串口与启动；再把我们的镜像塞进**已知能工作**的槽位；准备 `go` 命令方式作为退路 |
| U-Boot 是否给 S-mode 还是 M-mode 入口 | 内核一开始就跑错特权级 | 中 | V0 先探测 `misa`/`mstatus`/当前特权级并打印；入口按实际改 |
| 非一致 cache / `sysmap` 属性配错 | 随机旧数据、代码执行乱跳（最难查） | 高 | 先把 `port_icache_sync`/`port_dma_sync` 全部做成"最保守全刷"，跑通后再优化 |
| DDR 保留区/基址判断错 | 早期随机死机 | 中 | 以 TRM 内存图为准，PMM 从内核结束处起，避开前 64MB |
| 小核/8051 已在跑 | 随机踩内存、无法定位 | 中 | V0 就停放它们并打印确认 |
| DSI 寄存器文档不全 / D-PHY 时序 | 屏点不亮，试错成本高 | 中 | 先照抄厂商 Linux 驱动的寄存器序列与 `sysmap` 属性；用示波器/逻辑分析仪看 lane 时钟 |
| sensor 初始化序列拿不到 | V7 停住 | 中 | 从 `sophpi` 的 sensor 驱动抄；先支持板子上实际那一颗 |
| 触摸 IC 型号不明 | V4 停住 | 低 | 读板 + I2C 扫地址 |
| FP 状态没进上下文 | 线程/信号下浮点结果错（且不崩） | 中 | A4 的专用用例（`fpthr.c`）必须先在 qemu 上绿 |
| 工作量低估 | 产品化延期 | 中 | 见下 |

工作量粗估（单人）：

| 段 | 估时 |
|---|---|
| A1 平台无关化（x86 零变化） | 3–5 天 |
| A2/A3 `rv64virt` + `abi/rv64-linux`（qemu 上 l2–l8 全绿） | 8–12 天 |
| V0 启动接管 + 串口/tick/GPIO | 3–5 天（含踩坑） |
| V1 SD/FAT | 2–4 天 |
| V2 板上跑静态 ELF + busybox | 2–3 天 |
| V3 DSI/屏 + fb0 | 4–8 天 |
| V4 触摸 + evdev | 2–3 天 |
| V5 音频 | 3–5 天 |
| V6 网络（以太网配件） | 4–6 天 |
| V7 CSI + sensor + 预览 | 6–12 天 |
| V8 Alpine 全量矩阵 | 3–5 天 |
| V9（可选）M-mode 内核 | 4–7 天 |

---

## 10. 与其它文档的关系

- **平台无关分层与 `lx_abi` 契约**：见 [linux-abi-portable.md](linux-abi-portable.md)。
  本文只消费它，不重复定义。
- **对 [linux-abi-complete.md](linux-abi-complete.md) §8 的修订**：原文"其他架构的
  Linux personality：不做"改为"不做多 ABI 共存，但单一 ABI 可按构建选择"；
  rv64 后端即本文 V2 的依赖。
- **对 [linux-abi.md](linux-abi.md) §3/§8 的范围说明**：那里的地址空间布局、syscall 面
  都是 `x86_64-linux` 后端的实例，不再代表"Linux ABI"整体。
- **对 `port.h` 的修订**：`port_ata_read/write`、`port_net_*` 属于板级设备，迁到
  `dev/` 的 `os_blkdev`/`os_netdev`；架构头只留 `port_arch.h` 里的原语。
