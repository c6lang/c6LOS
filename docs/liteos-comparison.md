# c6LOS 与 OpenHarmony LiteOS-M 对比

- c6LOS 基线：`rtos-c6@734362d`（性能/实时性测试见 `docs/perf.md`）
- 对比对象：OpenHarmony `kernel_liteos_m`（LiteOS-M 轻量内核）
- 说明：这不是同硬件、同编译器、同负载的 benchmark。c6LOS 的数据来自本仓库 host/risc6/S3；LiteOS-M 使用公开文档与已发表数据，只有明确标注的地方才是实测。**不能把两张表直接相减得出“谁快多少”**。

## 1. 定位

| 维度 | c6LOS | LiteOS-M |
|---|---|---|
| 语言 | c6（C 的扩展） | C |
| 目标 | M0–M8 内核核心 + LOS_* 双 API 子集 | 面向 MCU 的完整轻量内核 |
| 主要验证 | x86-64 host、risc6 RV32 bare、ESP32-S3 真板 | Cortex-M/RISC-V/Xtensa 等多架构；OpenHarmony 设备 |
| 代码规模 | S3 镜像 text 21,091B / data 5,760B / bss 34,720B（含任务栈、日志、shell、gfx） | 非裁剪内核核心约 6–10KB ROM（社区估算）；完整 bundle 可达 300KB ROM/100KB RAM；Mini 系统最低 128KiB RAM |
| API 面 | 42 个 LOS_* 函数（task/sem/mux/event/queue/membox/mem/swtmr/hwi/tick） | 完整 LiteOS-M API（任务锁/优先级设置、队列 copy/query、mem realloc/align、event poll、tickless 等） |

## 2. 功能矩阵

| 能力 | c6LOS | LiteOS-M | 差距/说明 |
|---|---|---|---|
| 优先级调度 | 32 级，位图 + 同优先级轮转 | 32 级，0 最高，同优先级时间片 | 基本对齐；c6LOS 的任务表是固定 16 槽线性扫描 |
| 抢占 | tick 100Hz，S3/rv32 抢占 | tick 可配置，支持抢占 | c6LOS 没有 tickless |
| SMP | rv32/S3 每核 ctx、静态绑核/迁核、跨核 sem | 主流 LiteOS-M 以单核 MCU 为主 | c6LOS 的 SMP 是实验性，任务表/超时仍有短临界区假设 |
| 软定时器 | 16 个，tick 回调 | 可配置数量，tick 回调 | c6LOS 无 tickless/低功耗定时 |
| sem/mutex/event | 固定 8/4/4 个 | 数量由 `LOSCFG_BASE_IPC_*_LIMIT` 配置 | 上限硬编码，错误码只保留最小集 |
| queue | 4 个队列 × 8 slot × 64B | 可配置；有 Read/Write 与 ReadCopy/WriteCopy、Query | c6LOS 只有 copy 语义的 `LOS_QueueRead/Write`，长度语义已修正 |
| membox | 4 个池，块大小/数量调用者给 | membox 组件 | 无 realloc/align |
| 动态内存 | 最多 4 个 pool，best-fit 线性扫描 | bestfit/membox，多区域 | c6LOS 的 alloc/free 是 O(n)，性能是弱项 |
| HWI | 32 个注册槽，忽略 prio/mode | HWI 创建/删除/使能/触发、优先级/模式 | c6LOS 的 HWI 只是分发层，未接完整中断控制器抽象 |
| 异常/陷阱 | S3 向量表 + panic 帧；rv32 trap | 完整异常框架、栈回溯 | c6LOS 的 trap 诊断较简 |
| MPU/隔离 | 无 | 可选 MPU（`LOSCFG_MPU_ENABLE`，默认关闭） | **最大安全差距**：c6LOS 任务同地址空间同特权 |
| 低功耗/tickless | 无 | `LOSCFG_KERNEL_TICKLESS`/电源组件 | 未实现 |
| 多架构 | S3 Xtensa、rv32、x86-64、sim | Cortex-M、RISC-V、Xtensa、C-SKY 等 | c6LOS 端口少，但 c6→C99/LLVM 链路有可移植性 |
| 工具链 | c6 → C99（S3）/LLVM IR（risc6/x86） | 各架构 GCC/Clang | c6LOS 依赖 c6 编译器 |

## 3. API 兼容层差异

c6LOS 的 `LOS_*` 是**子集兼容**，不是 drop-in 替换：

- 已有：`LOS_TaskCreate/Delete/Delay/Suspend/Resume/Yield/IDGet/Count`，`LOS_Sem*`，`LOS_Mux*`，`LOS_Event*`，`LOS_Queue*`，`LOS_Membox*`，`LOS_Mem*`，`LOS_Swtmr*`，`LOS_Hwi*`，`LOS_Tick*`。
- 缺失/简化：`LOS_TaskLock/Unlock`、`LOS_TaskPriSet/Get`、`LOS_TaskSwitchInfoGet`、CPUP、`LOS_QueueReadCopy/WriteCopy/Query`、`LOS_MemRealloc/AllocAlign`、`LOS_EventPoll`、`LOS_HwiTrigger`、完整 `LOS_ERRNO_*` 映射、tickless/低功耗接口。
- 错误码：c6LOS 大多返回 0/-1/-2/-3，再映射到少量 LOS 错误码；LiteOS-M 的细粒度错误码（如 `LOS_ERRNO_QUEUE_READ_SIZE_TOO_SMALL`）没有完整复刻。
- 语义：`LOS_QueueRead` 在 LiteOS-M 返回消息地址，`LOS_QueueReadCopy` 才复制；c6LOS 的 `LOS_QueueRead` 是复制语义，**同名不同义**，移植时要注意。

## 4. 性能/实时性数据

### 4.1 c6LOS 实测（详见 `docs/perf.md`）

host x86-64（c6→LLVM，TSC cycles/op，N=100k）：

| 操作 | cycles/op |
|---|---:|
| task yield（一次切换） | 9,057 |
| sem post+pend | 771 |
| queue put+get | 1,905 |
| heap alloc+free | 984 |
| sched_tick | 627 |

risc6 bare RV32IMAC（cycles=insn×64，N=1000）：

| 操作 | cycles/op | 指令/op |
|---|---:|---:|
| task yield | 618,699 | 9,667 |
| sem post+pend | 297,430 | 4,647 |
| queue put+get | 823,517 | 12,867 |
| heap alloc+free | 3,674,274 | 57,410 |
| sched_tick | 317,355 | 4,958 |

S3 真板历史（CCOUNT @40MHz；本轮硬件不在线未重跑）：

| 指标 | 数值 |
|---|---|
| ISR→任务唤醒 | min 2,943 / avg 2,992 / max 4,486 cycles（avg 74.8µs，max 112µs） |
| tick 周期 | avg 401,052 cycles（10.026ms，标称 10ms）；max 695,423（17.4ms） |
| tick P99 偏差 | 0（2k cycles/桶） |

### 4.2 LiteOS-M 公开数据

- 一篇 MIPS/Loongson LS1J/LS1C 移植论文给出：**任务上下文切换 0.229µs，interrupt response 4.73µs**（不同架构、不同测量方法）。
- 厂商/博客常见“调度延迟 <1µs”“中断响应微秒级”，但缺少可复现方法，本文不作为硬指标。
- LiteOS-M 提供 `LOS_TaskSwitchInfoGet()`、CPUP 等统计设施，便于在目标板上做同口径测量；c6LOS 目前没有等价任务切换统计/CPUP。

### 4.3 能否说“c6LOS 比 LiteOS 快/慢”？

不能直接比：

- 指标口径不同：c6LOS 的 74.8µs 是 **定时器 ISR→任务唤醒**，LiteOS 的 0.229µs 是 **任务上下文切换**，不是同一事件。
- 架构/主频不同：S3 240MHz vs LS1J MIPS；CCOUNT 40MHz vs CPU 周期。
- 编译器/优化不同：c6LOS 的 risc6 构建是 `-O0`，S3 是 `-Os`；LiteOS 通常 `-Os` 且上下文切换为手写汇编。
- 负载不同：c6LOS 的数字是微基准，LiteOS 论文未给同等负载。

**可以得出的结论**：

- 功能完整度、端口成熟度、功耗/MPU/统计设施，LiteOS-M 明显领先。
- c6LOS 的 sem/queue/heap 微基准在同口径下可复现，但 heap 的 O(n) 线性扫描是明显性能弱项（risc6 约 57k 指令/次 alloc+free）。
- c6LOS 的 S3 唤醒 avg 74.8µs 满足其 <100µs 目标，但 max 112µs、tick max 17.4ms 说明长尾需要优化；LiteOS 的 tickless/低功耗路径在 idle 场景更有优势。

## 5. 实时性机制对比

| 机制 | c6LOS | LiteOS-M |
|---|---|---|
| 调度器 | 16 槽任务表 + 32 位就绪位图；同优先级轮转 | 每优先级就绪链表 + 位图；同优先级时间片 |
| tick | 100Hz，超时队列按 deadline 排序 | 可配置 tick；tickless 可停 SysTick |
| 上下文切换 | rv32 汇编保存 ra/s0-s11/sp；S3 80B 陷阱帧 | 各 arch 手写 `context_switch`，通常更精简 |
| ISR→任务 | 唤醒后由 IRQ 退出路径切换；S3 实测 avg 74.8µs | 提供 `LOS_TaskSwitchInfoGet`/CPUP 统计；公开 MIPS 数据 4.73µs IRQ 响应 |
| 优先级继承 | mutex PI，带环检测 | mutex 支持 PI（具体实现随版本） |
| SMP | 实验性静态分区 + 跨核 sem | 主流单核 MCU；SMP 不是其定位 |

## 6. 安全/可靠性对比

| 项 | c6LOS | LiteOS-M |
|---|---|---|
| MPU/任务隔离 | 无（同地址空间同特权） | 可选 MPU，按任务配置 region；默认关闭 |
| 对象生命周期 | 新增 destroy/唤醒等待者；timer 注销；仍有 caller-owned buffer 契约 | 成熟的对象/句柄管理 |
| 错误处理 | 最小错误码；部分 API 参数校验已加固 | 细粒度 `LOS_ERRNO_*`、参数校验完整 |
| 测试 | host 39 PASS、risc6 6 用例、S3 真板若干 | OpenHarmony 大量单元/集成/认证测试 |
| 形式化/认证 | 无 | 有商用认证/功能安全实践（取决于产品） |

## 7. 结论与建议

1. **定位不同**：c6LOS 是用 c6 从零实现的 LiteOS-M 子集，优势是 c6 语言/双 API/确定性 risc6 回归；LiteOS-M 是成熟量产内核。
2. **功能差距**：tickless、MPU、完整 API/错误码、HWI 抽象、任务统计/CPUP、低功耗、更多架构端口。
3. **性能差距**：没有同口径数据；c6LOS 的 heap 线性扫描和调度器线性扫描是主要优化点；上下文切换需要 S3 CCOUNT 实测后再与 LiteOS 比。
4. **安全差距**：c6LOS 无 MPU，不能作为不可信任务隔离边界；LiteOS-M 的 MPU 也默认关闭，但具备该能力。
5. **下一步建议**：
   - 在 **同一块 S3** 上移植/运行 LiteOS-M（若其 Xtensa 端口可用），用相同 CCOUNT 测试 harness 对比 ISR 延迟、切换、tick 抖动；
   - c6LOS 先做 heap size-class、任务表就绪链表、tickless 三个优化；
   - 补 `LOS_TaskSwitchInfoGet`/CPUP 等价统计，才能和 LiteOS 的公开指标同口径。

## 8. 参考

- OpenHarmony `kernel_liteos_m` 任务管理文档（32 优先级、时间片、抢占）。
- OpenHarmony LiteOS-M `LOSCFG_MPU_ENABLE`/`los_mpu.c`、`LOSCFG_KERNEL_TICKLESS` 配置与 tickless 接口。
- LiteOS-M API 文档：`LOS_TaskCreate`、`LOS_QueueRead/ReadCopy`、`LOS_MemAlloc/Realloc/AllocAlign` 等。
- MIPS/Loongson LiteOS-M 移植论文：上下文切换 0.229µs、中断响应 4.73µs（仅作跨架构参考）。
- 本仓库 `docs/perf.md`：c6LOS host/risc6/S3 实测与测试方法。
