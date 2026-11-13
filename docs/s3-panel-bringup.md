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

1. **`SPI_CLK_GATE` 少 bit0（`SPI_CLK_EN`，"enable clk gate"，复位值 0）**
   → 模块时钟被门控：写 `CMD.USR` 读回仍是 0，总线不出时钟，面板一个字节都
   收不到；但轮询"立刻通过"，非常像正常工作。这是最坑的一条。
2. **`volatile` 忙等循环会把 CPU 卡死（memw 内存屏障）**
   GCC 给每个 volatile 访存插 `memw`。SPI 事务之后只要有一条 APB 事务没回，
   下一条 `memw` 就永久等待：表现为串口突然静默、定时器中断也不再来，
   看起来像"整机死机/复位"。修法：面板驱动里的延时用不带 volatile 的
   `nop` 循环（`port/s3/panel.c: dly()`）。
3. **RTC 超级看门狗（SWD）没有关** → ROM/esptool 装了几秒超时，应用不关就
   每 ~3.5 秒整片复位一次：串口只能看到头几行、USB 每隔几秒重新枚举，
   极易误判成"CPU 停摆"。判断方法：读 `RTC_CNTL_RESET_STATE_REG`
   （`0x60008038`）低 6 位，`0x12` = SYS_SUPER_WDT。修法见 `port/s3/console.c`
   `port_init()`：`0x600080B8=0x8F1D312A`（解锁）→ `0x600080B4 |= 1<<30`
   （`SWD_DISABLE`）→ `0x600080B8=0`。
4. **RTC WDT**（`0x60008098` WDTCONFIG0，写保护钥匙 `0x50D83AA1` @ `0x600080B0`）
   同样要清 0。
5. **SPI mode**：原厂 `spi_mode=3`，即 `MISC.CK_IDLE_EDGE=1` +
   `USER.CK_OUT_EDGE=1`；写成 mode 0 面板时序虽然勉强能认，但没有理由不对齐。
6. **CS**：ST7789 串口靠 DC 区分命令/参数，`MISC.CS_KEEP_ACTIVE=1` 让 CS 常低，
   避免命令字节与参数分属两次事务时接口状态机被打断。
7. **DMA/TRANS_DONE 中断**：本内核 level-1 向量只按 `EXCCAUSE==4` 分流
   （分不清定时器和外设），所以 SPI2 中断必须 `DMA_INT_ENA=0` 且每次事务后清
   `DMA_INT_CLR`，否则会被当成定时器中断反复重入。

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
- **SPI 回环**：`s3_spi_loopback()` 把 `FSPIQ` 输入映射到 MOSI 同一根 pad
  做全双工读回（需要 `USER.DOUTDIN=1`）。

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
