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

## 4.5 SPI2 的 SCLK 输出打不出边沿（未解，现用 bit-bang 兜底）

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
0..127 号输出信号（只有 103 会翻转）、`SLAVE_REG.clk_mode_13`（{CPOL,CPHA} 支持位）
与 `CK_IDLE_EDGE`/`CK_OUT_EDGE` 的 6 种组合（SCLK 仍 0 边沿、MOSI 始终 256）。**下一步**：对照 IDF `spi_bus_initialize`
的完整初始化序列（尤其 `SPI_SLAVE_REG`/`SPI_CTRL` 里可能另有门控位），或换
SPI3/LCD_CAM 的 i80 通路。

### bit-bang 兜底通路（当前默认演示用）

`s3_lcd_use_bitbang(1)` 把 SCLK/SDA/CS 从 GPIO 矩阵摘回普通 GPIO，用
`GPIO_OUT_W1TS/W1TC` 按 mode 3 手动打时钟。自检：64 字节写出 **512 个 SCLK
上升沿**（PCNT 计数，正好 64×8）；整屏 240×240 填色 **5.16M cycles ≈ 64ms
≈ 15fps**（80MHz 折算），**已经过 M7-3 的 ≥10fps 线**，而且和"硬件 SPI"
路径实测耗时几乎一样——反过来印证硬件路径其实没在打时钟。

注意两点（都踩过）：
- `s3_lcd_init()` 里的引脚配置要沿用 `g_bitbang` 状态，否则 init 序列会被
  重新接回 SPI 矩阵、后面 GPIO 写不到 pad 上（表现为边沿数 0）。
- 本工具链 **u64 除法 ABI 不匹配**，测试里 `(u64)/1000` 会打出
  `TRAP cause=0`（IllegalInstruction）；先转 u32 再除。

## 5. 当前状态

- `make C6_ROOT=... BUILD=build/s3lcd S3_MAIN=tests/t_s3_lcd.c6 s3` →
  `fill_cycles=5159894`（≈64ms，fps10=155）、`blit_cycles=5808687`（≈73ms，
  fps10=137）、`PASS: s3 lcd`。按 CPU 80MHz 折算整屏刷新 **≈14fps**（过 ≥10fps）。
  其中约 1/4 时间花在 CPU 喂 FIFO（64 字节一次）上，换 DMA 还能再快。
- `tests/t_s3_lcd_demo.c6`：红 → 绿 → 8 色竖条 + `C6OS` 文字 → 蓝，各 1.5s
  循环，用来肉眼确认成色顺序、方向、窗口寻址（无人值守也能判）。
- 待确认：面板实际成像（需要人眼）。初始化/整屏填色/帧缓冲 blit 的软件
  路径已跑通，串口数据（周期数、fps）可复现。

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
