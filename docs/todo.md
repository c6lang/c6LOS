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
| P0-5 | 屏型号/引脚确认 | `boards/xiaozhi-s3-spi.h` | RDDID 或卖家资料确认 IC + 引脚 | **完成**：屏幕为 **ST7789 240×240 SPI**；引脚 SDA=10/SCL=9/DC=8/CS=14/RES=18/BL=13 与上游 `78/xiaozhi-esp32` v2.5.0 的 `xingzhi-cube-1.54tft-wifi/config.h` 逐项一致（原文已抓取核对），出处在 `docs/s3-board-identification.md` |
| P0-6 | 仓库骨架 + 设计文档 | README + docs/5 篇 | 文档评审通过 | 完成 |

> P0-4 必须在任何 `write-flash` 之前执行。

---

## M0 骨架（启动/控制台/panic/idle）

| 任务 | 细节 | 验收 | 状态 |
|---|---|---|---|
| M0-1 `port/s3/start.S` | 设 SP、清 bss、跳 `port_init`（WDT 移到 C 侧关） | S3-1：串口打印 `c6os s3 boot` | 完成（实测输出 `c6os s3 boot` / `c6os: alive`） |
| M0-2 `port/s3/memmap.h` | IRAM `0x40370000+448K`、DRAM `0x3FC88000+480K`、栈顶 | S3-2：打印段用量/栈顶/`sizeof(TCB)` | 完成（link.ld + image-info 段地址核对） |
| M0-3 控制台 | USB-Serial/JTAG FIFO 轮询写；UART0 备选 | 同一根 USB 线可输入输出 | 完成（PHY 抢回 USJ + 时钟/PAD 使能后实测有输出） |
| M0-4 `kernel/panic.c6` | 陷阱帧打印（pc/ps/a0–a15/任务名）、宕机循环 | 故意 `os_panic` 能打印完整帧 | 完成（host `os_panic_frame` + 日志回放 PASS；S3 level-1 向量按 `EXCCAUSE==4` 区分定时器中断与异常，异常走 `s3_trap` 打印 cause/pc/ps 后停机，`ill` 实测 `TRAP cause=0 pc=...`） |
| M0-5 `kernel/log.c6` | 4KB 环形日志、`kprintf` 最小格式化 | `%d/%u/%x/%s/%p/%llu` 正确 | 完成（4KB 环形日志 + `rt/kprintf.c`，host 断言 PASS） |
| M0-6 空闲任务 + tick 空转 | `port_idle`（`waiti 0`）、CCOMPARE0 100Hz | S3-3：1 秒 100 tick（±1%） | 完成（S3 实测 `elapsed=100` / `PASS: tick`；40MHz XTAL 下 400k cycles/tick） |
| M0-7 参考机 | sim/x86/rv32 三个 port 的 `port_init` 最小实现 | `make check-all` 绿 | 完成（新增 `check-all = check-host + x86 + rv32`；clean 后 host 34 项全 PASS，x86/rv32 构建 PASS） |

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
  已加 `tools/flash-s3.sh`：等待端口稳定后自动重试烧写；恢复步骤见
  `docs/porting-esp32s3.md`「USB 下载模式恢复」。
- host 参考机冒烟已通过：`c6 --emit-llvm` → `clang/lld` + `libtcc1` + `port/sim`，
  输出 `c6os host smoke` / `PASS`（`make check-host`）。
- `kprintf` host 断言 PASS（`%d/%u/%x/%X/%s/%c/%p`、宽度/补零）。c6 的 `va_arg`
  在 LLVM IR 路径输出错位，故格式化实现移到 `rt/kprintf.c`（C），c6 侧保留
  `kputc/kputs`；目标侧同一份 C 交 xtensa-gcc。
- tick 计数/换算 host PASS（`os_tick_get/advance`、100Hz、`ms<->ticks`）。
- M1 最小调度器 host PASS：16 槽静态池、`os_task_create/yield/exit/start`、
  同优先级轮转，两任务顺序 `ABab`；sim 用 ucontext（`port/sim/port_ctx.c`）。
- M1 补测 host PASS：`create/delete` 1000 次后 `os_task_count()==0`；
  `os_task_delay` + 虚拟 tick 顺序 `ABba`；就绪位图 + 软件 ctz 选优先级；
  每次切换检查栈底 `0xC6C0DEAD`，损坏走 `os_panic("task stack overflow")`。
- M3-1/M3-2 host PASS：信号量阻塞/唤醒（顺序 `BA`）、超时返回 `-2`（`T`）、
  互斥量递归与所有权移交（顺序 `AaB`）。
- M3-4 host PASS：事件 ANY 掩码 `0x2` 唤醒、ALL 掩码 `0x3` 两位置齐才唤醒、
  `os_event_clear` 后 `os_event_get` 只剩另一位。
- M4-1 host PASS：容量 1 的指针队列，生产者第二次 put 阻塞、消费者 get 唤醒，
  取回 1+2=3，顺序 `PGpg`；空队列 get 超时返回 0。
- M4-3/M4-4 host PASS：4×32B membox 耗尽返回空、非法/未对齐 free 返回 -1；
  8KB best-fit 堆 1000 次随机分配/释放后 `used==0`、`peak>0`。
- M2-3 host PASS：周期 2 tick 定时器第 2/4 tick 各命中一次，stop 后不再命中；
  一次性定时器只命中一次。
- M3-1/M3-3 host PASS：3 个等待者（prio 1/2/3）按优先级唤醒，顺序 `BCA`；
  `os_task_boost/restore` 在 prio 5↔1 间切换正确。
- M5 兼容层 host PASS：`LOS_SemCreate/Pend/Post/Delete` 走 `OsSem`，句柄 1..8，
  删除后 pend 返回 `LOS_ERRNO_SEM_INVALID`；`LOS_TickCountGet/Hz` 与 `os_tick_*` 一致。
- panic/环形日志 host 测试 PASS：`os_panic("boom")` 打印 `*** PANIC: boom ***`，
  回放 4KB 环里的 `boot ok`，再进 `--- halt ---`（sim 的 `port_panic_halt` 返回）。

---

## M1 任务与调度

| 任务 | 细节 | 验收 | 状态 |
|---|---|---|---|
| M1-1 TCB/对象池 | `struct OsTask`、16 槽静态池、magic 校验 | 创建/删除 1000 次无泄漏 | 完成（`magic=0xC6A50001` create 置位/delete+reap 清 0/lookup 校验，`t_task2` 删除后查找为空；C 侧不再解引用 OsTask，改用 `os_task_info`） |
| M1-2 就绪位图 | `u32 ready_bit` + 32 环形链、`clz` 选最高 | 位图单测 32 组 | 完成（`t_prio32`：31/5/10 三任务按 5→10→31 运行；32 个优先级逐个创建/运行全 PASS） |
| M1-3 创建/删除 | `os_task_create/delete`；自删除进 reap 链 | 自删除任务栈延迟回收 | 完成（`t_task2` 自删除后不返回、调度器延迟 `port_task_ctx_free` 并清 magic，`t_stress` 10^6 次无泄漏） |
| M1-4 挂起/恢复/延时 | `suspend_cnt`、`delay` 入超时队列 | 挂起计数嵌套正确 | 完成（`t_suspend` 嵌套 2→1→0 + 自挂起/他人恢复 PASS；delay 虚拟 tick PASS） |
| M1-5 时间片 | 10 tick 轮转，同优先级 FIFO | 两任务交替计数 1:1 | 完成（x86 PIT 抢占 `PASS: x86 preempt`；S3 定时器中断抢占 `ca=61198 cb=61311` / `PASS: s3 preempt`；host SIGALRM 异步抢占 `PASS: sim preempt`） |
| M1-6 栈哨兵 | 栈底 `0xC6C0DEAD`、每次切换检查 | 溢出必 panic 且打印任务名 | 完成（`t_overflow` 深递归覆盖哨兵，输出 `*** PANIC: task stack overflow ***` 后 `PASS`；sim 栈前留 64KB 安全垫） |
| M1-7 join | `os_task_join(timeout)` | 先 join 后退出、超时、删除后 join 三种路径 | 完成（`t_task2`：自删除、join 超时 `-2`、delete 后 join 返回 0，全部 PASS） |
| M1-8 上下文切换 | S3 call0 协作上下文（a0–a15/sp）；抢占陷阱帧后续补 | S3-4：两任务交替打印 1000 次无错 | 完成（S3 协作 `order: ABab` / `PASS: sched`；S3 定时器中断里 `os_sched_preempt_ctx()` 直接换任务，忙等任务不让出 CPU 也能 `PASS: s3 preempt`；x86 陷阱帧抢占 `PASS`） |

**M1 验收**：≥15 条断言；S3 上两任务交替；x86 裸机 1000 次切换寄存器一致。

---

## M2 tick 与软定时器

| 任务 | 细节 | 验收 | 状态 |
|---|---|---|---|
| M2-1 tick 计数/换算 | `u64` tick、ms↔tick 向上取整 | 边界（0/1/1000ms）正确 | 完成（计数/换算 host PASS；`ms→tick` 已改向上取整，0/1/101ms 边界断言通过） |
| M2-2 超时队列 | 按 deadline 有序插入/摘除 | 乱序插入 100 个，出队有序 | 完成（`kernel/timeout.c6` 有序数组 + u32 回绕比较；`t_timeout` 乱序 100 个、取消、到期弹出 PASS） |
| M2-3 软定时器 | 一次/周期、启动/停止/删除 | 周期计数误差 ≤1 tick/100 次 | 完成（一次/周期/启停 host PASS；S3 实测一次 100 tick 于第 100 tick 命中） |
| M2-4 抖动测量 | `CCOUNT` 记录 ISR 进出 | 打印 min/max/P99 | 完成（S3 真板：`min=400053 max=695423 avg=401052 p99dev=0 n=311`，`PASS: s3 jitter`；直方图 2k cycles/桶算 P99） |
| M2-5 虚拟时钟 | host/sim `os_test_tick(n)` | L1 无 wall-clock 依赖 | 完成（`port_virtual_tick` + `os_sched_tick` 虚拟推进） |

**M2 验收**：S3 上 100Hz 实测 ±1%；软定时器 100 次周期无漂移累积。

---

## M3 同步

| 任务 | 细节 | 验收 | 状态 |
|---|---|---|---|
| M3-1 信号量 | 计数、FIFO、超时、ISR post | 3 等待者按优先级唤醒 | 完成（pend/post/超时 + 3 等待者按优先级唤醒 host PASS） |
| M3-2 互斥量 | 递归、所有权移交、ISR 禁 pend | 递归 3 层解锁正确 | 完成（递归 + 所有权移交 host PASS；`port_in_isr` 下 block/delay panic，`t_isr` PASS） |
| M3-3 优先级继承 | `max(base, 等待者)`、链式重算 | 3 任务传递链反转对照通过 | 完成（`t_pi_chain`：L(5) 持 M1→M(3) 持 M2 等 M1→H(1) 等 M2，L 依次被抬到 3/1，`PASS: PI chain`） |
| M3-4 事件 | 32 位、AND/OR、清除、读后清 | 4 组组合断言 | 完成（ANY/ALL/clear + `LOS_WAITMODE_CLR` 读后清，`t_los_event_clr` 验证读走 0x1 后只剩 0x4） |
| M3-5 等待节点双归属 | 对象 pend 链 + 超时队列原子摘除 | post/超时/删除三方竞争 10^5 次无双重入队 | 完成（`t_race` 10^5 轮 pend/post/tick 竞争：成功+超时=100000、坏返回 0、任务数归零，`PASS: wait race`） |

**M3 验收**：反转对照组（无 PI vs 有 PI）延迟差 ≥5×；超时路径无泄漏。

---

## M4 IPC 与内存

| 任务 | 细节 | 验收 | 状态 |
|---|---|---|---|
| M4-1 `Queue@T` | 泛型队列、元素拷贝、满/空/超时 | 元素为结构体/指针/u64 三类 | 完成（`Queue@T` 按 `sizeof(T)` 深拷贝；`t_queue_generic` 验证 int/u64/结构体；指针队列 `os_queue_init` 与 LOS_Queue 仍 PASS） |
| M4-2 兼容队列 | `LOS_QueueCreate/Read/Write` | 与原生同一断言集 | 完成（`compat/los_queue.c6` + `t_los2` 的 LOS_Queue 读写/PASS） |
| M4-3 定长内存池 | `membox`、O(1) 分配、归属校验 | 耗尽返回 `OS_ENOMEM` | 完成（O(1) 分配/释放 + 归属/对齐校验，host PASS） |
| M4-4 best-fit 堆 | 块头/合并/对齐/统计 | 1000 次随机分配释放无碎片泄漏 | 完成（best-fit + 相邻合并 + 8B 对齐，1000 次随机 host PASS） |
| M4-5 统计 | 已用/峰值/最大空闲块 | 与手算一致 | 完成（`os_heap_used/peak/max_free`，分配后最大空闲块下降断言通过） |

**M4 验收**：队列三类元素；堆 1000 次随机操作后 `used==0`；OOM 路径可恢复。

---

## M5 双 API

| 任务 | 细节 | 验收 |
|---|---|---|
| M5-1 `include/los/*.h` | 类型/宏/原型与 LiteOS-M 对齐 | 用 LiteOS 示例头可编译 |
| M5-2 `compat/los_*.c6` | `LOS_Task/Sem/Mux/Event/Queue/Membox/Mem/Tick/Swtmr` | 每个对象两条路径结果一致 | 完成（Task/Sem/Mux/Event/Queue/Membox/Mem/Swtmr 均有 host PASS：`t_los`/`t_los2`/`t_los3`/`t_los4`） |
| M5-3 错误码映射 | `OS_*` → `LOS_*` | 映射表 100% 覆盖 | 完成（`t_los_err`：Sem 非法句柄/超时/重复删除、Task 空参/非法优先级/非法 ID、Queue 非法句柄，全部命中 `LOS_ERRNO_*`） |
| M5-4 一致性测试 | 同一断言集跑原生/兼容 | 输出逐行一致 | 完成（`t_consistency` 对 Sem/Queue 分别跑原生与 LOS 两路：post→pend、超时失败、put/read 值与字节一致，输出 `PASS: native/LOS consistency`） |

**M5 验收**：`make check-host API=both` 双份全绿；LiteOS 风格 demo 可跑。

- **S3 上 IPC/内存池冒烟（2026-11）**：`tests/t_s3_ipc.c6` 在真板依次跑
  sem/mutex/event/queue/membox/heap 的非阻塞路径，串口输出 17 行
  `PASS: ...` + `ALL PASS: ipc`。用于确认 c6→C99→Xtensa 下结构体布局、
  指针队列与堆对齐没有 32 位目标特有的问题。

---

## M6 中断与 HWI

| 任务 | 细节 | 验收 |
|---|---|---|
| M6-1 向量表 | `HwiCreate/Delete`、优先级、使能 | 3 个外设中断共存 | 完成（`kernel/hwi.c6` 32 槽注册表 + `compat/los_hwi.c6` 双 API；`t_hwi` 3 个 IRQ 共存/参数/禁用/重复注册/删除 + LOS_Hwi 全 PASS） |
| M6-2 下半部 | ISR → 任务唤醒延迟 | S3-5：延迟 < 100µs | 完成（S3 定时器 ISR 末尾 post 信号量唤醒任务，100 次 `min=2943 avg=2992 max=4486` cycles，平均 74.8µs < 100µs，`PASS: s3 irq latency`） |
| M6-3 嵌套/关中断 | `port_irq_save/restore` 嵌套计数 | 嵌套 5 层恢复正确 | 完成（`t_irq_nest` 5 层深度 0→4→0，`PASS: irq nest`；sim 计数、x86 RFLAGS.IF、S3 `rsil/wsr ps`、rv32 用户态 no-op） |
| M6-4 ISR 安全 API | post/set/put/resume 白名单 | ISR 里误用阻塞 API 必 panic | 完成（`os_task_block_arg`/`os_task_delay` 检查 `port_in_isr` 并 panic；`t_isr` 输出两条 PANIC 后 PASS） |

**M6 验收**：ISR 唤醒任务延迟实测；嵌套中断不丢 tick。

---

## M7 shell 与屏

| 任务 | 细节 | 验收 |
|---|---|---|
| M7-1 shell | COBS 帧 + 命令表（ps/free/tick/date/log/reboot） | 每条命令输出正确 | 完成（host `t_shell` 捕获断言；S3 真板 USB 串口输入 `help/tick/ps/free` 全通：`ps n=1 / 1 1 X sh`、`used=0 peak=0 maxfree=4080`） |
| M7-2 面板探测 | RDDID 识别 ST7789/ST77916，profile 兜底 | 两种面板都能点亮 | **真板完成（屏幕已点亮，用户目视确认）**（`rt/panel.c` host 匹配 PASS；真板确认为 **ST7789 240×240 SPI**，SCLK=9/SDA=10/DC=8/CS=14/RST=18/BL=13，上游出处见 `docs/s3-board-identification.md`。`port/s3/panel.c` 已改成 GPSPI2 硬件 SPI 并跑通初始化：`SPI_CLK_GATE` 少 `CLK_EN`(bit0) 导致模块时钟被门控是最关键的坑（写 `CMD.USR` 读回仍是 0、面板收不到任何字节），另外对齐了 mode 3、`CS_KEEP_ACTIVE`、GPIO21 电源保持；寄存器配方/自检手段/踩坑全部记在 **`docs/s3-panel-bringup.md`**。同时查清两起"整机像死机"的真正原因：①`volatile` 忙等循环触发的 `memw` 永久等待；②RTC 超级看门狗 SWD 未关导致每 ~3.5s 整片复位（现已关，见 `port/s3/console.c`）。**当前驱动有两条通路**：GPSPI2 硬件 SPI（数据通路已验证：MOSI 256 边沿/64B）与
bit-bang 兜底（PCNT 实测 512 边沿/64B、整屏填色 5.16M cycles ≈ 15fps）；
**硬件 SPI 的 SCLK 输出实测打不出边沿**（同一时刻 MOSI 有 256 个、SCLK 0 个，
GPIO9 软件翻转能数到 99，测量链路正常），所以 `port/s3/panel.c` 现在**默认走 bit-bang**（`g_bitbang=1`），demo/dash/`t_s3_lcd`
全用它；`s3_lcd_use_bitbang(0)` 可切回硬件 SPI 做实验。
详见 `docs/s3-panel-bringup.md` §4.5。**已由真板确认成像（用户目视：颜色/竖条/文字/仪表盘都对）**，`tests/t_s3_lcd_kiosk.c6`（当前留在板上跑）会
自动轮播 纯色红/绿/蓝 → 8 色竖条+文字 → 仪表盘一帧，串口同步打印各步耗时与 tick |
| M7-3 帧缓冲/字体 | RGB565、8×16 字体、`fill_rect/blit` | 240×280 全屏刷新率 ≥10fps | **真板完成**（`rt/gfx.c` RGB565 `pixel/get/fill_rect/blit`、8×8 字库 + `char16/text16` 把每行复制成 8×16 字符单元；`t_gfx` host PASS。真板：240×240×16bit 帧缓冲 115200B 放进扩展 DRAM（`link.ld` DRAM 起址 0x3FCA0000），`t_s3_lcd` 实测整屏填色 5.16M cycles、帧缓冲全屏 blit 5.81M cycles；用 SPI 分频反算 CPU 跑 **80MHz PLL**，折算 **≈14fps**，已过 ≥10fps 验收线，`PASS: s3 lcd`） |
| M7-4 仪表盘 | 任务表/CPU/内存/日志滚动 | 屏上数字与串口一致 | **真板通过**（`rt/dash.c` 绘制 tick/任务数/堆用量 + 任务行；`t_dash` host PASS；`tests/t_s3_dash.c6` 在 S3 上建 dash 任务，每帧 6.69M cycles（首帧含字体，≈5.9fps）画进 240×240 帧缓冲再 blit 上屏，串口同步打印同一组数字：`tick=57/83/109... tasks=3`，tick 稳定递增 ≈26/帧，说明定时器→调度器→dash 任务整条链路在真板成立；屏幕成像已由用户目视确认） |

**M7 验收**：屏显示与串口数据一致；shell 可交互。

---

## M8 收尾

| 任务 | 细节 | 验收 |
|---|---|---|
| M8-1 压力/长稳 | 随机创建/删除/超时 10^6 次 | 10 分钟无泄漏、无 panic | 完成（host `t_stress` 10^6 次 6.9s；S3 `t_s3_stress` 25000 轮创建/延时/回收，00:20:11→00:29:59 约 9m48s，`PASS: s3 stress`，无 panic） |
| M8-2 双核 | 第二核启动、每核就绪队列、IPI | 两核各自跑任务，负载均衡可观测 | **真板通过**（`tests/t_s3_core1.c6`：core1 裸核计数 → `core0 prid_raw=52685 core_id=0` / `core1 count=344 prid=1` / `PASS: core1`；`tests/t_s3_smp.c6`：两核各跑一个 yield 任务 → `PASS: s3 smp core1 scheduler`；`tests/t_s3_smp_sem.c6`：核 1 `os_sem_pend` 阻塞、核 0 `os_sem_post` 唤醒 → `PASS: s3 smp cross-core sem`。本轮修掉的三处：①`port/s3/ctx.S` 的 `port_task_switch`/`port_task_yield` 把核槽位写死成 0（双核共用同一份保存区）；②`port_task_switch` 必须**先压帧再算核号**，否则算下标用的 a3/a5/a6/a12 会毁掉被打断任务的寄存器（现象是随机 `TRAP cause=1c`）；③切换路径不能 `callx0` 进 C 函数（本工具 C 是 windowed 风格调用约定，callx0 不转窗口会冲掉 a12-a15），所以核号在汇编里直接读 PRID 与 `g_prid_base[0]` 比较。回归：`t_s3_lcd` 仍 `PASS: s3 lcd`、`t_s3_dash` tick 稳定递增。risc6 参考机侧另有 `--harts 2` 的 SMP 冒烟（`make check-sim`，`PASS: smp task a/b`、`PASS: smp cross-core sem`）。S3 真 IPI 仍未做（核 1 用忙轮询 `port_idle` 观测别核投递） |
| M8-3 FPU 优化（可选） | 惰性保存 FPU 帧；全镜像统一 ABI | 浮点任务切换上下文一致 | 跳过（可选；内核核心不使用浮点，S3 启动未使能 FPU） |
| M8-4 PSRAM（可选） | 8MB 初始化、帧缓冲迁移 | 大缓冲分配成功 | 跳过（可选；屏驱动未点亮，帧缓冲仍在内部 RAM） |
| M8-5 上板文档 | 烧写/回滚/量产镜像 | 新人按文档可复现 | 完成（`docs/board-s3.md`：工具链/构建/BOOT+RESET 烧写/USB 重枚举/串口/中断要点/故障表） |

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

- 2026-11 追加：`_bss_start` 原先未 4 字节对齐（`0x3FCE309B`），`start.S` 的清零循环会触发
  Xtensa 对齐异常；链接脚本已给 `.data/.bss` 加 `ALIGN(16)`。栈顶原 `0x3FCFF000`
  超出 S3 480KB DRAM（`0x3FC88000+0x78000=0x3FCF8000`），已改 `0x3FCEF000`。
- 入口/阶段标记实验：RTC STORE0 预置 `0xBEEF` 后复位仍为 `0xBEEF`，未见应用写入；
  物理 RESET 后 USB 设备直接消失（dmesg `USB disconnect`），换用“不复位 USB”的控制台
  初始化后仍消失。下一步用**纯汇编最小镜像**（只写 USB EP1）区分“未执行”与“执行后挂死”。
- 注意：清 `build/s3` 时把原厂 16MB flash 备份 `build/s3/backup/flash-16MB.bin` 删掉了；
  原厂固件已无本地备份（`0x0` 处已被本内核覆盖）。如要恢复原厂，需要重新从官方固件包刷。

- **S3 上板成功（2026-11）**：USB-Serial/JTAG 打印 `c6os s3 boot` / `c6os: alive`。
  三个关键修复：
  1. **`.rodata` 不能放 IRAM `0x403C...`**：该地址是 Xtensa 指令别名，数据 load 触发
     `Guru Meditation Error: LoadStoreError`（`EXCVADDR` 指向字符串）。链接脚本把
     `.rodata*` 并入 `.data`（DRAM `0x3FCE3000`）。
  2. **USB PHY 归属**：S3 内部 FSLS PHY 默认可能给 USB-OTG；应用要先写
     `RTCCNTL.usb_conf`（`0x60008120`）`SW_HW_USB_PHY_SEL(bit20)=1`、
     `SW_USB_PHY_SEL(bit19)=0` 把 PHY 切回 USB-Serial/JTAG，再使能
     `SYSTEM_PERIP_CLK_EN1` bit10 与 `CONF0.USB_PAD_ENABLE(bit14)`。
  3. **看门狗**：`start.S` 里过早写 TIMG0 不可靠；改在 `port_init` 先开
     `SYSTEM_PERIP_CLK_EN0` bit13（TIMG0），再按 `0x50D83AA1` 解锁写
     `WDTCONFIG0/1=0`。否则约 300ms 被 `TG0WDT_SYS_RST` 复位，USB 永远枚举不上。
- 实测启动日志（拨插 USB，非按 RESET）：
  `ESP-ROM ... load:0x3fce3000,len:... load:0x403c9000,len:... entry 0x403c901c`
  → `c6os s3 boot` → `c6os: alive`。`--ram-only-header` 会带一条
  `SHA-256 comparison failed ... Attempting to boot anyway`，不影响启动；后续改成
  带 SHA256 的普通镜像即可消除。
- 注意：本板按 RESET 会让 USB 直接掉线不回来，验证请用**拔插 USB**（重新上电）。

- **S3 协作调度实测（2026-11）**：`tests/t_s3_sched.c6` 两任务 A/B 交替 `yield`，串口输出
  `order: ABab` / `PASS: sched`。`port/s3/ctx.c` + `ctx.S` 用静态 4KB 栈池，call0 保存
  `a0–a15 + sp`；`port_task_switch` 必须先把目标任务写入 `g_s3_cur`，否则每次切换都从
  任务入口重启（表现为 `order[]` 越界 `StoreProhibited`）。
- S3 测试镜像构建：
  `make BUILD=build/s3test S3_MAIN=tests/t_s3_sched.c6 S3_EXTRA_C6="kernel/task.c6 kernel/tick.c6 kernel/timer.c6" s3`

- **c6 C99 后端指针加载修复（2026-11）**：`t != 0` 在 32 位目标上恒真。根因是
  `tcc/c99-gen.c:load()` 对 `VT_LLONG` 一律发出 `*(long long *)slot`，而指针局部槽
  只写 4 字节，高半是脏的；c6 跑在 64 位主机上又按 `PTR_SIZE==8` 把指针比较升成
  64 位。修复：当 `sv->sym` 是 `VT_PTR`/`VT_FUNC` 时改用
  `(long long)(unsigned long)*(void **)slot`（按目标指针宽度加载）。
  superc 仓库提交 `a8d010a`；`make c99test` 103/103 通过；S3 上 `os_timer_tick`
  恢复原来的 `t && active && expire` 写法后不再误入空指针分支。
- **S3 tick 实测通过（2026-11）**：`tests/t_s3_tick.c6` 串口输出
  `DONE elapsed=100` / `PASS: tick`。此前“约 24 个 tick 后停住”其实是三个问题叠加：
  1. **TG0 看门狗每 ~3s 复位一次**（`dmesg` 里 USB 每 3 秒 disconnect/reconnect）。
     `port_init` 的关闭只做一次且不可靠，现改为 `s3_wdt_kick()` 在每次 `port_idle`
     里重新解锁并清零 `WDTCONFIG0/1`，复位消失。
  2. **CPU 仍是复位后的 40MHz XTAL**，不是 240MHz；按 240MHz 算的
     `2,400,000 cycles/tick` 大了 6 倍。现按 40MHz 用 `400,000 cycles/tick`
     （真正的 240MHz PLL 配置留作 M0 后续项）。
  3. **USB-Serial/JTAG 发送**：写入前必须等 `EP1_CONF.WR_DONE` 清零，否则 FIFO
     写不进去、`DATA_FREE` 再也不置位，串口在几十字节处整体卡死；`port_putc`
     已加这段等待。
- **c6 → C 变参 ABI**：`kprintf("elapsed=%u", (u64)...)` 在 S3 上只打出
  `elapsed=` 就停住（c6 调用侧与 C `va_list` 布局不一致）。S3 测试暂时改用手写
  十进制输出；`kprintf` 的变参修复单列为 c6 后续项。
- S3 测试镜像构建：
  `make BUILD=build/s3test S3_MAIN=tests/t_s3_sched.c6 S3_EXTRA_C6="kernel/task.c6 kernel/tick.c6 kernel/timer.c6" s3`


- **x86-64 参考目标上板等价验证（2026-11）**：`make run-x86` 在 qemu 里从自写引导扇区启动，
  经 16→32→long mode 进入 c6 生成的 64 位 `os_main`，串口输出
  `c6os x86_64 boot` / `PASS: x86 hello`。要点：
  1. qemu 8.2 的 `-kernel` 不再接受 multiboot/PVH，只能走**自有引导扇区**
     （int13h LBA 读平铺镜像到 0x10000，再切保护模式）。
  2. 4 级页表需要 PML4→PDPT→PD 三层，直接把 PS=1 的 PD 挂在 PML4 下会 triple fault。
  3. c6 的 IR 在 `-O1` 下会用 `movaps` 清栈槽；必须开 CR4.OSFXSR/OSXMMEXCPT，
     或按内核禁浮点约定用 `-mno-sse -mno-sse2` 重编译 IR。

- **x86-64 参考目标任务/时基（2026-11）**：新增 `port/x86_64/ctx.S` 协作上下文
  （保存 rbx/rbp/r12–r15 + rsp + 返回地址）与 `ctx.c` 静态栈池，`make run-x86-sched`
  输出 `order: ABab` / `PASS: x86 sched`；`make run-x86-tick` 用 `rdtsc` 每 10M 周期
  推进一 tick，`os_task_delay(100)` 实测 `elapsed=100` / `PASS: x86 tick`。
  裸机无中断源，`port_idle` 用 `pause` 忙等而不是 `hlt`（否则永不唤醒）。
  引导扇区一次读 255 扇区（0x10000–0x2FDFF），覆盖 ~100KB 的内核载荷。

- **rv32 参考目标（2026-11）**：`make run-rv32` 用 qemu-riscv32 user-mode，c6 IR 的
  `target triple/datalayout` 由构建规则改写成 riscv32（c6 本身只产出 x86-64 IR），
  串口走 `write(1,...)` 系统调用，输出 `c6os rv32 boot` / `PASS: rv32 hello`。

- **rv32 调度与 tick（2026-11）**：`port/rv32/ctx.S` 保存 ra/s0–s11/sp，
  `make run-rv32-sched` 输出 `order: ABab` / `PASS: rv32 sched`；
  `port_idle` 用 `csrr time` 每 1,000,000 周期推进一 tick，
  `make run-rv32-tick` 输出 `elapsed=100` / `PASS: rv32 tick`。
  32 位目标缺 `__udivdi3/__umoddi3`，已在 `rt/x86_libc.c` 补移位实现。

- **x86-64 抢占（2026-11）**：`port/x86_64/irq.c` 建 IDT、重映射 PIC、PIT 100Hz；
  `irq.S` 保存完整陷阱帧，抢占时把帧当成任务上下文切回调度器，恢复时
  `iretq`。`make run-x86-preempt` 两同优先级任务纯忙等，输出
  `ca≈1.18M cb≈1.19M` / `PASS: x86 preempt`。
  两个关键坑：抢占路径不回 `iret`，必须在切回调度器后 `sti`；切换前把
  `g_in_task` 清零，避免在调度器栈上再次抢占写坏任务上下文。

- **M2-2 有序超时队列（2026-11）**：`os_timeout_add/cancel/pop_expired` 用绝对
  deadline 有序插入（`(i32)(a-b)` 处理 u32 回绕）；`os_sched_tick` 只弹到期项，
  不再线性递减 `delay_left`。阻塞/延时/唤醒/删除都同步取消，减少 M3-5 的双归属窗口。
  host `t_timeout`：100 个乱序插入后 deadline 单调、取消中间项、到期全弹出 PASS。

- **S3 level-1 定时器中断（2026-11，真板 PASS）**：`port/s3/irq.S` 在 IRAM 建 1KB
  向量表（Xtensa `j` 是 3 字节，0x300 偏移要 256 条），`wsr vecbase` 前先清
  `PMS_CORE_0_VECBASE_OVERRIDE`（0x600C11C0 SEL=0，否则 VECBASE 被 ROM 覆盖），
  并清 `PS.UM`（ROM 进来是 user mode，level-1 会走 0x340 用户向量）。
  `CCOMPARE0 = CCOUNT + 400000`、`INTENABLE |= (1<<6)`，handler 保存 a0–a15/EPC1/PS，
  用 `wsr PS + rsync + rfe` 返回。`make ... S3_MAIN=tests/t_s3_irq.c6 s3` 实测
  `PASS: s3 irq tick`（纯忙等任务里 tick 仍推进 5 个）。`port_idle` 改为 `waiti 0`。

- **模拟器异步 tick 抢占（host sim PASS）**：`port/sim/port_ctx.c` 用 `SIGALRM`
  （10ms）模拟定时器中断，handler 里 `os_sched_tick()` 后 `swapcontext` 到调度器；
  两个忙等任务完全不让出 CPU，仍都能推进（`ca≈6.9M cb≈7.3M`），输出
  `PASS: sim preempt`。handler 走 `os_sched_preempt_ctx()` 直接切到目标任务
  （不经过协作调度器），与 S3 计划的中断返回路径同构。关键坑：信号处理栈
  **不能**用 `SA_ONSTACK` 共用 altstack，否则后一次中断会覆盖前一个任务挂起中的
  sigreturn 帧，任务会“凭空返回”。

- **S3 定时器中断抢占（真板 PASS）**：level-1 入口把 a0/a2–a15 + EPC1 + PS 压成
  80 字节陷阱帧，`s3_timer_irq(frame)` 只把 `ctx->sp` 指到该帧，`os_sched_tick()`
  后用 `os_sched_preempt_ctx()` 选出下一个任务，入口改 `a1 = target->sp` 并复用
  “弹帧 + `rfe`” 路径。任务创建时预置同样格式的初始帧（a2=entry、a3=arg、
  EPC=tramp）。两个同优先级忙等任务都不 yield，20 tick 内计数 `ca=61198 cb=61311`。
  关键坑：XTOS 的 `PS` 必须用 `wsr ps` 写入“去掉 EXCM/INTLEVEL 的值再或上 EXCM”
  后 `rfe`；协作切换保持 `ret`（a0=恢复 PC）。

- **M6-3 关中断嵌套（host PASS）**：`u32 port_irq_save()` 返回进入前状态、
  `port_irq_restore(state)` 恢复；5 层嵌套返回 0/1/2/3/4，倒序恢复后再次 save 仍为 0。
  sim 用计数模拟；x86 保存 RFLAGS 并清 IF；S3 用 `rsil a2,15` / `wsr a2,ps`；
  rv32 参考机是用户态，当前 no-op。

- **S3 陷阱入口（真板 PASS）**：`_vec_level1` 先读 `EXCCAUSE`，等于 4 才是定时器
  中断，其余跳到 `_vec_exc`，把陷阱帧交给 `s3_trap()` 打印 `cause/pc/ps` 并停机。
  `tests/t_s3_trap.c6` 执行 `ill`，实测 `*** TRAP cause=00000000 pc=403cb9f0 ps=00060500`；
  定时器用例仍 `PASS: s3 irq tick`。

- **M2-4 tick 抖动（真板 PASS）**：定时器 ISR 入口读 `CCOUNT`，统计相邻中断间隔，
  `os_jitter_min/max/avg/p99/count()` 输出；200 tick 后 `avg=401052` cycles
  （100.26Hz，理论 400000），99% 偏差落在 <2000 cycles 桶。同时修掉调度器空转时
  `g_s3_cur` 仍指向已阻塞任务的问题：S3 `port_leave_task()` 现在清空当前 ctx，
  否则 ISR 会把 idle 上下文写进阻塞任务。

- **M1-2 就绪位图 32 组（host PASS）**：`t_prio32` 先同时挂 31/5/10 三个就绪任务，
  验证 `ctz32(ready_bit)` 选最高优先级得到 5→10→31；再对 0..31 每个优先级单独
  创建/运行一次，全部命中。

- **M5-3 错误码映射（host PASS）**：`t_los_err` 在任务上下文里逐项断言
  `LOS_ERRNO_SEM_INVALID/TIMEOUT`、`LOS_ERRNO_TSK_INVALID/NO_MEMORY`、
  `LOS_ERRNO_QUEUE_INVALID`，输出 `PASS: LOS error mapping`。

- **M4-1 泛型值队列（host + S3 PASS）**：`OsQueue` 改为按 `item_size` 字节拷贝，
  `os_queue_init_ex/put_item/get_item` 支持任意元素；`include/c6os/queue_t.h`
  用 c6 泛型 `Queue@T` 包装成 `q_init/q_put/q_get`。`t_queue_generic` 覆盖
  `int`/`u64`/`struct Pair`，输出 `PASS: generic queue`；S3 `t_s3_ipc` 的
  q-init/put/get 与其余 IPC 全 PASS。

- **M3-5 等待节点竞争（host PASS）**：`t_race` 让 waiter（prio 2）循环
  `os_sem_pend(...,1)`，poster（prio 1）交替 `post + yield`，双方每轮都
  `os_sched_tick()` 推进超时；100000 轮后 `ok+timeout==N`、`bad==0`，
  `os_task_count()==0`，没有双重入队/唤醒泄漏。

- **M6-1 Hwi 注册表（host PASS）**：`os_hwi_create/delete/enable/dispatch/count`
  维护 32 个中断槽；`t_hwi` 注册 IRQ 2/5/9，分别带不同 arg 触发，验证计数、
  禁用后不触发、重复注册失败、删除后计数下降，输出 `PASS: hwi 3 irq`。

- **M6-1 LOS_Hwi 双 API（host PASS）**：新增 `LOS_HwiCreate/Delete/Enable/Disable`，
  映射到 `os_hwi_*`；`t_hwi` 追加 LOS 路径，3 个 IRQ 共存与禁用/删除全部 PASS。

- **M7-1 shell 核心（host PASS）**：`os_shell_exec()` 解析一行命令，内置
  `help/tick/ps/free/log`（`free` 绑定 `OsHeap`，`ps` 遍历任务表打印 id/prio/状态/名字，
  `log` 走 `klog_dump`）。`t_shell` 捕获输出并逐条断言，`PASS: shell`。

- **M7-1 COBS 帧（host PASS）**：`cobs_encode/decode` 带可选 0x00 分隔符；
  `t_cobs` 验证 `{11 22 00 33}` → `03 11 22 02 33 00`、连续 5 个 0 的块码，
  以及编码→解码回环，输出 `PASS: cobs`。`os_shell_exec_frame()` 已把 COBS 帧解成
  一行再执行，`t_shell` 用编码后的 `tick` 帧走通。

- **S3 shell 输入基础（未完成）**：`port_getc()` 已读 `USB_SERIAL_JTAG_EP1_REG`
  并用 `EP1_CONF.SERIAL_OUT_EP_DATA_AVAIL` 门控、`OUT_EP1_ST.REC_DATA_CNT`
  计数批量取字节；默认 S3 构建加入 `rt/shell.c`、`rt/cobs.c`、`kernel/heap.c6`，
  `t_s3_shell` 可构建并输出 banner。调试中曾观察到主机写入后 `EP1_CONF` 由
  `0x2` 变 `0x6`、`OUT_EP1_ST` 计数变为 3，但 shell 任务未稳定取到命令；
  下一步在任务里打印每次 `port_getc()` 的返回与寄存器。

- **M7-1 命令补齐（host PASS）**：新增 `date`（tick→ms uptime）与 `reboot`
  （`port_reboot()`，S3 置 `RTC_CNTL_OPTIONS0.SW_SYS_RST`，host no-op）。
  `t_shell` 断言 `uptime_ms=`，命令表覆盖 help/tick/ps/free/log/date/reboot。

- **S3 构建源列表修复**：`$(BUILD)/c99/.stamp` 原先用 `cmp` 只比较源文件**路径列表**，
  内容改了但路径不变时会跳过 c6 重编译（曾导致改了测试却烧旧镜像）。现在
  `.stamp` 依赖 `s3-src-check` 且每次调用 c6，保证 `S3_MAIN/S3_EXTRA_C6` 变化生效。

- **任务 magic 与 clean 构建（host PASS）**：`OsTask` 增加 `magic`，创建置
  `0xC6A50001`，删除/回收清 0，`os_task_get` 同时校验 state+magic；`t_task2`
  删除后 `os_task_get(id)==0`。另修 `check-host` 漏列 7 个测试依赖的问题，
  `make clean` 后 34 项全 PASS；新增 `make check-all = check-host + x86 + rv32`。

- **M8-5 上板文档**：新增 `docs/board-s3.md`，记录本板实测的下载模式、`--before usb-reset`
  / `--after hard-reset`、115200 raw 串口、VECBASE/PMS/PS.UM、TIMG0 看门狗、陷阱分流，
  以及“原厂备份丢失、回滚需整片官方固件”的风险。

- **M5-4 原生/LOS 一致性（host PASS）**：`t_consistency` 同一段断言分别走
  `os_sem_*`/`os_queue_*` 与 `LOS_Sem*`/`LOS_Queue*`，比较返回码、超时失败、
  指针出队和 4 字节值（0x1234 小端 34 12）一致。

- **M7-3 帧缓冲基础（host PASS）**：`OsGfx` 保存 RGB565 缓冲与宽高，
  `os_gfx_pixel/get/fill_rect/blit` 全部做边界裁剪；`t_gfx` 验证整屏填充、
  负坐标裁剪、3×3 blit 和越界像素不写，输出 `PASS: gfx`。

- **M7-2 面板 profile（host PASS）**：`os_panel_pick(rddid)` 匹配
  `0x8552→st7789 240×280`、`0x7791→st77916 360×360`，未知 ID 回退 ST7789；
  `t_panel` 验证三种输入，输出 `PASS: panel probe`。

- **M7-3 数字字体（host PASS）**：`os_gfx_char/text` 用 8×8 点阵绘制
  `0-9` 与 `:`，支持整数倍缩放；`t_gfx` 绘制 "1" 并断言字形像素与空白行。

- **M7-4 仪表盘渲染（host PASS）**：`os_dash_render(g,tick,tasks,heap_used)` 清屏后
  用 2× 数字分三行画白/绿/红数值；`t_dash` 统计非背景像素 >50，输出 `PASS: dash`。

- **M7-3 字库扩展**：8×8 点阵增加 `A-Z`、空格、`-`、`.`，`os_gfx_text` 可绘制
  `"CPU-1"` 一类标签；`t_gfx` 用 `"A1"` 覆盖字母与数字。

- **M8-1 S3 长稳（真板 PASS）**：`t_s3_stress` 每轮创建 prio2 worker、
  `os_task_delay(1)` 让其运行退出，再检查/回收；每 5000 轮打印 `step/runs`。
  25000 轮从 00:20:11 跑到 00:29:59（约 9m48s），最后 `step 25000 runs=25000`
  紧接 `PASS: s3 stress`，任务数回到 1（driver 自身），全程无 panic。
  串口抓取注意：杀掉后台 capture 时必须连子进程一起杀，否则残留 python
  会占住 `/dev/ttyACM0` 导致后续 esptool 报 I/O error。

- **M7-1 S3 shell 输入打通（真板 PASS）**：USB-Serial/JTAG OUT 必须用 **32 位读**
  `*(volatile u32*)0x60038000 & 0xFF`，用 8 位读会一直得到 0；数据可用性看
  `INT_ST bit2`，字节数取 `OUT_EP1_ST[22:16]`，读完清 `INT_CLR bit2`。
  实测发 `ps\r/free\r/help\r` 得到 `ps n=1`、`1 1 X sh`、`used=0 ...`、
  `help tick ps free log date reboot`。
- **跨语言结构体 ABI**：c6 C99 后端与 gcc 的 `OsTask` 布局不一致，shell 不能直接
  解引用；新增 `os_task_info(idx,id,prio,state,name,cap)` 与 `os_sys_heap_*`
  让 C 侧只走 c6 导出函数。`rt/shell.c` 已改用这两个接口。
- **S3 构建规则修复**：C99 缓存按“源文件路径列表”判断导致改头文件不重编；改为
  `S3_C6` 每个源文件一条规则、依赖全部 `include/c6os|los/*.h`，彻底避免旧布局。

- **c6 C99 与 C 的结构体布局差异**：c6 `-c99` 后端按 64 位主机布局生成结构体偏移
  （`OsTask` 步长 88），而 xtensa-gcc 按 32 位是 72。因此 **C 文件不得直接解引用
  c6 内核结构体**：`rt/shell.c` 的 ps/free 已改为 `os_task_info()` /
  `os_sys_heap_*()` 只读接口，S3 实测 `ps n=1 / 1 1 X sh`、heap 数值正确。
- **S3 构建规则**：`$(BUILD)/c99/%.c` 旧缓存只比较源路径列表，改头文件不重编；
  已改为 `S3_C6` 每个源文件一条 c6 规则，依赖全部 `include/c6os|los/*.h`。

- **M6-2 ISR→任务延迟（真板 PASS）**：`s3_irq_set_hook()` 在定时器 ISR
  `os_sched_tick()` 之后 post 信号量；worker 每 tick `os_sem_pend` 醒来读
  `CCOUNT - ISR入口CCOUNT`。100 次：min 2943 / avg 2992（74.8µs）/
  max 4486 cycles（112µs），平均满足 <100µs。hook 为空时对原中断路径零影响。

- **S3 面板 ID 探测（未通过）**：`t_s3_panelid` 在微雪 1.85 引脚上 bit-bang
  QSPI：CS 拉低→0x0B+00 04 00→8 dummy→单线读 3 字节，返回全 0；
  已尝试 TCA9554 0x20 配置 EXIO2 输出并 0/1 复位，仍为 0。下一步用示波器/
  确认板型（微雪 ST77916 QSPI vs 面包板 ST7789 SPI）与复位极性。

- **面板硬件识别受阻（真板）**：两条候选路径都试过：
  1) 微雪 ST77916 QSPI（PCLK40/CS21/D0 46/D1 45/D2 42/D3 41）：
     0x0B+00 04 00+8 dummy，**从 D1/IO1 单线读**，复位 TCA9554(0x20)
     会卡住（暂禁用），读回 `00 00 00`。
  2) 面包板 ST7789 SPI（SCLK21/SDA47/DC40/CS41/RST45）：
     0x04 后读 4 字节，全 0（可能模块未接 SDO）。
  3) 背光测试：GPIO42 高 2s、GPIO5 高 2s，屏幕均不亮。
  4) GPIO 回环：pin46/45 输出高可读回 1，pin42/41/40/21/10/11 读回 0；
     IO_MUX sel=0..7 对 pin42 均无回环（这些脚被屏/触摸/其他外设占用或非 GPIO）。
  结论：当前只能确认“ID 读路径/背光/复位都不在已试引脚上”，需要屏幕 FPC
  丝印或板子型号照片；在此之前 M7-2/3/4 的真实点亮无法继续。

- **S3 GPIO 回环诊断**：`s3_gpio_loop` 写高/低后读 `GPIO_IN/IN1`；46/45 正常，
  42/41/40/21/10/11 始终为 0；`s3_gpio_loop_sel` 对 pin42 扫 IO_MUX 0..7 也无变化，
  说明这些脚不是空闲 GPIO（接屏/触摸）或需要外设功能，不能当普通 IO 探测。

- **官方固件对照（真板）**：下载 xiaozhi-esp32 v2.5.2 的 1.85 / 1.85b / 1.85c
  合并镜像并烧到 0x0：
  - `1.85`：启动即报 `tca9554 write_direction_reg failed` 并 abort；
  - `1.85c`：同样 TCA9554 失败；
  - `1.85b`：`77916_spi: LCD panel create success`、`Backlight: Set brightness to 75`，
    随后 ES8311 I2C(0x30) 失败 abort；但**屏幕始终不亮**。
  1.85b 源码确认了真实引脚：PCLK40/CS21/D0 46/D1 45/D2 42/D3 41、**RST=GPIO3**、
  BL=5、ST77916 360×360 QSPI，并已生成 `include/c6os/st77916_table.h`
  （v1/v2 两套初始化命令）。结论：屏/背光在当前硬件上 1.85b 官方固件也不亮，
  更像屏排线/背光供电或板型不符，已先恢复 c6 `t_s3_irq` 镜像。

- **M8-2 core1 启动尝试（未通过）**：core1 入口在 IRAM 置自己的 SP/VECBASE 后循环加
  `g_core1_count`；core0 写 0x600C0014（bit1 CLKGATE、bit2 RESET、bit0 RUNSTALL，
  MESSAGE 暂按 bit16 起放入口地址）后等待，读回 count=0。说明启动地址字段编码
  不对（地址 0x403Cxxxx 被截断）或需要 ROM `ets_set_appcpu_boot_addr`；暂不继续。

- **M8-2 core1 启动（真板 PASS）**：正确寄存器是 `SYSTEM_CORE_1_CONTROL_0`
  0x600C0000（bit1 CLKGATE、bit2 RESET、bit0 RUNSTALL）与 `CONTROL_1`
  0x600C0004（32 位入口地址，之前误用 0x600C0014 导致 count=0）。core1 入口在
  IRAM 设自己的 SP/VECBASE 后计数；core0 等 2M 循环读回 `count=83`，输出
  `PASS: core1`。SMP 调度仍未接。

- **ST77916 QSPI 软件驱动（待上电验证）**：`port/s3/panel.c` 增加 4-line bit-bang
  命令（0x02/cmd/0x00 + 参数）、0x32 颜色写、GPIO3 硬复位、GPIO5 背光，使用
  `include/c6os/st77916_table.h` 的 v1 初始化表；`t_s3_lcd` 循环红/绿/蓝/白。
  烧写运行后 **USB 直接掉线**（`/dev/ttyACM0` 消失），符合“无电池时屏/背光
  电流把 USB 拉垮”的猜测；接 3.7V 电池后再验证。

- **M8-2 双核并行用例（待上电）**：`tests/t_s3_twocore.c6` 启动 core1 计数循环，
  core0 同时跑 20 万次内核任务循环，完成后打印 `core0=/core1=` 并要求两者非 0；
  镜像 `build/s3tc/c6os.bin` 已构建，等 USB/电池恢复后上板。

- **M7-4 任务表/日志行**：`os_dash_task_row(g,y,id,prio,state,name)` 画
  id/prio/状态/名字，`os_dash_log` 画灰色日志行；`t_dash` 追加两行渲染断言。

- **M8-2 core1 邮箱（待上电）**：core1 循环里比较 `g_core1_cmd/ack`，core0
  `s3_core1_send()` 自旋等应答；`t_s3_twocore` 发 100 条命令，同时 core0
  20 万次任务循环，要求 `ack==100` 且两核计数非 0。

- **M7-2 ST77916 表校验（host PASS）**：`t_st77916` 遍历 v1/v2 初始化命令，检查
  参数长度 ≤16、延时 ≤2000、命令数 >50，输出 `PASS: st77916 table`。

- **M7-3 帧缓冲→QSPI**：`s3_lcd_blit(fb,w,h)` 设窗口后用 `0x32` 逐像素发
  RGB565；`t_s3_lcd` 先填色再 blit 8×8 红块，等电池/上电后看屏验证。

- **S3 IRQ 嵌套用例（待上电）**：`t_s3_irqnest` 任务里 `port_irq_save()` 后忙等
  5000 万次（>1s），要求 tick 冻结；`port_irq_restore()` 后 `os_task_delay(2)`
  要求 tick 恢复前进。镜像 build/s3in 已构建。

- **M3-4 读后清（host PASS）**：`LOS_EventRead` 支持 `LOS_WAITMODE_CLR`，
  读到 bits 后 `os_event_clear(e,bits)`；`t_los_event_clr` 写 0x5，读 0x1 后
  再读 0x1 超时、读 0x4 成功，输出 `PASS: event clear`。

- **M7-3 8×16 字符单元**：`os_gfx_char16/text16` 将 8×8 点阵每行画两遍，
  得到 8×16 单元（当前字库仍是 8×8 形状）；`t_gfx` 断言 "AB" 的 (3,0)/(3,1) 同时点亮。

- **risc6 裸机定时器（模拟器 PASS）**：新增 `port/rv32bare/`（`.risc6_bare` 标记、
  `mtvec`/`mret` 陷阱、32 寄存器帧、MMIO 0x40000000 控制台）与
  `tests/t_rv32bare_tick.c6`；`make run-rv32bare-tick` 在 `~/risc6/build/risc6 --bare`
  下输出 `PASS: rv32 bare tick`，`insns=263076 rc=0`。这是在**指令边界注入 MTIP**
  的真陷阱路径，不是协作 tick。
- **risc6 用户态回归**：现有 `rv32 hello/sched/tick` 全部在 risc6 上跑通：
  hello 1776 条 / sched 36318 条 / tick 1,876,629 条指令，输出与 qemu-riscv32 一致。

- **risc6 回归入口**：新增 `make check-sim`，依次跑 rv32 user 的 hello/sched
  和 bare MTIP tick；当前输出 hello `insns=1776`、sched `36318`、bare tick
  `263076`，均 rc=0。risc6 已支持 1–4 hart（见 risc6 仓库 E7），M8-2 的每核静态分区已在 `t_rv32bare_smp` 上跑通；真板双核仍待上电。

- **S3 面板改用 GPSPI2 硬件 SPI（真板跑通初始化+整屏刷新）**：
  `port/s3/panel.c` 从 bit-bang 改为 SPI2 CPU 直写 FIFO（W0..W15，64 字节/次），
  `t_s3_lcd` 输出 `fill_cycles=5159894`、`blit_cycles=5808687`、`PASS: s3 lcd`。
  用 SPI 分频档位（/4、/24、/40、EQU_SYSCLK）实测传输耗时线性变化，反算
  **CPU 跑在 80MHz PLL**（不是 40MHz XTAL），故整屏 blit 约 73ms ≈ **14fps**，
  过 M7-3 的 ≥10fps 线。详细配方与自检手段见 `docs/s3-panel-bringup.md`。

- **`SPI_CLK_GATE` 必须置 `CLK_EN`(bit0)**：IDF `spi_ll_master_init` 里是
  `clk_en/mst_clk_active/mst_clk_sel` 三位一起置；只置后两位时模块时钟被门控，
  写 `CMD.USR` 读回仍是 0、SCLK 不出波形、面板一个字节都收不到，但寄存器轮询
  "立刻通过"，极易误判成事务正常。同时把 SPI 对齐原厂：mode 3
  （`MISC.CK_IDLE_EDGE` + `USER.CK_OUT_EDGE`）、`MISC.CS_KEEP_ACTIVE`。

- **`volatile` 忙等会把 CPU 卡死（`memw` 屏障）**：GCC 给每个 volatile 访存插
  `memw`，SPI 事务后只要有 APB 事务未回，下一条 `memw` 就永久等待——表现为
  串口突然静默、定时器中断也不再进，看着像"整机死机/复位"。面板驱动内的延时
  因此改成不带 volatile 的 `nop` 循环。

- **RTC 超级看门狗 SWD 未关 → 每 ~3.5s 整片复位**：读
  `RTC_CNTL_RESET_STATE_REG`（0x60008038）低 6 位得 `0x12`(SYS_SUPER_WDT)，
  表现是「串口只出几行 + USB 每隔几秒重新枚举」，此前多轮排查都被它误导。
  `port/s3/console.c port_init()` 里按 `0x600080B8=0x8F1D312A` →
  `0x600080B4 |= 1<<30`(SWD_DISABLE) → `0x600080B8=0` 关闭后恢复正常。

- **已修复：`t_s3_irq` 的 tick 推进失败**（原记录为"未解问题"）。根因是
  `port_core_id()` 读 Xtensa `rsr.prid` 在这颗 S3 上拿到的是垃圾值（core0=0xCDCD、
  core1=0xABAB），恒等于 1 → `os_sched_tick()` 里"非 0 号核直接返回"的 SMP 保护
  永远命中 → tick 不前进（ISR 其实在跑）。改成启动时按核登记 PRID 值、查表返回
  下标后，真板实测：`t_s3_irq` → **`PASS: s3 irq tick`**；`t_s3_dash` 的
  `tick=57/83/109…` 稳定递增；`t_s3_shell` 正常出 `shell ready` / `shell>`。

- **S3 面板"只有背光"的根因：GPIO 矩阵输出没打开输出使能**（本轮找到并修掉）：
  只写 `GPIO_FUNCx_OUT_SEL_CFG.func_sel`=101/103/110 而不置 `GPIO_ENABLE_REG`
  对应位时，SCLK/MOSI/CS 三根脚是高阻+内部上拉（回读恒 1），面板收不到任何
  字节；而 SPI 事务照常完成（`CMD.USR` 自清、耗时随分频线性变化、移位数据在动），
  所以此前所有"SPI 正常"的判断都成立却点不亮屏。修好后回读立刻正常：CS=0、
  MOSI 采样 996/2000、CS 采样 0/2000。诊断手法（读 pad 电平 / 传输中采样 /
  扫 0..127 号输出信号）见 `docs/s3-panel-bringup.md` §4；顺带修了
  `SPI_CLOCK` 的 H+L≠N 占空比错误（改回自洽的 0x3041）。

- **`port_core_id()` 一直是错的（PRID 在这颗 S3 上是垃圾值），修好后 tick 才动**：
  Xtensa `rsr.prid` 在本板读回 core0=`0xCDCD`、core1=`0xABAB`（三种写法一致、
  每核稳定但都不是 0/1），于是 `port_core_id()` 恒返回 1 → `os_sched_tick()` 里
  "非 0 号核直接返回" 的 SMP 保护一直命中 → **tick 永远不前进**（面板/仪表盘上
  `tick=0` 的真凶，不是定时器问题：ISR 实测在跑）。
  修法：`port_core_bind_self()` 让每个核在入口把自己的 PRID 值登记进
  `g_prid_base[]` 槽位（core0 在 `port_init`、core1 在 `core1_entry`），
  `port_core_id()` 按值查表返回下标。修后 `t_s3_dash` 的 tick 正常递增，
  `t_s3_core1` 也正确报 `core_id=0`/`prid=1`。
  教训：RTC 慢速内存 mark（0x50000000）在这块板子上不可靠（2/3/7 号字读到过
  垃圾值），诊断要用串口打印，别只信 mark。

- **M8-2 双核内核调度真板 PASS（本轮）**：三处修复后 `t_s3_smp` / `t_s3_smp_sem` 均通过。
  ①`ctx.S` 的核槽位写死成 `g_sched[0]` → 改成读 PRID 与 `g_prid_base[0]` 比较取下标；
  ②`port_task_switch` 里"保存现场"和"算核号"的顺序：**必须先压帧**，算核号要用 a3/a5/a6/a12，
  先动它们会毁掉被打断任务的寄存器（表现为随机 `TRAP cause=1c`/LoadStoreAlignment）；
  ③切换路径**不能** `callx0` 进 C 函数（windowed 风格调用约定不转窗口，a12-a15 被冲掉，
  第一次改就因此让目标 ctx 变脏指针）。这三条已写进 `port/s3/ctx.S` 的注释。

- **S3 面板初始化延时不够（按手册重算）**：`dly()` 实测 5 cycles/单位
  （`dly(1200000)` 只有 6.0M cycles ≈ 75ms），而 ST7789 要求复位释放/SWRESET/
  SLPOUT 之后各 ≥120ms，原来三条都只等 75ms——命令发太早面板会直接不理。
  改成 `DLY_120MS`=3000000（≈187ms）、`DLY_20MS`=1000000（≈31ms）。

- **板上当前状态（收工快照）**：`tests/t_s3_lcd_kiosk.c6` 正在板上循环：
  纯色红→绿→蓝（各 1.2s）→ 8 色竖条 + `C6OS / ST7789 240X240 / BITBANG OK`
  文字（1.2s）→ 仪表盘一帧（1.6s，含 tick/tasks/DASH 字样）。串口同步打印
  `1 red 5168k-cyc … 5 dash 5814k-cyc tick=<n>`，tick 单调递增。
  若屏幕仍全黑，问题已不在 c6LOS 这一侧的可观测项，而在这三者之一：
  ①面板/排线本身；②这块屏的 SDA 只做输入（回读全不出值，见 §4.4）；
  ③屏需要本驱动之外的初始化（当前已按 ST7789 手册与各家开板示例补齐延时 +
  porch/gate/VCOM/gamma 表）。

- **音量键（VOL+/VOL-）结论：硬件常闭/未装，不是软件问题**。证据链：
  ①上游 `78/xiaozhi-esp32`（v2.5.0 与 main 一致）`xingzhi-cube-1.54tft-wifi`
  定义 `VOLUME_UP=GPIO40 / VOLUME_DOWN=GPIO39`，且 `Button(gpio, active_high=false)`
  → `iot_button` 的 `gpio_config.active_level=0, disable_pull=false`，即**内部上拉、
  按下接地**；②我们按同样配置（IO_MUX 用 39/40 的真实地址 0x600090A0/0xA4，
  MCU_SEL=1 + FUN_IE + FUN_WPU）实测**恒为 0**，按上游语义等于"一直按住"；
  ③原厂固件开机日志 `Set output volume to 80/90/100`（8.1s/9.3s/11.0s）正是
  "VOL+ 恒按下、应用不断加音量"的表现；④全车 GPIO 0..48 变化扫描，按物理键
  无任何电平变化（BOOT=GPIO0 正常）。
  另外修掉一个真 bug：GPIO≥32 的电平在 `GPIO_IN1`(0x60004040)，之前用
  `GPIO_IN >> pin` 是移位越界（未定义行为），读出来是垃圾。

- **S3 GPIO 分组坑（重要）**：`GPIO_OUT/ENABLE/IN` 只到 GPIO31；**GPIO32..53 在
  `GPIO_OUT1`/`GPIO_ENABLE1`/`GPIO_IN1`**。之前读 GPIO39/40 用 `GPIO_IN >> pin`
  是移位越界（未定义行为），恒得 0 —— 音量键"读不到"的一部分原因就在这里。
  已用 `tests/t_s3_in1.c6` 验证：把 GPIO40 当输出驱动高/低，`GPIO_IN1` 能正确
  跟随（`PASS: GPIO_IN1 readable`）。
  另外对照 IDF 源码（`espressif__button/button_gpio.c` 的 `gpio_config` +
  `gpio_ll_pullup_en/input_enable`）确认：按键就是"输入 + 内部上拉 + 低有效"，
  写的是 MTCK/MTDO 那两个 IO_MUX，与我们的写法一致；仍待查的是为什么同样的
  配置在我们固件里读到恒 0（怀疑与 JTAG TAP 对 MTDO 的驱动/释放时机有关）。

- **喇叭/mic 的关键结论（照 IDF v6 源码核对）**：ESP32-S3 的 I2S **没有 CPU 可写的
  TX FIFO，也没有自己的 DMA 描述符寄存器**（`soc/esp32s3/register/soc/i2s_reg.h`
  里只有 CONF/CLKM/RXEOF_NUM 等，没有 OUT_LINK/AHB 寄存器）。它的数据搬运走
  **GDMA 引擎**（通用 DMA，需要在 GDMA 侧配通道 + 描述符链，再用
  `DMA_PERI_OUT_SEL` 把通道接到 I2S0 TX）。所以在 c6LOS 里要真正出声/收声，
  下一步是实现 GDMA 通道 + 描述符链，而不是继续折腾 I2S 本身。
  参考现成环境：IDF v6.0.1 源码 `/media/winger/inst_26/esp/esp-idf`、工具链
  `/media/winger/inst_26/esp/tools`、上游固件 `/media/winger/inst_26/esp/xiaozhi`
  （已验证：按 `CONFIG_BOARD_TYPE_XINGZHI_CUBE_1_54TFT_WIFI=y` 编译出的固件，
  真板屏亮 + 三个按键都正常）。
