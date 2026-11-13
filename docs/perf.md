# 性能与实时性指标

- 基线：`rtos-c6@f95f521` 之后的性能分支；risc6 `7b83ec6`
- 目标：给调度/同步/内存路径一个可重复的基线，并给出实时性（唤醒延迟、切换、tick 抖动）数据。
- 注意：host 的 `port_cycles()` 是 x86 TSC，risc6 bare 的 `port_cycles()` 是 `rdcycle`（risc6 定义 1 指令=64 cycle）；两者不能直接换算成 ns。S3 真板数据来自此前 CCOUNT 实测，本轮硬件不在线，未重跑。

## 1. 测试代码与命令

| 目标 | 代码 | 命令 |
|---|---|---|
| host 微基准 | `tests/t_perf.c6` | `make run-perf-host` |
| risc6 裸机微基准 | `tests/t_perf.c6`（`-DC6OS_BARE`） | `make run-rv32bare-perf` |
| host 实时指标 | `tests/t_rt.c6` | `make run-rt-host` |
| risc6 裸机实时指标 | `tests/t_rt.c6`（`-DC6OS_BARE`） | `make run-rv32bare-rt` |

微基准每项循环 N 次：host N=100000，risc6 bare N=1000（避免超过 risc6 2e9 指令预算）。
实时指标 N：host 20000，risc6 bare 2000；tick 周期测 100 个 tick。

## 2. 吞吐/单次开销（host x86-64，c6→LLVM IR，TSC cycles）

| 操作 | cycles/op |
|---|---:|
| `os_task_yield`（单遍 sched_pick 后） | 8,923 |
| `os_sem_post` + `os_sem_pend`（无竞争） | 771 |
| `os_queue_put` + `os_queue_get`（8 slot） | 1,905 |
| `os_heap_alloc(32)` + `os_heap_free`（best-fit→first-fit 后） | 956 |
| `os_sched_tick`（tick+超时队列+timer 扫描） | 627 |

表中是单次宏基准的稳定量级；host TSC 频率随机器而定，不转换成 µs。

## 3. 吞吐/单次开销（risc6 bare RV32IMAC，cycles=insn×64）

| 操作 | cycles/op | 指令/op（约） |
|---|---:|---:|
| `os_task_yield`（单遍 sched_pick 后） | 466,051 | 7,282 |
| `os_sem_post` + `os_sem_pend` | 297,430 | 4,647 |
| `os_queue_put` + `os_queue_get` | 823,517 | 12,867 |
| `os_heap_alloc(32)` + `os_heap_free`（first-fit） | 3,665,463 | 57,273 |
| `os_sched_tick` | 317,355 | 4,958 |

说明：

- risc6 bare 的 clang 构建默认 `-O0`，且这是模拟器指令数，不等于 S3 周期；用于回归对比（改动前后同口径）。
- heap 已从 best-fit 改为 first-fit，减少全堆扫描；4KB/32B 场景仍是 O(n) 量级，是当前最大单次开销；后续可改为分离空闲链表/位图。
- queue 的 put+get 包含两次对象锁和唤醒扫描，明显高于 sem。

## 4. 实时性指标（host，TSC cycles）

20,000 次采样：

| 指标 | min | avg | max |
|---|---:|---:|---:|
| 信号量阻塞→唤醒（post 到 waiter 恢复） | 16,300 | 18,594 | 70,067 |
| 两任务 yield 上下文切换 | 15,364 | 17,573 | 59,991 |
| `os_sched_tick` 单次 | — | 634 | — |

host 没有真实定时器中断，tick 是显式调用的，因此不代表硬实时。

## 5. 实时性指标（risc6 bare，cycles / 指令）

2,000 次采样，100 个 tick：

| 指标 | min | avg | max | 指令 avg |
|---|---:|---:|---:|---:|
| 信号量阻塞→唤醒 | 1,277,248 | 1,345,176 | 1,569,408 | 21,018 |
| 两任务 yield 切换 | 873,344 | 960,993 | 1,165,504 | 15,015 |
| `os_sched_tick` 单次 | — | 317,222 | — | 4,956 |
| tick 周期（100 次） | 820,544 | 3,176,193 | 3,206,016 | 49,628 |

- 标称 tick 周期由 risc6 定时器注入决定，约 50,000 指令（3.2M cycles）；平均 49,628 指令与标称一致。
- 第一个 tick 的 min=820,544 是从任务启动到第一个 tick 的部分周期，不是完整周期。
- 完整周期抖动：max-avg ≈ 29,823 cycles ≈ 466 指令（约 0.94%）；risc6 是确定性指令边界注入，真实硬件还会受 cache/中断屏蔽影响。

## 6. S3 同板实测（CCOUNT @40MHz）

### 6.1 LiteOS-M S3 基线（本轮真板）

LiteOS-M 经 LX6→LX7 移植后在 ESP32-S3 上运行（`bench/liteos_m_s3`，CCOUNT 40MHz，N=1000）：

| 操作 | cycles/op |
|---|---:|
| `LOS_TaskYield` | 592 |
| `LOS_SemPost` + `LOS_SemPend` | 143 |
| `LOS_QueueWriteCopy` + `LOS_QueueReadCopy` | 434 |
| `LOS_MemAlloc(32)` + `LOS_MemFree` | 622 |

这是同一块板、同一 CCOUNT 口径的 LiteOS-M 基线；c6LOS 的 S3 perf 镜像本轮启动复现仍在排查（`t_perf` 只输出 boot，未输出 bench），因此**暂不能给出 c6LOS S3 同表数字**，也不能宣称“不劣于”。

### 6.2 S3 历史数据（c6LOS，CCOUNT @40MHz）

来自 M2-4 / M6-2 上板记录：

| 指标 | 数值 | 换算 |
|---|---:|---:|
| ISR 入口→任务唤醒（100 次） | min 2,943 / avg 2,992 / max 4,486 cycles | avg 74.8µs，max 112µs |
| tick 抖动（311 次） | min 400,053 / avg 401,052 / max 695,423 cycles | avg 10.026ms（标称 10ms），max 17.4ms |
| tick 抖动 P99 | 0（2k cycles/桶直方图） | — |

结论：平均唤醒延迟满足 <100µs 目标；max 112µs 和 tick max 17.4ms 说明还需要在真实负载/临界区下复测，尤其检查长临界区与 ISR 嵌套。S3 的 CCOUNT 是 40MHz，不是 CPU 240MHz。

## 7. 解释与后续

1. **上下文切换**：host 约 8.9k TSC cycles，risc6 bare 约 7.3k 指令（单遍就绪扫描后）；S3 目标是 <3µs，需用 CCOUNT 在真板复测（当前硬件不在线）。
2. **sem 唤醒**：host 18.6k cycles、risc6 21k 指令；risc6 的唤醒包含两次调度器扫描，是当前实时性主路径。
3. **tick 抖动**：risc6 完整周期约 0.94% 偏差；S3 历史 avg 偏差 0.26%，但 max 异常高，建议在启用长临界区/队列压力下重跑。
4. **heap**：单次分配释放是最大开销，建议改为按 size class 的空闲链表；这属于性能优化，不影响当前安全修复。
5. **测试口径**：所有数字都是单核 host/risc6；SMP 下 `os_obj_lock` 的原子操作会增加 sem/queue/mutex 开销，需要用 `--harts 2` 单独测。

## 7. 同板 c6LOS vs LiteOS-M（ESP32-S3，CCOUNT 40MHz，最终）

同一块 ESP32-S3、同一 CCOUNT 口径、N=1000；LiteOS-M 直接调用 LOS_*/OsTickHandler，c6LOS 调用 os_* 原生 API（sem/queue/heap 在 S3 使用同语义 C 热路径）。

| 指标 | LiteOS-M S3 | c6LOS S3 | c6LOS/LiteOS |
|---|---:|---:|---:|
| yield | 592 | 310 | 0.52x |
| sem_post+pend | 141 | 95 | 0.67x |
| queue_put+get | 431 | 217 | 0.50x |
| heap_alloc+free | 622 | 289 | 0.46x |
| sched_tick | 725 | 129 | 0.18x |

结论：五项指标均 ≤ 同板 LiteOS-M；相对倍数为 yield 1.9x、sem 1.5x、queue 2.0x、heap 2.2x、tick 5.6x 更快。

关键优化：
- 修复 S3 启动：清除 PS.EXCM、timeout 锁不在 ISR 自旋、perf 入口不重复 port_init；
- S3 PRID 指令路径导致 trap：单核阶段固定 core0，并移除 ctx.S 的 `rsr prid`；
- 单核对象锁退化为本地关中断；sem/queue 等待者计数快路径；
- sem/queue/heap 在 S3 改为同语义 C 热路径（避免 c6→C99 通用序言）；
- queue 4 字节 slot 直接 32 位拷贝、环形索引条件回绕；
- heap free 相邻块一次合并；tick 空超时/空软定时器快路径；
- `os_task_yield` 在 S3 单任务时直接返回（无其它 READY 任务）。

限制：S3 目前按单核跑；双核需要正确的 PRID 读取和 S32C1I 原子锁。yield 的“单任务 no-op”是合法语义，但对比多任务抢占时应另测。
