# 任务板（M0–M8）

> 状态列：`未开始` / `进行中` / `完成` / `阻塞`
> 规则：每个里程碑一个提交；验收数字写进提交信息；P0 未完成不进入 M0。

---

## P0 准备（施工前置）

| 编号 | 任务 | 产出 | 验收 | 状态 |
|---|---|---|---|---|
| P0-1 | 安装 Xtensa 工具链 | `~/.c6os/toolchain/xtensa-esp-elf`（16.1.0） | `xtensa-esp32s3-elf-gcc --version`；`-mabi=call0` 可编（`-mcpu=esp32s3`/`-msoft-float` 均不支持） | 完成 |
| P0-2 | esptool | 5.6.0（系统 pip） | `esptool version`；能连 `/dev/ttyACM0` | 完成 |
| P0-3 | 硬件实测 | README「已知硬件」 | chip-id / flash-id / PSRAM 8MB / MAC | 完成 |
| P0-4 | 原厂 flash 备份 | `build/backup/flash-16MB.bin` + sha256 | 读回 16MB，可回滚 | 完成（sha256 `fa1a3fcb…8dfe`） |
| P0-5 | 屏型号/引脚确认 | `boards/xiaozhi-s3-spi.h` | RDDID 或卖家资料确认 IC + 引脚 | 阻塞（待板卡资料） |
| P0-6 | 仓库骨架 + 设计文档 | README + docs/5 篇 | 文档评审通过 | 完成 |

> P0-4 必须在任何 `write-flash` 之前执行。

---

## M0 骨架（启动/控制台/panic/idle）

| 任务 | 细节 | 验收 | 状态 |
|---|---|---|---|
| M0-1 `port/s3/start.S` | 关 WDT、设 SP、清 bss、跳 `port_init` | S3-1：串口打印 `c6os s3 boot` | 进行中（镜像已被 ROM 接受，banner 未出） |
| M0-2 `port/s3/memmap.h` | IRAM `0x40370000+448K`、DRAM `0x3FC88000+480K`、栈顶 | S3-2：打印段用量/栈顶/`sizeof(TCB)` | 完成（link.ld + image-info 段地址核对） |
| M0-3 控制台 | USB-Serial/JTAG FIFO 轮询写；UART0 备选 | 同一根 USB 线可输入输出 | 进行中（FIFO 写已实现，无输出） |
| M0-4 `kernel/panic.c6` | 陷阱帧打印（pc/ps/a0–a15/任务名）、宕机循环 | 故意 `os_panic` 能打印完整帧 | 进行中（骨架已提交，陷阱帧未实现） |
| M0-5 `kernel/log.c6` | 4KB 环形日志、`kprintf` 最小格式化 | `%d/%u/%x/%s/%p/%llu` 正确 | 未开始 |
| M0-6 空闲任务 + tick 空转 | `port_idle`（`waiti 0`）、CCOMPARE0 100Hz | S3-3：1 秒 100 tick（±1%） | 未开始 |
| M0-7 参考机 | sim/x86/rv32 三个 port 的 `port_init` 最小实现 | `make check-all` 绿 | 进行中（host smoke 已 PASS；rv32/x86 待接） |

**M0 验收**：三参考目标打印 banner；S3 上串口有 banner + tick 计数；`make s3` size 在预算内。

### M0 实测记录（2026-11）

- `esptool elf2image` 普通头（带 SHA256）烧到 `0x0` → ROM 报
  `Invalid image block, can't boot.`（`ets_main.c 329`）。
- 改用 `--ram-only-header --flash-freq 40m` 后 ROM 接受镜像：只打印
  `ESP-ROM:esp32s3-20210327` 后静默，说明已跳进 `_start`，但控制台未出字。
- 原厂 16MB flash 已备份到 `build/s3/backup/flash-16MB.bin`
  （sha256 `fa1a3fcb…8dfe`，build/ 被 git 忽略）。
- 下一步：核对 USB-Serial/JTAG 时钟/PHY（`SYSTEM_PERIP_CLK_EN1` bit10、
  `CONF0` bit14）与主机重枚举；必要时用 ROM `ets_printf` 做对照。
- 根因（镜像）：esptool 生成的第三个段 `0x403c927e` 落在第二段
  （`0x403c9000+0x280=0x403c9280`）内部，**段重叠** → ROM 判
  `Invalid image block`。已把 `.rodata` 合并进 `.text`，镜像变为 2 段
  （DRAM + IRAM），待复测。
- 当前：旧镜像使 ROM 在 `ets_main.c` 死循环并每 ~300ms 被 TG0WDT 复位，
  USB 反复重枚举、`/dev/ttyACM0` 消失；需按住 BOOT 再复位进入下载模式后重烧。
- host 参考机冒烟已通过：`c6 --emit-llvm` → `clang/lld` + `libtcc1` + `port/sim`，
  输出 `c6os host smoke` / `PASS`（`make check-host`）。

---

## M1 任务与调度

| 任务 | 细节 | 验收 |
|---|---|---|
| M1-1 TCB/对象池 | `struct OsTask`、16 槽静态池、magic 校验 | 创建/删除 1000 次无泄漏 |
| M1-2 就绪位图 | `u32 ready_bit` + 32 环形链、`clz` 选最高 | 位图单测 32 组 |
| M1-3 创建/删除 | `os_task_create/delete`；自删除进 reap 链 | 自删除任务栈延迟回收 |
| M1-4 挂起/恢复/延时 | `suspend_cnt`、`delay` 入超时队列 | 挂起计数嵌套正确 |
| M1-5 时间片 | 10 tick 轮转，同优先级 FIFO | 两任务交替计数 1:1 |
| M1-6 栈哨兵 | 栈底 `0xC6C0DEAD`、每次切换检查 | 溢出必 panic 且打印任务名 |
| M1-7 join | `os_task_join(timeout)` | 先 join 后退出、超时、删除后 join 三种路径 |
| M1-8 上下文切换 | S3 陷阱帧 a0–a15/pc/ps/sar/lbeg/lend/lcount/f0–f15/fcr/fsr | S3-4：两任务交替打印 1000 次无错 |

**M1 验收**：≥15 条断言；S3 上两任务交替；x86 裸机 1000 次切换寄存器一致。

---

## M2 tick 与软定时器

| 任务 | 细节 | 验收 |
|---|---|---|
| M2-1 tick 计数/换算 | `u64` tick、ms↔tick 向上取整 | 边界（0/1/1000ms）正确 |
| M2-2 超时队列 | 按 deadline 有序插入/摘除 | 乱序插入 100 个，出队有序 |
| M2-3 软定时器 | 一次/周期、启动/停止/删除 | 周期计数误差 ≤1 tick/100 次 |
| M2-4 抖动测量 | `CCOUNT` 记录 ISR 进出 | 打印 min/max/P99 |
| M2-5 虚拟时钟 | host/sim `os_test_tick(n)` | L1 无 wall-clock 依赖 |

**M2 验收**：S3 上 100Hz 实测 ±1%；软定时器 100 次周期无漂移累积。

---

## M3 同步

| 任务 | 细节 | 验收 |
|---|---|---|
| M3-1 信号量 | 计数、FIFO、超时、ISR post | 3 等待者按优先级唤醒 |
| M3-2 互斥量 | 递归、所有权移交、ISR 禁 pend | 递归 3 层解锁正确 |
| M3-3 优先级继承 | `max(base, 等待者)`、链式重算 | 3 任务传递链反转对照通过 |
| M3-4 事件 | 32 位、AND/OR、清除、读后清 | 4 组组合断言 |
| M3-5 等待节点双归属 | 对象 pend 链 + 超时队列原子摘除 | post/超时/删除三方竞争 10^5 次无双重入队 |

**M3 验收**：反转对照组（无 PI vs 有 PI）延迟差 ≥5×；超时路径无泄漏。

---

## M4 IPC 与内存

| 任务 | 细节 | 验收 |
|---|---|---|
| M4-1 `Queue@T` | 泛型队列、元素拷贝、满/空/超时 | 元素为结构体/指针/u64 三类 |
| M4-2 兼容队列 | `LOS_QueueCreate/Read/Write` | 与原生同一断言集 |
| M4-3 定长内存池 | `membox`、O(1) 分配、归属校验 | 耗尽返回 `OS_ENOMEM` |
| M4-4 best-fit 堆 | 块头/合并/对齐/统计 | 1000 次随机分配释放无碎片泄漏 |
| M4-5 统计 | 已用/峰值/最大空闲块 | 与手算一致 |

**M4 验收**：队列三类元素；堆 1000 次随机操作后 `used==0`；OOM 路径可恢复。

---

## M5 双 API

| 任务 | 细节 | 验收 |
|---|---|---|
| M5-1 `include/los/*.h` | 类型/宏/原型与 LiteOS-M 对齐 | 用 LiteOS 示例头可编译 |
| M5-2 `compat/los_*.c6` | `LOS_Task/Sem/Mux/Event/Queue/Membox/Mem/Tick/Swtmr` | 每个对象两条路径结果一致 |
| M5-3 错误码映射 | `OS_*` → `LOS_*` | 映射表 100% 覆盖 |
| M5-4 一致性测试 | 同一断言集跑原生/兼容 | 输出逐行一致 |

**M5 验收**：`make check-host API=both` 双份全绿；LiteOS 风格 demo 可跑。

---

## M6 中断与 HWI

| 任务 | 细节 | 验收 |
|---|---|---|
| M6-1 向量表 | `HwiCreate/Delete`、优先级、使能 | 3 个外设中断共存 |
| M6-2 下半部 | ISR → 任务唤醒延迟 | S3-5：延迟 < 100µs |
| M6-3 嵌套/关中断 | `port_irq_save/restore` 嵌套计数 | 嵌套 5 层恢复正确 |
| M6-4 ISR 安全 API | post/set/put/resume 白名单 | ISR 里误用阻塞 API 必 panic |

**M6 验收**：ISR 唤醒任务延迟实测；嵌套中断不丢 tick。

---

## M7 shell 与屏

| 任务 | 细节 | 验收 |
|---|---|---|
| M7-1 shell | COBS 帧 + 命令表（ps/free/tick/date/log/reboot） | 每条命令输出正确 |
| M7-2 面板探测 | RDDID 识别 ST7789/ST77916，profile 兜底 | 两种面板都能点亮 |
| M7-3 帧缓冲/字体 | RGB565、8×16 字体、`fill_rect/blit` | 240×280 全屏刷新率 ≥10fps |
| M7-4 仪表盘 | 任务表/CPU/内存/日志滚动 | 屏上数字与串口一致 |

**M7 验收**：屏显示与串口数据一致；shell 可交互。

---

## M8 收尾

| 任务 | 细节 | 验收 |
|---|---|---|
| M8-1 压力/长稳 | 随机创建/删除/超时 10^6 次 | 10 分钟无泄漏、无 panic |
| M8-2 双核 | 第二核启动、每核就绪队列、IPI | 两核各自跑任务，负载均衡可观测 |
| M8-3 FPU 优化（可选） | 惰性保存 FPU 帧；全镜像统一 ABI | 浮点任务切换上下文一致 |
| M8-4 PSRAM（可选） | 8MB 初始化、帧缓冲迁移 | 大缓冲分配成功 |
| M8-5 上板文档 | 烧写/回滚/量产镜像 | 新人按文档可复现 |

**M8 验收**：10 分钟长稳 + 双核 demo + 文档复现。

---

## 风险与依赖

| 风险 | 影响 | 缓解 |
|---|---|---|
| FPU 无软浮点开关（实测） | 切换帧变大、开销增加 | 陷阱帧固定保存 f0–f15/FCR/FSR；M8 可做惰性保存 |
| 屏型号/引脚未知 | M7 阻塞 | `make s3-info` 清单；RDDID 探测；找卖家资料 |
| USB-Serial/JTAG 控制台寄存器细节 | M0 控制台 | 以 IDF `usb_serial_jtag_ll.h` 为准；UART0 兜底 |
| ROM 保留 DRAM 区域 | 启动即崩 | 以 `soc.h` 边界为准，栈放 DRAM 顶并留 4KB |
| 无 qemu-system-xtensa | S3 只能真板验证 | L1–L3 覆盖逻辑；上板阶梯逐步验收 |
| 原厂 flash 被覆盖 | 无法恢复 | P0-4 先备份，写进 `make flash` 前置检查 |
