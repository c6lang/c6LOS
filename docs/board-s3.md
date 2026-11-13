# ESP32-S3 真板操作手册（c6os）

适用板卡：ESP32-S3（QFN56 v0.2，双核 Xtensa LX7，8MB PSRAM，16MB flash，
40MHz 晶振），USB-Serial/JTAG 直连（`303a:1001` → `/dev/ttyACM0`）。

## 1. 工具链

```sh
export C6=<c6 仓库根>
TOOL=~/.c6os/toolchain/xtensa-esp-elf/bin
$TOOL/xtensa-esp32s3-elf-gcc --version     # gcc 16.x
esptool version                            # 5.x
```

## 2. 构建

```sh
cd /home/winger/rtos-c6
make C6_ROOT=$C6 BUILD=build/s3irq S3_MAIN=tests/t_s3_irq.c6 s3
# 产物：build/s3irq/c6os.bin（esptool elf2image --ram-only-header）
```

`S3_MAIN` 必须是带 `void os_main(void)` 的 c6 文件；`S3_EXTRA_C6` 追加内核
源文件（默认 task/tick/timer/timeout/heap；IPC 用例追加 sem/mutex/event/queue/membox）。

## 3. 烧写（每轮流程）

板子按 **BOOT → RESET → 松开 BOOT** 进下载模式，然后：

```sh
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 \
  --before usb-reset --after no-reset write-flash 0x0 build/s3irq/c6os.bin

# 复位并让 USB 重新枚举（不要用普通 RESET，USB 会掉线）
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 \
  --before default-reset --after hard-reset read-mac
```

- `--before usb-reset` 在本板比 `no-reset` 可靠；`--after hard-reset` 后
  `/dev/ttyACM0` 会以新设备号重新出现，等 1~2 秒再抓串口。
- 镜像烧在 **0x0**，会覆盖原厂 bootloader；没有完整 flash 备份时不要指望
  “回原厂”，需重新下载官方固件包整片烧写。

## 4. 串口

115200 8N1，`winger` 不在 `dialout`，命令加 `sudo -n`。
最小抓取脚本（DTR/RTS + 非阻塞读）见仓库历史提交；要点：

```python
termios.tcsetattr(fd, TCSANOW, 115200 8N1 raw)
fcntl.ioctl(fd, 0x5416, struct.pack('i', 0x006))   # DTR|RTS
```

启动通常打印两次 `c6os s3 boot`（USB 重新枚举/复位），随后才是测试 banner。

## 5. 已接外设/中断

- 控制台：USB-Serial/JTAG `EP1_REG` 轮询写；`port_init` 重新抢回 PHY、
  使能时钟/PAD，等待枚举。
- 定时器：TIMG0 看门狗在时钟使能后关闭；`CCOMPARE0 = CCOUNT + 400000`
  （100Hz），level-1 中断走 IRAM 向量表 `+0x300`。
- VECBASE：先清 `PMS_CORE_0_VECBASE_OVERRIDE`（0x600C11C0 低 2 位），
  并清 `PS.UM`（ROM 入口是 user mode），否则 level-1 走 0x340 用户向量。
- 陷阱：`EXCCAUSE != 4` 走 `s3_trap` 打印 cause/pc/ps 后停机。
- 输入：`port_getc()` 用 **32 位**读 `EP1_REG`（8 位读恒为 0），按
  `OUT_EP1_ST[22:16]` 批次计数出队，读完清 `INT_CLR` bit2；真板 shell
  `help/tick/ps/free` 已全部正常。
- **GPIO ≥32 的寄存器分组**（踩过）：`GPIO_OUT`(0x04)/`GPIO_ENABLE`(0x20)/`GPIO_IN`(0x3C)
  只覆盖 GPIO0..31；GPIO32..53 要用 **`GPIO_OUT1`(0x60004010)、
  `GPIO_ENABLE1`(0x60004030)、`GPIO_IN1`(0x60004040)**。之前用 `GPIO_IN >> 39`
  读音量键是移位越界（未定义行为），读出来恒 0，白白排了很久。
- **IO_MUX 不是全线性**：GPIO0..21 是 `0x60009000+0x04+4p`，但 GPIO39/40 是
  JTAG 的 MTCK/MTDO 专用寄存器（`0x600090A0/0xA4`），GPIO33..37 又是另一组。
- 核 ID：**`rsr.prid` 在这颗 S3 上读回来的不是 0/1**（实测 core0=0xCDCD、
  core1=0xABAB，每核稳定但都是垃圾值），所以 `port_core_id()` 不能直接
  `prid & 1`。做法是 `port_core_bind_self()`：每个核在入口（core0 在
  `port_init`、core1 在 `core1_entry`）把自己的 PRID 值登记进 `g_prid_base[]`，
  之后按值查表返回下标。注意 `os_sched_tick()` 里有「非 0 号核直接返回」的
  SMP 保护，核 ID 错了会表现为 **tick 永不递增**（很像定时器坏了）。
- RTC 慢速内存（0x50000000）mark 在这块板子上**不可靠**（2/3/7 号字读到过
  垃圾），诊断优先用串口打印。
- 看门狗：除 RTC_WDT（`0x60008098=0`）外，还必须关 **RTC 超级看门狗 SWD**
  （`0x600080B8=0x8F1D312A` → `0x600080B4 |= 1<<30` → `0x600080B8=0`），
  否则每 ~3.5s 被整片复位，串口只出几行（复位原因 0x12 = SYS_SUPER_WDT）。

## 6. 常用验证

```sh
make ... S3_MAIN=tests/t_s3_irq.c6 s3       # PASS: s3 irq tick
make ... S3_MAIN=tests/t_s3_preempt.c6 s3   # PASS: s3 preempt
make ... S3_MAIN=tests/t_s3_jitter.c6 s3    # PASS: s3 jitter
make ... S3_MAIN=tests/t_s3_ipc.c6 s3       # ALL PASS: ipc
```

## 7. 故障排查

| 现象 | 处理 |
| --- | --- |
| 串口 0 字节 | 确认已 `hard-reset` 并等枚举；DTR/RTS 置位；检查是否烧到 0x0 |
| `SHA-256 comparison failed ... boot anyway` | `--ram-only-header` 的正常提示，可忽略 |
| 复位后掉 USB | 用 `--before usb-reset` / 拔插 USB；本板普通 RESET 不恢复 |
| 启动约 300ms 被复位 | TIMG0 时钟未使能就关看门狗；确认先置 `SYSTEM_PERIP_CLK_EN0` bit13 |
| 中断不触发 | 查 VECBASE override、PS.UM、INTENABLE bit6、CCOMPARE0 重装 |
| 面板初始化后整机停摆（串口静默、连中断计数也停） | 先查板级电源保持脚 GPIO21（原厂启动即拉高），再对齐 SPI mode3/80MHz，见 §8 |
| SPI2 写 `CMD.USR` 后读回是 0、面板一个字节都收不到 | 没置 `SPI_CLK_GATE`（0xE8）的 **`CLK_EN`(bit0)**/`MST_CLK_ACTIVE`/`MST_CLK_SEL`，模块时钟被门控 |
| 串口只出几行就静默、USB 每隔几秒重新枚举 | RTC 超级看门狗未关（读 `0x60008038 & 0x3F`，`0x12`=SYS_SUPER_WDT），见 §5 |
| 加了 `volatile` 忙等之后 CPU 像死机 | GCC 给 volatile 访存插 `memw`，SPI 事务后未回的 APB 事务会让它永久等待；驱动内延时用 `nop` 循环 |

原厂 16MB flash 备份已丢失；恢复原厂需官方固件包整片烧写（见 docs/todo.md）。

## 8. ST7789 面板（240×240，M7）

面板驱动在 `port/s3/panel.c`，完整的寄存器配方、自检手段与踩坑记录见
**`docs/s3-panel-bringup.md`**。原厂固件出处与完整板级配置见
`docs/s3-board-identification.md`「上游固件出处」——上游是开源 XiaoZhi
（78/xiaozhi-esp32）v2.5.0 的 `main/boards/nologo/xingzhi-cube-1.54tft-wifi`。

| 项 | 值 |
| --- | --- |
| 面板 | ST7789 240×240，RGB565，8 位命令/参数，`INVON`(0x21) 必须发 |
| 引脚 | SCLK=GPIO9，MOSI/SDA=GPIO10，DC=GPIO8，CS=GPIO14，RST=GPIO18，BL=GPIO13 |
| 上游 SPI | `SPI3_HOST`，**mode 3**，**80MHz**，DMA，单次 ≤115200B（整帧） |
| 本仓库 SPI | GPSPI2（`0x60024000`）CPU 直写 `W0..W15`，20MHz，无 DMA |
| 板级电源 | **GPIO21** 需拉高（原厂用 RTC GPIO，关机时拉 0 再 deep sleep） |

硬件 SPI2 的三个硬性前提（踩过的坑）：

1. `SPI_CLK_GATE`（`0xE8`）必须写 `MST_CLK_ACTIVE(bit1)|MST_CLK_SEL(bit2)=1`，
   否则主控状态机没有时钟，`CMD.USR` 永远不自清（现象：串口停在写 USR 之后）。
2. 启动一次事务的顺序是 `MS_DLEN → CMD.UPDATE → 等 UPDATE 自清 → CMD.USR`，
   UPDATE 和 USR 连着写会丢配置同步。
3. `SYSTEM_PERIP_CLK_EN0` bit6 使能 + bit6 复位释放之后，先清 `DMA_INT_CLR`、
   再写 `DMA_INT_ENA=0`；本内核的 level-1 向量只按 `EXCCAUSE==4` 分流，其它
   中断上来会被当成定时器处理并反复重入。

