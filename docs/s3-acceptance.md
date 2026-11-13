# S3 真板验收对照表（本轮收工快照）

板卡：ESP32-S3（QFN56 v0.2，MAC `cc:8d:a2:ec:99:c4`），烧写/串口见
`docs/board-s3.md`。每一项都在真板上跑过，下面是"跑什么 → 看到什么"。

| 待办项 | 用例 | 真板实测输出 | 状态 |
| --- | --- | --- | --- |
| P0-5 屏型号确认 | —（查上游固件） | ST7789 240×240 SPI；上游 `78/xiaozhi-esp32` v2.5.0 的 `xingzhi-cube-1.54tft-wifi` 板级配置，见 `docs/s3-board-identification.md` | ✅ |
| M7-2 面板点亮 | `tests/t_s3_lcd_kiosk.c6` | `init done gate=7` 后循环 `1 red 5168k-cyc / 2 green / 3 blue / 4 bars 5814k-cyc / 5 dash` | 驱动✅，成像待肉眼确认 |
| M7-3 帧缓冲/字体上屏 | `tests/t_s3_lcd.c6` | `fb_bytes=115200 DRAM OK`、`fill_cycles=5168839 fps10=154`、`blit_cycles=5814672 fps10=137`、**`PASS: s3 lcd`**（≈14fps ≥ 10fps） | ✅ |
| M7-4 仪表盘 | `tests/t_s3_dash.c6` | `manual_tick=7 core=0`、`first_frame_cycles=6707529`、`tick=128/155/181/207…` `tasks=3` | ✅ |
| M8-2 双核 SMP | `tests/t_s3_core1.c6`<br>`tests/t_s3_smp.c6`<br>`tests/t_s3_smp_sem.c6` | `core0 prid_raw=52685 core_id=0` / `core1 count=344 prid=1` / **`PASS: core1`**<br>**`PASS: s3 smp core1 scheduler`**<br>**`PASS: s3 smp cross-core sem`** | ✅ |
| S3 shell 输入复测 | `tests/t_s3_shell.c6` | `shell ready` / `shell>`，`help/tick/ps/free` 正常 | ✅ |
| （回归）中断 tick | `tests/t_s3_irq.c6` | **`PASS: s3 irq tick`**（此前一直失败，本轮修好） | ✅ |

## 本轮修掉的真实缺陷

1. **GPIO 矩阵输出没打开输出使能**（只有背光的头号嫌疑）：只写 `OUT_SEL.func_sel`
   没置 `GPIO_ENABLE_REG`，SCLK/MOSI/CS 一直高阻+内部上拉（回读恒 1）。修后
   CS 拉低到 0、MOSI 采样出现 0/1 交替。
2. **RTC 超级看门狗 SWD 未关**：每 ~3.5s 整片复位（复位原因 0x12），表现为
   "串口只出几行 + USB 隔几秒重新枚举"。`port_init` 里关闭。
3. **`volatile` 忙等触发 `memw` 永久等待**：SPI 事务后 APB 事务未回，CPU 当场
   停摆。驱动内延时改 `nop` 循环。
4. **`SPI_CLK_GATE` 少 `CLK_EN`(bit0)**：模块时钟被门控，写 `CMD.USR` 读回是 0、
   不出 SCLK，但轮询"立刻完成"，极像正常。
5. **`SPI_CLOCK` 占空比不成立**（H+L≠N）。
6. **`port_core_id()` 一直是错的**：`rsr.prid` 在这颗 S3 上是垃圾值
   （core0=0xCDCD、core1=0xABAB），恒等于 1 → `os_sched_tick()` 的"非 0 号核
   直接返回"永远命中 → **tick 永不前进**。改为启动时按核登记 PRID 查表。
7. **`ctx.S` 双核共用 `g_sched[0]`** + 保存现场与算核号的顺序问题（先动寄存器会
   毁掉被打断任务的上下文，表现为随机 `TRAP cause=1c`）；且切换路径不能
   `callx0` 进 C（windowed 风格约定会冲掉 a12-a15）。
8. **ST7789 初始化延时不足**：`dly()` 实测 5 cycles/单位，`dly(1200000)` 只有
   75ms，而手册要求 SWRESET/SLPOUT 后各 ≥120ms；已按标定值改成 187ms，并补齐
   porch/gate/VCOM/gamma 初始化表。

## 仍未知的一件事

**SPI2 的 SCLK 输出在这块板子上打不出边沿**（PCNT 实测：同一毫秒窗口 MOSI 有
256 个上升沿、SCLK 0 个；GPIO9 软件翻转 100 次能数到 99，测量链路正常）。
已排除时钟门控、占空比、SPI mode、时钟源、`CK_DIS`、`clk_mode_13`、以及
0..255 全部输出信号号。因此驱动**默认走 bit-bang**（`g_bitbang=1`，实测 512
边沿/64B、整屏 5168k cycles ≈ 15fps），硬件 SPI 通路保留可用
`s3_lcd_use_bitbang(0)` 复现实验。

面板的物理成像仍只能靠人眼确认；若屏幕仍无图像，可能性收敛为：面板/排线本身、
这块屏 SDA 只做输入（回读全不出值）、或该屏需要本驱动之外的初始化。

## 现场排查用：假设逐个替换（`tests/t_s3_lcd_sweep.c6`）

用户反馈"只有背光、屏是黑的"，因此做一个每 4 秒换一种假设、串口同步报步号的
演示，靠眼睛定位。已核对上游源码：

- 引脚与上游 `config.h` **完全一致**：SDA=10、SCL=9、DC=8、CS=14、RES=18、
  BL=13；240×240；`DISPLAY_SWAP_XY/MIRROR_X/MIRROR_Y` 全 false、offset 0。
- 上游 `xingzhi-cube-1.54tft-wifi.cc` 用的是 `esp_lcd_new_panel_st7789` +
  **`SPI3_HOST`**（所以本仓库驱动也加了 SPI3 支持：基址 `0x60025000`、
  时钟位 bit16、信号 `SPI3_CLK_OUT=66 / SPI3_D_OUT=68 / SPI3_CS0=71`）。
- 上游 `power_manager.h` 只管充电检测，**没有显示电源脚**。

演示步骤（每步 4s，屏幕上应变色/出图案）：

| 步 | 变量 | 画面 |
| --- | --- | --- |
| 1 SLOWBCLK | bit-bang 降到约 1MHz | 整屏红 |
| 2 DCINV | DC 极性取反 | 整屏绿 |
| 3 NOINV | 发 INVOFF(0x20) | 整屏蓝 |
| 4 BGR | MADCTL=0x08（BGR） | 整屏红 |
| 5 ROT | MADCTL=0x60（MX+MV） | 8 色竖条 + 文字 |
| 6 FAST | 恢复全速 + INVON + MADCTL=0 | 整屏白 |

哪一步出现颜色/图案，就说明对应假设成立（例如只有 1 有反应 = 时钟太快；
只有 2 有反应 = DC 反相）。
