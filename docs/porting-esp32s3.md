# ESP32-S3 移植手册（目标机）

> 状态：设计定稿 + 构建链已实测 · 相关：[toolchain.md](toolchain.md) · [architecture.md](architecture.md)
> 实测板：ESP32-S3 QFN56 rev v0.2，8MB PSRAM，16MB flash，1.85″ SPI 屏

---

## 1. 实测硬件

| 项 | 值 | 来源 |
|---|---|---|
| 芯片 | ESP32-S3 (QFN56) revision v0.2 | `esptool chip-id` |
| 核心 | Dual Core Xtensa LX7 + LP core，240MHz | `esptool chip-id` |
| PSRAM | Embedded 8MB (AP_3v3) | `esptool chip-id` |
| Flash | 16MB，quad，3.3V，ef 4018 | `esptool flash-id` |
| 晶振 | 40MHz | `esptool chip-id` |
| USB | `303a:1001` USB JTAG/serial debug unit → `/dev/ttyACM0` | `lsusb` |
| MAC | `cc:8d:a2:ec:99:c4` | `esptool chip-id` |
| 屏 | 1.85″，SPI（IC/引脚待确认） | 产品资料 |

访问串口需要 `dialout` 或 root：本机 `winger` 不在 `dialout`，当前用 `sudo -n` 调 esptool。

---

## 2. 启动与镜像

ESP32-S3 的 ROM 从 flash `0x0` 读取**二级引导器镜像**（esp_image），按段头把各段搬到
目标地址，再跳到镜像头里的 entry。我们**不做** IDF 的 bootloader + 分区表 + app 三段式，
而是把内核本身做成二级引导器：

```
flash 0x0: [esp_image header][segment table][.iram 段][.dram 段][checksum/sha256]
   ROM: 校验 → 按 VMA 搬运 → 跳 entry(_start)
   _start: 关 WDT → 设 SP → 清 bss → VECBASE → port_init → … → os_start
```

- 镜像由 `esptool elf2image` 生成（不要手写头部；`espimg.py` 只做校验/回读解析）。
- **不需要分区表**：我们不加载 app、不解析分区。
- 烧写偏移 `0x0`；回滚用全片备份。
- 进入下载模式：`--before default-reset` 实测可直接连；失败时按住 BOOT 再复位。

```sh
# 烧写前必备份（16MB）
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 read-flash 0 0x1000000 build/backup/flash-16MB.bin
# 烧写
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 build/s3/c6os.bin
# 回滚
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 build/backup/flash-16MB.bin
```

`make flash` 必须先检查备份存在，否则拒绝执行。

---

## 3. 内存映射（ESP32-S3）

来自 IDF `components/soc/esp32s3/include/soc/soc.h`：

| 区域 | 范围 | 大小 | 用途 |
|---|---|---|---|
| IRAM | `0x40370000` – `0x403E0000` | 448KB | 代码/只读数据；内核镜像 |
| DRAM | `0x3FC88000` – `0x3FD00000` | 480KB | 数据/bss/栈/堆 |
| IROM | `0x42000000` – `0x44000000` | 32MB | flash 映射（本设计不用） |
| DROM | `0x3C000000` – `0x3E000000` | 32MB | flash 映射（本设计不用） |
| RTC SRAM | `0x50000000`+ | 8KB | 深睡保留（M8 可选） |

约定：

- `.text/.literal/.rodata` → IRAM；`.data/.bss` → DRAM。
- 栈顶 = `0x3FD00000 - 0x1000`（留 4KB 给 ROM/异常）；向下增长，16B 对齐。
- 启动时打印 `_text_start/_text_end/_data_start/_bss_end/SP`，与 `soc.h` 核对。
- ROM 可能占用 DRAM 低端/高端的小块；首版把内核数据放 `0x3FC88000` 起，
  栈放高端，实测冲突再调整（M0 验收项）。

链接脚本 `port/s3/link.ld`：

```ld
ENTRY(_start)
MEMORY {
  IRAM (rx) : ORIGIN = 0x40370000, LENGTH = 0x70000
  DRAM (rw) : ORIGIN = 0x3FC88000, LENGTH = 0x78000
}
SECTIONS {
  .text : { *(.text*) *(.literal*) } > IRAM
  .rodata : { *(.rodata*) } > IRAM
  .data : { *(.data*) } > DRAM
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > DRAM
}
```

---

## 4. 控制台：USB-Serial/JTAG（默认）与 UART0（备选）

板上只有一根 USB 线，控制台走 **USB-Serial/JTAG**，不额外占引脚，也不需要 USB-TTL。

寄存器（IDF `soc/esp32s3/register/soc/usb_serial_jtag_reg.h`）：

| 寄存器 | 偏移 | 关键位 |
|---|---|---|
| `USB_SERIAL_JTAG_EP1_REG` | `+0x00` | `RDWR_BYTE[7:0]`：写入即压入 TX FIFO / 读出 RX 字节 |
| `USB_SERIAL_JTAG_EP1_CONF_REG` | `+0x04` | `bit1 SERIAL_IN_EP_DATA_FREE`：TX FIFO 未满；`bit0 WR_DONE`：写 1 表示本批写完 |
| `USB_SERIAL_JTAG_INT_RAW_REG` | `+0x08` | `bit3 SERIAL_IN_EMPTY_INT`：TX FIFO 空（可再写） |
| `USB_SERIAL_JTAG_INT_CLR_REG` | `+0x14` | 写 1 清中断 |

基址：`0x60038000`（ESP32-S3 TRM；施工时用 IDF `reg_base.h`/`soc.h` 再核对一次）。

`putc` 轮询流程：

```
while (!(EP1_CONF & (1<<1))) ;     // 等 TX FIFO 有空间
EP1_REG = ch;                      // 压入一个字节
EP1_CONF = 1;                      // WR_DONE
```

- 输出只在任务/panic 上下文轮询，ISR 里不打印（除 panic）。
- 读入（shell）：查 `EP1_CONF bit2 SERIAL_OUT_EP_DATA_AVAIL`，读 `EP1_REG`。
- 备选 UART0：GPIO43(TX)/GPIO44(RX)，115200 8N1；`CONFIG_CONSOLE=uart0` 切换。
- 注意：ROM 下载也走这个 USB 口；烧写时不要同时打印（复位后由 ROM 接管）。

---

## 5. 时钟与 tick

- 复位后 CPU 跑 40MHz 晶振；M0 先不配 PLL（40MHz 足够跑 tick 与调度）。
- 定时器：Xtensa `CCOMPARE0` 特殊寄存器，比较中断（level 1）。
  - 100Hz @40MHz：`CCOMPARE0 += 400000`；
  - ISR 里先写回下一次 `CCOMPARE0`（清中断），再 `os_tick_irq()`；
  - 读当前计数用 `CCOUNT`（`port_cycles()`）。
- 240MHz 在 M8 前不做；切换时重新算 tick 常量。
- 看门狗：启动即关 TIMG0 WDT 与 RTC WDT；否则约 300ms 复位。

---

## 6. 中断与异常

- `VECBASE` 指向向量表；每个向量 64B，放跳板 `j common_handler`。
- 公共入口（`.S`）保存：

```
a0–a15, pc, ps, sar, lbeg, lend, lcount,
f0–f15, fcr, fsr        # S3 默认硬浮点，必须保存
```

- 返回用 `rfi <level>`；中断级别与 `PS.INTLEVEL` 在 `port_irq_save/restore` 里维护。
- C 侧只暴露 `os_tick_irq()` / `os_sched_irq(sp)`；`port` 不碰内核对象。
- 首版只开 level 1 定时器中断；外设在 M6 接入 `HwiCreate`。

> FPU：实测 `xtensa-esp32s3-elf-gcc` **没有** `-msoft-float`/`-mno-fpu` 开关，
> `float` 直接生成 `add.s f0` 等指令。因此陷阱帧必须包含 f0–f15 + FCR/FSR；
> 内核核心自身仍禁止浮点（lint），浮点只出现在应用任务。

---

## 7. 双核（M8）

- Core0 启动后把 `_start` 地址写入 `0x600D0000`?（S3 的 CPU 启动寄存器以 TRM 为准），
  Core1 由 ROM/我们自己拉起的流程在 M8 实现。
- 每核一份 `cur` 与就绪位图；跨核唤醒用 IPI（软件中断）。
- M0–M7 只用 Core0；Core1 停在 `waiti 0`。

---

## 8. 显示（1.85″ SPI）

- 面板候选：
  - `ST7789` 240×280（1.85″ 最常见）；
  - `ST77916` 360×360 圆屏（部分 1.85″ 圆屏）。
- 探测：SPI 读 `RDDID (0x04)`，按 ID 选驱动；失败用 `boards/xiaozhi-s3-spi.h` 的默认值。
- 引脚：待板卡资料确认（CS/DC/RST/SCLK/MOSI/BL）。`make s3-info` 打印当前 profile。
- 帧缓冲：RGB565；240×280 = 131KB 放内部 SRAM；360×360 = 253KB 需要 PSRAM（M8）。
- 刷新：SPI 40MHz，DMA 可选；首版轮询写，10fps 即可。

---

## 9. 上板阶梯

| 步 | 动作 | 观察 |
|---|---|---|
| S3-0 | 备份 16MB flash | sha256 记录 |
| S3-1 | 最小镜像（`_start` 写 USB FIFO） | 串口出现 `c6os s3 boot` |
| S3-2 | 段/栈/bss 自检 | 打印 IRAM/DRAM 用量、SP、`sizeof(TCB)` |
| S3-3 | CCOMPARE0 tick | 1 秒 100 tick |
| S3-4 | 两任务切换 | 交替计数 1000 次 |
| S3-5 | 抢占 | 高优先级唤醒延迟 <100µs |
| S3-6 | 同步对象断言集 | `42 test(s), 0 failure(s)` |
| S3-7 | 屏 | 填充/文字/仪表盘 |
| S3-8 | 长稳 | 10 分钟无泄漏 |

---

## 10. 已实测的构建链（施工基线）

```sh
C6=/path/to/c6
$C6/tcc/c6 -B $C6/tcc -c99 -o hello.c hello.c6
xtensa-esp32s3-elf-gcc -mabi=call0 -Os -ffreestanding -fno-builtin -nostdlib \
  -mlongcalls -mtext-section-literals -c hello.c -o hello.o
xtensa-esp32s3-elf-ld -T link.ld -o hello.elf hello.o
esptool --chip esp32s3 elf2image --flash-mode dio --flash-size 16MB --flash-freq 80m \
  -o hello.bin hello.elf
```

实测结果（2026-11）：

- `hello.elf`：`Machine: Tensilica Xtensa Processor`，entry `0x40370004`，text 20B；
- `hello.bin`：96B，头部 `e9 01 02 4f 04 00 37 40 …`（magic `0xE9`，entry `0x40370004`）；
- `-mabi=call0` 生成 `call0`/`ret.n`（默认 windowed 生成 `call8`/`retw.n`）；
- `-mcpu=esp32s3` **不可用**：用 `xtensa-esp32s3-elf-gcc` 前缀选核；
- `-msoft-float` **不可用**：S3 默认硬浮点。

---

## 11. 风险与未决

| 项 | 现状 | 计划 |
|---|---|---|
| 屏 IC/引脚 | 未知 | `make s3-info` + RDDID + 卖家资料 |
| ROM 保留 DRAM | 未实测 | S3-2 打印后调整栈顶 |
| USB JTAG 基址 | 按 TRM `0x60038000` | 用 IDF 头核对后写死 |
| 无 qemu-system-xtensa | 只能真板 | L1–L3 覆盖逻辑 |
| FPU 帧 | 必须保存 16 个 f 寄存器 | M1 实现并做浮点任务切换测试 |
| PSRAM | 8MB 未初始化 | M8 可选；首版不用 |
