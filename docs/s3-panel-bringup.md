# S3 面板（ST7789 240×240）点亮实录

这块板子（Xingzhi Cube 1.54TFT WiFi）的面板驱动在 `port/s3/panel.c`，用
**GPSPI2 硬件 SPI + CPU 直写 FIFO**（不 bit-bang、不上 DMA）。本文记录从
"白屏/花屏/整机像死机"到跑通整屏刷新的全过程结论，避免以后再踩。

板型识别、上游参考（开源 XiaoZhi `78/xiaozhi-esp32` v2.5.0 的
`main/boards/nologo/xingzhi-cube-1.54tft-wifi`）见 `docs/s3-board-identification.md`。

## 1. 接线（与原厂 config.h 一致）

| 信号 | GPIO | 接法 |
| --- | --- | --- |
| SCLK | 9 | GPIO matrix 输出 `FSPICLK`(101) |
| MOSI/SDA | 10 | GPIO matrix 输出 `FSPID`(103) |
| CS | 14 | GPIO matrix 输出 `FSPICS0`(110) |
| DC | 8 | 普通 GPIO 输出（`SIG_GPIO_OUT`=0x100） |
| RST | 18 | 普通 GPIO 输出 |
| BL | 13 | 普通 GPIO 输出（高=亮，原厂用 LEDC PWM 调光） |
| 电源保持 | 21 | 原厂启动即拉高（RTC GPIO），关机拉 0 后 deep sleep |

`GPIO_OUT_SEL_CFG(pin)` 里的 `OEN_ALWAYS`（bit10）必须置位，否则矩阵输出的
引脚没有输出使能。

## 2. GPSPI2 最小可用初始化（顺序不能改）

```c
SYS_PERIP_RST_EN0 |= bit6;   /* SYSTEM_SPI2_RST   */
SYS_PERIP_CLK_EN0 |= bit6;   /* SYSTEM_SPI2_CLK_EN */
SYS_PERIP_RST_EN0 &= ~bit6;

SPI2_DMA_INT_CLR(0x38) = 0xFFFFFFFF;   /* 先清 DMA/TRANS_DONE 挂起 */
SPI2_DMA_INT_ENA(0x34) = 0;            /* 再关中断，否则 level-1 会反复重入 */

/* 关键：三段都要置，等价 IDF spi_ll_master_init 的
 *   clk_gate.clk_en=1; clk_gate.mst_clk_active=1; clk_gate.mst_clk_sel=1; */
*(u32*)(SPI2_BASE + 0xE8) = (1<<0)|(1<<1)|(1<<2);

SPI2_CLOCK(0x0C) = 0x00003043;   /* PLL80M/(N+1=4)=20MHz，n=3,h=1,l=3 */
SPI2_CTRL(0x08)  = 0;
SPI2_USER(0x10)  = USR_MOSI(1<<27) | CK_OUT_EDGE(1<<9);   /* SPI mode 3 */
SPI2_MISC(0x20)  = CK_IDLE_EDGE(1<<29) | CS_KEEP_ACTIVE(1<<30);
```

一次传输：

```c
for (i = 0; i < nw; i++) SPI2_W(i) = word;      /* W0..W15 = 0x98 + 4*i */
SPI2_MS_DLEN = n*8 - 1;
SPI2_CMD = CMD_UPDATE(1<<23);  while (SPI2_CMD & CMD_UPDATE) {}   /* 同步配置 */
SPI2_CMD = CMD_USR(1<<24);     while (SPI2_CMD & CMD_USR) {}
```

## 2.5 初始化延时必须按手册给够（实测标定）

`dly()` 是 **5 个 CPU 周期 / 单位**（80MHz 下 62.5ns/单位，实测 `dly(1000)`=5020
cycles、`dly(1200000)`=6.0M cycles）。原来 init 里用 `dly(1200000)` 只等 **75ms**，
而 ST7789 手册要求：

| 位置 | 要求 | 现在用的值 |
| --- | --- | --- |
| 复位释放 → SWRESET | ≥120ms | `DLY_120MS`=3000000（187ms） |
| SWRESET → 下一条 | ≥120ms | 同上 |
| SLPOUT → 下一条 | ≥120ms | 同上 |
| DISPON 之后 | ≥20ms | `DLY_20MS`=1000000（31ms） |

命令发太早面板会直接不理（表现就是"只有背光"），所以宁可多等。

## 2.6 初始化命令表

在最小集（SWRESET/SLPOUT/COLMOD/MADCTL/INVON/NORON/DISPON）之上补了 ST7789
240×240 各家开板示例通用的电源、门控、VCOM 与 gamma 表：`0xB2`(porch)、
`0xB7`(gate)、`0xBB`(VCOM)、`0xC0/0xC2/0xC3/0xC4/0xC6`(LCM/VDV/VRH/帧率)、
`0xD0`(电源控制)、`0xE0/0xE1`(正负 gamma)。不少屏在默认 VCOM/porch 下就是不亮。

## 3. 踩过的坑（每条都曾经让屏幕一片黑）

1. **GPIO 矩阵输出没打开输出使能（本项是"只有背光"的真正原因）**
   `GPIO_FUNCx_OUT_SEL_CFG` 只写了 `func_sel=101/103/110`，没置 `GPIO_ENABLE_REG`
   对应位。结果 SCLK/MOSI/CS 三根脚一直是 **高阻 + 内部上拉**：回读恒为 1、
   面板一个字节都收不到，而 SPI 事务本身照常完成（`CMD.USR` 自清、耗时随分频
   变化、数据在移位），看上去一切正常。IDF 里 `gpio_set_direction(OUTPUT)`
   做的就是置 `GPIO_ENABLE_REG`。修好后回读立刻变化：
   CS 从 1 变 0、MOSI 采样出现 0/1 交替。
   自检办法：`tests/t_s3_lcd_pins.c6` 读 pad 电平，`tests/t_s3_sigscan.c6`
   扫 0..127 号输出信号看哪个在翻转。
2. **`SPI_CLK_GATE` 少 bit0（`SPI_CLK_EN`，"enable clk gate"，复位值 0）**
   → 模块时钟被门控：写 `CMD.USR` 读回仍是 0，总线不出时钟，面板一个字节都
   收不到；但轮询"立刻通过"，非常像正常工作。这是最坑的一条。
3. **`volatile` 忙等循环会把 CPU 卡死（memw 内存屏障）**
   GCC 给每个 volatile 访存插 `memw`。SPI 事务之后只要有一条 APB 事务没回，
   下一条 `memw` 就永久等待：表现为串口突然静默、定时器中断也不再来，
   看起来像"整机死机/复位"。修法：面板驱动里的延时用不带 volatile 的
   `nop` 循环（`port/s3/panel.c: dly()`）。
4. **RTC 超级看门狗（SWD）没有关** → ROM/esptool 装了几秒超时，应用不关就
   每 ~3.5 秒整片复位一次：串口只能看到头几行、USB 每隔几秒重新枚举，
   极易误判成"CPU 停摆"。判断方法：读 `RTC_CNTL_RESET_STATE_REG`
   （`0x60008038`）低 6 位，`0x12` = SYS_SUPER_WDT。修法见 `port/s3/console.c`
   `port_init()`：`0x600080B8=0x8F1D312A`（解锁）→ `0x600080B4 |= 1<<30`
   （`SWD_DISABLE`）→ `0x600080B8=0`。
5. **RTC WDT**（`0x60008098` WDTCONFIG0，写保护钥匙 `0x50D83AA1` @ `0x600080B0`）
   同样要清 0。
6. **SPI mode**：原厂 `spi_mode=3`，即 `MISC.CK_IDLE_EDGE=1` +
   `USER.CK_OUT_EDGE=1`；写成 mode 0 面板时序虽然勉强能认，但没有理由不对齐。
7. **CS**：ST7789 串口靠 DC 区分命令/参数，`MISC.CS_KEEP_ACTIVE=1` 让 CS 常低，
   避免命令字节与参数分属两次事务时接口状态机被打断。
8. **DMA/TRANS_DONE 中断**：本内核 level-1 向量只按 `EXCCAUSE==4` 分流
   （分不清定时器和外设），所以 SPI2 中断必须 `DMA_INT_ENA=0` 且每次事务后清
   `DMA_INT_CLR`，否则会被当成定时器中断反复重入。

8. **`SPI_CLOCK` 的 H+L 必须等于 N**：`CLKCNT_N/H/L` 是"计数减一"，高电平
   拍数 H 与低电平拍数 L 之和必须等于一个周期的拍数 N（H+L=N）。写成
   `CLKCNT_N=3, CLKCNT_H=1, CLKCNT_L=3` 时 H+L=6≠N=4，占空比不成立。复位
   默认值 `0x3041`（N=4/H=2/L=2，20MHz）才是自洽的。

## 4. 自检手段（不依赖眼睛）

- **传输耗时随分频线性变化** → 主控确实在打时钟：

  | 分频 | 64 字节传输的 CCOUNT |
  | --- | --- |
  | /4（20MHz） | 2910 |
  | /24（3.3MHz） | 13185 |
  | /40（2MHz） | 21417 |
  | EQU_SYSCLK（80MHz） | 1360 |

  由 /40 档反算 **CPU 跑在 ~84MHz，即 80MHz PLL**（不是 40MHz XTAL），
  所以全屏刷新率要按 80MHz 折算。
- **RTC 慢速内存埋点**：`s3_mark(idx, val)` 写 `0x50000000+4*idx`，
  复位后可用 `esptool read-mem` 读回（注意 RTC 内存里有几个字不稳定，
  只用 0/2/3/9 号验证过；启动阶段 ROM 也会用一部分）。
- **复位原因**：`0x60008038 & 0x3F`；`0x60008018` 是 STATE0。
- **pad 电平回读**：输出脚也能读——置该脚 IO_MUX 的 `FUN_IE`(bit9) 后读
  `GPIO_IN_REG`(`GPIO_BASE+0x3C`)。这是发现"输出使能没打开"的关键手段。
- **传输中采样**：`s3_spi_begin()` 起一次慢速（125kHz）传输后立刻返回，
  调用方在 `CMD.USR` 仍为 1 期间反复读 pad 电平，等于给 SPI 做了个逻辑分析仪：
  实测 CS=0/2000（确实选中）、MOSI 996/2000（在翻转）。**SCLK(信号 101)
  至今采到恒高**，与"数据在动"矛盾，属未解疑点（见下）。
- **面板寄存器回读（无定论）**：bit-bang 模式下可以让 SDA 三态后按时钟采样，
  于是能真正发 RDDCOLMOD(0x0B)/RDDMADCTL(0x0A)/RDDID(0x04) 并读回。
  实测（`tests/t_s3_lcd_rdreg.c6`）：写 COLMOD=0x55 后读回 `0000`/`ff00`，
  写 MADCTL=0x60 后读回 `0000`/`0800`，RDDID 全 0——**读不出我们写进去的值**，
  既可能是这块屏的 SDA 只做输入（面板侧不驱动），也可能是回读时序还不对。
  因此"面板是否应答"目前仍只能靠人眼看屏确认。
- **SPI 读通路（已知未打通）**：想做 `RDDID(0x04)` 读回面板 ID 来自证
  "面板是否应答"，但 S3 的 SPI2 接收侧在本板始终读回 0xFF，已排除：
  ①`FSPIQ_IN_IDX=102` 映射寄存器没错（`GPIO_BASE+0x154+4*102`，
  bit[5:0]=pad、bit6=`sig_in_sel` 使能，最初漏了 bit6）；
  ②`USER.DOUTDIN=1`、`USR_MISO` 已置；
  ③正对照证明 pad 电平与 `GPIO_IN_REG` 都对——`tests/t_s3_lcd_id.c6` 把 SDA
  当普通 GPIO 驱动 0/1 时 `in=00/01`，但同一时刻 `rx=ffffffff`。
  即 MCU 侧 GPIO 读没问题，是 SPI2 的 MISO/RX FIFO 通路（或 FIFO 读取时机）
  还没对；在打通之前**不能用读 ID 判断面板死活**，只能靠人眼看屏。
  `s3_spi_loopback()` 是同一用途的早期尝试（把 `FSPIQ` 接到 MOSI 同一 pad），
  结论相同。

## 4.5 SPI2 的 SCLK 输出打不出边沿（**历史记录，真因见 §4.6**）

用 **PCNT 边沿计数器**直接数引脚，把"时钟到底有没有上脚"钉死了：

| 测点 | 64 字节传输期间的上升沿 |
| --- | --- |
| MOSI(GPIO10)、FIFO 装 0x5555… | **256**（= 512 bit 的一半，数据确实在移） |
| SCLK(GPIO9)、同一时刻 | **0** |
| 对照：GPIO9 当普通输出软件翻转 100 次 | **99**（测量链路正常） |

也就是说 SPI2 在移位、MOSI 在动，但 `FSPICLK`(信号 101) 始终停在高电平
（mode 3 的空闲电平）。已排除：`GPIO_ENABLE_REG` 已置（0x00246700 含 pin9）、
`OUT_SEL=0x65`(=101)、`SPI_CLK_GATE=7`、`CK_DIS=0`（把它置 1 引脚会变低，
说明脚确实挂在 SCLK 上）、`CLOCK=0x00003041`（H+L=N 自洽）、`CK_IDLE_EDGE`
置 0/1、`USER` 加 CMD/DUMMY/DOUTDIN、`equ_sysclk`、PLL/XTAL 两种源、扫
0..255 号**全部**输出信号（只有 103 会翻转，128–255 段也没有任何信号在动）、`SLAVE_REG.clk_mode_13`（{CPOL,CPHA} 支持位）
与 `CK_IDLE_EDGE`/`CK_OUT_EDGE` 的 6 种组合（SCLK 仍 0 边沿、MOSI 始终 256）、
以及 IDF `esp_lcd` 那种"命令相位 + 数据相位"的事务结构（`USER.USR_COMMAND` +
`USER2[31:28]`=命令 bitlen、`USER2[15:0]`=命令值；`tests/t_s3_cmdphase.c6`，
实测该结构下事务根本回不来，`tests` 在 init 阶段就卡住）。**下一步**：对照 IDF `spi_bus_initialize`
的完整初始化序列（尤其 `SPI_SLAVE_REG`/`SPI_CTRL` 里可能另有门控位），或换
SPI3/LCD_CAM 的 i80 通路。

### bit-bang 兜底通路（**驱动默认路径**）

`g_bitbang` 默认 1：所有用例（demo、dash、t_s3_lcd）都走这条。`s3_lcd_use_bitbang(1)`
把 SCLK/SDA/CS 从 GPIO 矩阵摘回普通 GPIO，用
`GPIO_OUT_W1TS/W1TC` 按 mode 3 手动打时钟。自检：64 字节写出 **512 个 SCLK
上升沿**（PCNT 计数，正好 64×8）；整屏 240×240 填色 **5.16M cycles ≈ 64ms
≈ 15fps**（80MHz 折算），**已经过 M7-3 的 ≥10fps 线**，而且和"硬件 SPI"
路径实测耗时几乎一样——反过来印证硬件路径其实没在打时钟。

**帧率上限实测**：把内层循环改成"整字写 GPIO_OUT + 阴影字"（`s3_lcd_set_bb_fast(1)`）
后，整帧 5140k cycles 对原版 5176k——只快 0.7%。说明瓶颈不是指令条数，而是
**APB 上 GPIO 寄存器写延迟**（每 bit 至少两次写），所以 bit-bang 在这块板子上
≈15fps 就是上限（已过 ≥10fps 验收线）。想更快只能让硬件 SPI 的 SCLK 出脚。

注意两点（都踩过）：
- `s3_lcd_init()` 里的引脚配置要沿用 `g_bitbang` 状态，否则 init 序列会被
  重新接回 SPI 矩阵、后面 GPIO 写不到 pad 上（表现为边沿数 0）。
- 本工具链 **u64 除法 ABI 不匹配**，测试里 `(u64)/1000` 会打出
  `TRAP cause=0`（IllegalInstruction）；先转 u32 再除。

### 4.5.1 当时的结论：硬件 SPI 不能用（**已被 §4.6 推翻**：是三个寄存器 bug）

同一份驱动、同一套初始化与像素数据，只把通路从 bit-bang 换成 GPSPI2 硬件 SPI
（`tests/t_s3_lcd_hw.c6`，`s3_lcd_use_bitbang(0)`）：

| 通路 | 串口 | 屏幕 |
| --- | --- | --- |
| 硬件 SPI（SPI2/SPI3，各种分频与模式） | `1 red 5173k-cyc` 正常打印 | **黑屏** |
| bit-bang（GPIO 翻转，PCNT 实测 512 边沿/64B） | `1 red 5175k-cyc` | **正常出图** |

两者整屏耗时几乎相同（≈5.17M cycles），说明硬件 SPI 的事务只是被"内部节拍"
拖着走、并没有把时钟打到引脚上。因此 `port/s3/panel.c` **固定默认 bit-bang**，
硬件通路保留为实验开关。

## 4.6 【已解决】硬件 SPI "SCLK 打不出边沿"的三个真因

长期卡在这里（bit-bang 兜底、帧率只有 ~5fps，连续刷新还会"定格"）。三个 bug
叠在一起，每修掉一个现象才往前走一步；下面每条都有**客观测量**支撑，不需要
逻辑分析仪、也不需要人眼。

### 4.6.1 `SPI_CLOCK.CLKCNT_L` 必须等于 `CLKCNT_N`

IDF `esp_hal_gpspi/esp32s3/include/hal/spi_ll.h: spi_ll_master_cal_clock()` 里：

```c
l = n;                    /* ← 恒等于 n */
reg.clkcnt_n = n - 1;
reg.clkcnt_h = (duty*n + 127)/256 - 1;
reg.clkcnt_l = l - 1;
```

我们原来按"高 H + 低 L = 一个周期 N"写成了 `L = N/2`（例如 0x00003041 =
N=4/H=2/L=2，恰好也是复位默认值）。结果：**SPI 内部照常移位**（MOSI 有边沿、
事务耗时随分频线性变化），但 **SCLK 引脚一个边沿都没有** → 面板只有背光。
这也解释了当年"MOSI 通、CLK 不通"的困惑。

真板对照（同一次传输里用软件当逻辑分析仪：78kHz 慢时钟，轮询 GPIO9/GPIO10）：

| CLKCNT_L | SCLK(GPIO9) 边沿 | MOSI(GPIO10) 边沿 |
|---|---|---|
| N/2（旧写法） | **0** | 256 |
| N（IDF 写法） | **1024** | 256 |

20MHz 下用 PCNT 复核：`l=n/2` → 0 边沿；`l=n` → 512 边沿（64 字节 = 512 bit）。
用例：`tests/t_s3_sclkdiag.c6`、`tests/t_s3_sclkdiag2.c6`。

### 4.6.2 FIFO 是**低字节先出**，不是 MSB 打包

S3 GPSPI 的 `data_buf`（0x98 起 16 个字）是按 `bit[7:0] → [15:8] → …` 顺序移出去
的（IDF 的 `spi_ll_write_buffer()` 就是直接 `memcpy`，不做字节交换）。
我们按 `w |= p[i] << 24 | …`（MSB 打包）写，于是：

- 4 字节一组被整组倒序；
- 单字节命令（`cmd()`）更是变成 `0x00` → **面板一条命令都没收到**（黑屏）。

客观验证（`tests/t_s3_spiwire.c6`）：SPI 发已知图案，软件扮演从机在 SCLK 上升沿
采 MOSI 拼回字节：

```
want = 55 0f f0 a5 3c c3 99 66
rx   = a5 f0 0f 55 66 99 c3 3c      ← 每 4 字节一组倒序
```

改成 `w |= b << (8 * k)` 后收到的就是 `55 0f f0 a5 3c c3 99 66`。

### 4.6.3 mode 3 的 `USER.CK_OUT_EDGE` 必须是 0

`CK_OUT_EDGE=1`（"在上升沿更新数据"）会和 ST7789 的上升沿采样**正好撞车**，
采到的是刚换上去的数据。同一次 wire 测试的 4 种组合：

| CK_IDLE_EDGE | CK_OUT_EDGE | 上升沿采到的字节 |
|---|---|---|
| 1 | 1（旧写法） | `4b e0 1e aa …`（全错） |
| 1 | **0** | `a5 f0 0f 55 …`（字节序修正后即正确图案） |
| 0 | 0 / 1 | 波形极性不对（idle 低），ST7789 用的是 mode 3 |

所以最终配置就是 IDF 那一组：`MISC.CK_IDLE_EDGE=1` + `USER` 里**不置**
`CK_OUT_EDGE`，`CLOCK=0x00003043`（80MHz/4 = 20MHz，N=4/H=2/L=4）。

### 4.6.4 附带的两处改动

- **CS 用普通 GPIO 常低**（不再走 FSPICS0）：ST7789 靠 DC 区分命令/数据，
  CS 常低完全合法，事务之间的 CS 时序就不再是变量。
- `port/s3/panel.c` 默认通路改为**硬件 SPI 20MHz**；bit-bang 完整保留
  （`s3_lcd_use_bitbang(1)`）作为兜底。

## 4.65 【已解决】"连续刷新几秒后定格"的真正原因：控制台把 GUI 卡住

这一条和面板/SPI 无关，是**内核 USB-Serial-JTAG 控制台**的坑：

`port_putc()` 里原来有两处 `guard > 2000000` 的忙等（等 `WR_DONE` 自清、等
`DATA_FREE` 置位）。逐条验证：每个字节最坏 2×2,000,000 次空转 ≈ **250ms**，
一行 20 字节的调试打印就是 **~5 秒**的阻塞。主机没在读串口时（板子独立跑、
上位机没打开 `/dev/ttyACM*`）`WR_DONE` 根本不会自清，于是：

- GUI 任务/单任务卡在 `port_putc` 里 → 画面**定格几秒**；
- 一旦上位机开始读串口 → 立刻"自己恢复"（用户实测确认）；
- 只在按键时才打印的版本几乎不卡 → 和"越连续刷越容易定格"完全吻合。

修法：两处等待上限都压到 20000 次（≈1ms，仍够 USB 满速帧 1ms/帧跑完），
超出就**丢字节**，绝不阻塞业务代码。实测（`tests/t_s3_gui.c6`，每 60 帧打印
一行、串口不接）：连续跑 1~2 分钟观察不到任何停顿。

## 4.68 上屏走 GDMA：五个坑（全部靠"自环"客观定位，不用眼睛）

目标是把"CPU 逐 64 字节喂 FIFO"换成 GDMA 一次推完（实测纯移位 1846k cycles，
CPU 开销几乎为 0）。中间的坑一个比一个隐蔽，最后都靠**自环**定位：把 MISO 从
MOSI 那根 pad 引回来（`GPIO_FUNC102_IN_SEL = pad10 | bit6`），发已知图案、读
RX FIFO 比对字节。CPU 通路做正对照（`cpuloop rx=550ff0a5…` 正确），这样 DMA
通路出问题时不会误判。

| # | 坑 | 现象 | 真相/修法 |
|---|---|---|---|
| 1 | 描述符 `owner` 位 | MOSI 全 0，但 USR 照样按时结束、cycles 正常 | lldesc w0 的 bit31 **必须=1**；GDMA 原始中断 bit2=`out_dscr_err`。IDF 实测参考值 `0xC0100100` |
| 2 | `OUT_LINK` 地址字段 | 同上 | 20 位字段是**相对 0x3FC00000 的偏移**，不是 `desc & 0xFFFFF`。IDF 真值 `link=0x008e9d80` ↔ 描述符 `0x3FCE9D80` |
| 3 | `dma_conf` bit19/20 | 第一拍正常、**第二拍起再也发不出去**（整屏只剩角落一小截） | 那两位是 slave 专用的 `*_seg_trans_clr_en`，master 模式下会把 outfifo 状态卡住。**只设 bit28 `dma_tx_ena`** |
| 4 | 连续多拍之间跳过 `UPDATE` | 整屏（8 拍）只画出第一拍的内容 | DMA 拍里**每次都写 `MS_DLEN` + `CMD.UPDATE`**（每 16KB 一次，开销可忽略）；CPU 通路的"DLEN 未变就跳过"优化不能套到这里 |
| 5 | 头部丢失 | 纯 DMA 时前 2 个字节是 `00`（`rx=000ff0a5…`） | **CPU 先往 W0 预灌头 4 字节**，描述符从 +4 开始 → `rx=550ff0a5…` 精确一致。另外：武装完要**立刻**置 USR，中间加延时会让 GDMA FIFO 空转报错（全 0） |

还有一个**工具链地雷**（和 `u64` 除法 TRAP 是同一类）：把字节交换写成
`(w & 0x00FF00FF) << 8 | (w & 0xFF00FF00) >> 8` 会被编译器识别成
`__bswapsi2` 并真的去调用，而这个 libgcc 辅助函数**一进去就 TRAP**
（`cause=0`，pc 落在 `__bswapsi2`）→ 整屏不更新。所以逐像素拆字节写。

现状（`port/s3/panel.c`）：
- **局部刷新（GUI 常态）走 DMA**：污区 469k cycles（5.9ms），整帧
  draw 1350k + blit 469k ≈ 23ms → **约 33fps**（原来 CPU 通路 + 整屏重绘是 5fps）。
- **大面积刷新（>4 拍 = >65KB）自动回落到 CPU 通路**：8 拍整屏仍有错位（上面
  第 3/4 条的残余），CPU 通路整屏 ~51ms 保证画面正确。
- 用例：`tests/t_s3_lcddma.c6`（自环 + 链 + prefill 对照）、
  `tests/t_s3_dmafill.c6`（色带/多拍范围）、`tests/t_s3_lcdperf.c6`（帧预算）。

## 4.7 只有背光的真正原因（本轮定位）

`cmd()`（命令字节）走的是**硬件 SPI** `spi_xfer()`，而 `dat()`（参数/像素）走
`spi_write()`（bit-bang 时才分派到 GPIO）。也就是说切到 bit-bang 之后，**所有
命令字节仍然从打不出时钟的硬件 SPI 出去**——面板一条命令都没收到，只有一堆
参数/像素数据。表现完全就是"背光亮、屏全黑"。

**同一个 bug 还有第二处**：`s3_lcd_fill()` / `s3_lcd_fill_rect()`（以及 blit 里
按块发送的部分）**直接调用 `spi_xfer()`**，绕过了 `spi_write()` 的分派——所以
即使命令修好了，像素数据仍然从硬件 SPI 出去，屏上只会显示面板里残留的旧画面
（看起来像"原厂待机图"）。

最终修法：把分派放进 `spi_xfer()` 本身（`g_bitbang` 时转 `bb_write`），这样
命令、参数、像素三条路全部统一，屏上立刻出现我们的画面（用户确认）。

## 5. 当前状态

**硬件 SPI 已打通（2025 本轮）**，`port/s3/panel.c` 默认就走它：

- 通路：GPSPI3（0x60025000）+ GPIO 矩阵（SCLK=GPIO9←66、MOSI=GPIO10←68），
  CS 用普通 GPIO 常低，DC/RST/BL 普通 GPIO，**20MHz**（`CLOCK=0x00003043`）。
- 真板实测：`tests/t_s3_gui.c6`（c6LOS 两个任务：按键 10ms 轮询 + GUI 任务
  **不节流**整屏重绘）连续跑 1~2 分钟画面一直在动，串口不接也不会卡；
  整屏一帧 ≈ 7.2M cycles ≈ 90ms（含 gfx 绘图），即 ~5fps；
  串口打印 `frame n tick=... d=60`。
- **帧预算（`tests/t_s3_lcdperf.c6`，80MHz CPU，整屏 115200 字节）**：
  | 项目 | 20MHz | 40MHz |
  |---|---|---|
  | gfx 绘图（全屏填充 + 文字 + 方块） | 805k cycles（~10ms） | 同 |
  | 上屏 `s3_lcd_blit` | 5943k（~74ms） | **4071k（~51ms）** |
  每块（64 字节）耗时模型（分频扫描 N=4/8/16/32 拟合出来）：
  `cycles/块 = 1254（固定：APB 喂 16 个字 + USR 轮询）+ 512×N（移位）`，
  斜率正好是 80MHz 源 / 80MHz CPU，说明时钟配置准确；那 1254 cycles
  的固定开销占了 20MHz 下 38% 的时间，就是"CPU 逐块喂 FIFO"的代价。
- 默认时钟已提到 **40MHz**（`CLOCK=0x00001001`，N=2/H=1/L=2）：真板目视确认
  画面干净、明显更快（再往上是 80MHz，超 ST7789 串行时钟规格，没冒这个险）。
- 下一步提速（按性价比）：
  1. **GDMA 一次推整帧**：上屏的固定开销直接归零，理论上只剩移位时间
     （20MHz→3.7M、40MHz→1.85M cycles）。难点是 ST7789 要大端字节序，而帧缓冲
     是 u16 本机序 → 需要一块"字节交换后的暂存缓冲"（115KB，S3 内存够）或者
     让 gfx 直接存成已交换序（会影响 host 侧 golden 帧测试，得加开关）；
     S3 的 GDMA 描述符地址字段（20 位、相对 DRAM 基址）是老坑，要留时间调。
  2. 局部刷新（dirty rect）：GUI 只贴变化的块，`s3_lcd_blit_rect` 里每像素
     `p/w`、`p%w` 的除法要先换成增量计算（现在每像素一次 `__udivsi3`，很贵）。
  3. gfx 全屏填充/文字渲染优化（805k cycles 里大部分是逐像素写）。
现在是"CPU 逐 64 字节喂 FIFO + 轮询 USR"，每块的
  APB 开销和移位时间差不多；下一步是 ①SPI 时钟提到 40MHz ②上 GDMA
  （`SPI_DMA_CONF` + `lldesc`）把整帧一次推完 ③gfx 全屏填充改成 memset 式。

- `tests/t_s3_lcd_hwlive.c6`：整屏连续刷新长跑（帧计数 + 移动条 + 每帧换色），
  每 100 帧打印一次帧周期，用来肉眼+串口双重确认"不再定格"。


- `make C6_ROOT=... BUILD=build/s3lcd S3_MAIN=tests/t_s3_lcd.c6 s3` →
  `fill_cycles=13984439`（fps10=57）、`blit_cycles=14634898`（fps10=54）、
  `PASS: s3 lcd (bitbang path)`。即**真实整屏刷新 ≈5.7fps**。
  （更早记录的 5.16M/≈14fps 是假的：那时像素数据还在走"打不出时钟"的硬件
  SPI，CPU 只是在空转等节拍，见 §4.7。）bit-bang 每个 bit 要 15 个周期
  （APB 上 GPIO 写延迟），这是这块板唯一可用通路的上限。
- `tests/t_s3_lcd_kiosk.c6`（**当前留在板上跑的那个**）：一轮里把三件事都演一遍
  —— 纯色红/绿/蓝（面板点亮）→ 8 色竖条 + `C6OS/ST7789 240X240/BITBANG OK`
  文字（帧缓冲+字体上屏）→ 仪表盘一帧（数字/DASH 渲染），每步 1.2~1.6s 循环；
  串口同步打印 `1 red 5168k-cyc … 5 dash 5814k-cyc tick=1581`。
- `tests/t_s3_lcd_demo.c6`：红 → 绿 → 8 色竖条 + `C6OS` 文字 → 蓝，各 1.5s
  循环，用来肉眼确认成色顺序、方向、窗口寻址。
- 待确认：面板实际成像（需要人眼）。初始化/整屏填色/帧缓冲 blit 的软件
  路径已跑通，串口数据（周期数、fps）可复现。

## 5.5 自检用例索引（都在 `tests/` 下，`make ... S3_MAIN=tests/<名字>.c6 s3` 即可构建）

| 用例 | 用途 |
| --- | --- |
| `t_s3_lcd_kiosk` | **演示/点亮验收**：纯色红绿蓝 → 8 色竖条+文字 → 仪表盘一帧循环 |
| `t_s3_lcd` | 正式验收：DRAM 自检 + 整屏填色/帧缓冲 blit 周期与 fps，`PASS: s3 lcd` |
| `t_s3_dash` | 仪表盘任务：tick/tasks/堆用量上屏，串口同步打印 |
| `t_s3_lcd_pins` | 读回 pad 电平（发现"输出使能没打开"就靠它） |
| `t_s3_sigscan` | 扫 0..255 号 GPIO 输出信号，找哪个在传输期间翻转 |
| `t_s3_sclk_cnt` / `t_s3_sclk_why` | 用 PCNT 数 SCLK/MOSI 边沿（含软件翻转正对照） |
| `t_s3_sclk_try` / `t_s3_clk13` | 逐个替换 SPI 配置（mode/时钟源/ck_mode_13…）看时钟是否出脚 |
| `t_s3_spi3` / `t_s3_vendor` | 切到 SPI3、照上游（SPI3 + equ_sysclk 80MHz）参数驱动 |
| `t_s3_dly` | 标定 `dly()` 周期数（5 cycles/单位），用于按手册算延时 |
| `t_s3_lcd_sweep` | 假设替换演示（慢时钟/DC 反相/INVOFF/BGR/旋转/全速） |
| `t_s3_lcd_id` / `t_s3_lcd_id2` / `t_s3_lcd_rdreg` | RDDID / 寄存器回读（本板读不出值，仅供参考） |
| `t_s3_sclkdiag` | **定位 SCLK 不出脚**：传输进行中软件轮询 GPIO9/10 数跳变（含正对照） |
| `t_s3_sclkdiag2` | 逐项对照 IDF 快照，证明 `CLKCNT_L=N` 才是时钟出脚的必要条件 |
| `t_s3_spiwire` | **验证线序/相位**：软件当 SPI 从机，上升沿采 MOSI 拼回字节与发送图案比对 |
| `t_s3_gui` | **GUI 连续刷新回归**：c6LOS 按键任务 + 不节流的 GUI 任务（原来会"定格"的场景） |
| `t_s3_lcd_hwlive` | 硬件 SPI 整屏连续刷新长跑（帧周期 + 帧计数） |
| `t_s3_lcdperf` | **帧预算拆解**：绘图 vs 上屏、20/40MHz 对比、分频扫描拟合每块开销 |
| `t_s3_lcddma` | **GDMA 通路自环验证**：MISO←MOSI 比对字节、描述符链、CPU 预灌对照 |
| `t_s3_dmafill` | 整屏/多拍 DMA 范围测试（横向色带，一眼看出截断在第几拍） |
| `t_s3_hello` | 纯串口心跳（单独验证控制台会不会在没有主机读时卡住） |

## 6. 复现

```sh
make C6_ROOT=/home/winger/c6 BUILD=build/s3demo S3_MAIN=tests/t_s3_lcd_demo.c6 s3
P=$(ls /dev/ttyACM* | head -1)
sudo -n esptool --chip esp32s3 --port $P --before default-reset --after no-reset \
  write-flash 0x0 build/s3demo/c6os.bin
sudo -n esptool --chip esp32s3 --port $P --before default-reset --after watchdog-reset read-mac
sudo -n python3 tools/serial.py -p $P -t 10
```

注意：本板复位后 `/dev/ttyACM*` 可能从 0 变成 1，用 `ls /dev/ttyACM*` 取当前设备。
