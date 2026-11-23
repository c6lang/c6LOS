# 测试与验收（四层）

> 状态：设计定稿 · 相关：[architecture.md](architecture.md) · [toolchain.md](toolchain.md) · [todo.md](todo.md)

---

## 1. 分层原则

| 层 | 跑什么 | 能证明什么 | 不能证明什么 |
|---|---|---|---|
| L1 host | 内核逻辑单测（虚拟时钟） | 状态机、链表、错误码、API 语义 | 抢占、真实中断、栈切换 |
| L2 rv32 (qemu-riscv32 或 risc6) | 同一套逻辑 + ILP32 ABI | 32 位指针/`u64`/结构体布局 | 抢占（risc6 可在指令边界注入机器定时器中断，qemu user-mode 不行） |
| L3 x86-64 裸机 (qemu-system) | 真抢占、ISR、上下文切换、嵌套中断 | 调度器与 port 的交互 | Xtensa 特有行为 |
| L4 ESP32-S3 真板 | 启动、控制台、tick、调度、屏、长稳 | 目标机全部行为 | —— |

**门禁**：L1/L2/L3 全绿才允许烧板；L4 通过才算里程碑完成。

---

## 2. 测试组织

```
tests/
  test.h              # ASSERT/ASSERT_EQ/EXPECT_*，输出 "N test(s), M failure(s)"
  t_task.c6           # 创建/删除/挂起/恢复/延时/join/优先级
  t_sched.c6          # 位图、时间片、同优先级轮转、抢占点
  t_tick.c6           # tick 换算、抖动、软定时器
  t_sem.c6            # 计数、超时、FIFO、ISR post
  t_mux.c6            # 递归、优先级继承、传递链、超时
  t_event.c6          # AND/OR/清除/读后清
  t_queue.c6          # 满/空/超时/元素拷贝/ISR put
  t_membox.c6         # 定长池耗尽、归属校验
  t_mem.c6            # best-fit、对齐、合并、统计、OOM
  t_api.c6            # 原生 API 与 LOS_* 同一断言集
  abi32.c6            # ILP32 边界（指针数组、函数指针、u64）
  stress.c6           # 多任务压力/长稳
  errors/             # 负向用例（参数校验、状态错误）
```

约定：

- 每个用例一个 `test "name" { ... }`；失败只计数不中止，最后统一退出码。
- 断言必须带上下文（`ASSERT_EQ(task.state, READY)` 打印两侧值）。
- 内核测试禁止浮点、禁止 libc、禁止 `@T`。
- **虚拟时钟**：host/sim 上 `os_test_tick(n)` 推进 tick 并跑超时/软定时器，
  不依赖 wall-clock；真机上用真实 `CCOMPARE0`。
- 每个对象至少 1 条「正向 + 超时 + 删除等待者」用例。

---

## 3. L1 host

```sh
make check-host
```

- `c6 --emit-llvm` 合并/逐 TU 出 IR → `clang -fuse-ld=lld` → 链接 `-ltcc1`。
- 时钟用虚拟 tick；`port/sim` 用 `ucontext` 只做**协作**切换，不模拟中断。
- 覆盖：全部逻辑用例 + 原生/兼容一致性 + 负向用例。
- 内存：ASan/UBSan 可用（host 允许）。`make check-host SAN=1`。

---

## 4. L2 qemu-riscv32（ILP32）

```sh
make run-rv32          # = make rv32 && qemu-riscv32 build/rv32/hello
```

- `c6 --emit-llvm` → `clang --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32`
  → `ld.lld -Ttext=0x10000 -e _start` → `qemu-riscv32`。
- 端口提供 `_start`、`ecall` 退出、`mret`/`mtvec` 最小陷阱（只验证 ABI，不验证抢占）。
- 覆盖：`abi32.c6` + 逻辑用例；输出与 L1 相同断言集。
- 已验证基线：`rv32: c6 kernel core says hello`，rc=0。
- 注意：qemu user-mode 不实现机器态 `mret`，`rv32-sched`/`rv32-tick` 这类
  走上下文切换的用例在 qemu 上会报非法指令，抢占验证要用 §4.1 的 risc6。

### 4.1 换用 risc6 跑同一批 ELF

```sh
make rv32 rv32-sched rv32-tick     # 与 qemu 用的是同一批产物
make run-rv32-emu                  # = ~/risc6/build/risc6 build/rv32/hello
make run-rv32-emu-sched
```

- `RISC6=/path/to/risc6` 可换路径；输出与退出码与 `qemu-riscv32` 逐字节一致
  （已核对 `hello`、`sched/sched`、`tick/sched`）。
- 比 qemu 多出的能力：`--bare` / ELF 内 `.risc6_bare` 段 → 机器态陷阱、`mret`、
  指令边界注入 MTIP（每 `RISC6_SLICE`=50000 条指令一次），可验证真抢占；
  `--stats` 给每条指令类别计数，`--time` 给 Minsn/s。
- 已知偏差见 `~/risc6/docs/design.md` §5.4：非对齐访问放行、`time` 由指令数
  换算（×64）、`brk` 固定窗口、单核 `lr/sc`、`fence` 为 nop、
  未知系统调用返回 `-ENOSYS` 并打印一次诊断。

---

## 5. L3 x86-64 裸机（真抢占）

```sh
make check-all         # 构建 L1/L2/L3 全部产物
make run-x86           # x86-64 裸机：真抢占/中断/上下文切换
```

- `examples/boot/` 的 512B 引导器 → 长模式 → payload@0x400000；PIT 100Hz；
  `sched_irq0` 陷阱入口压完整帧；qemu 退出码 `(code<<1)|1`，`code=0` → 1。
- 覆盖：抢占、嵌套中断、ISR 唤醒任务、时间片轮转、栈哨兵、上下文切换 1000 次往返。
- 断言：切换前后 callee-saved 全部一致；FPU 区（若开）逐字节一致；
  `os_sched_irq` 返回的 sp 必须 16B 对齐。

---

## 6. L4 ESP32-S3 真板

```sh
make s3 && make flash
```

上板阶梯（每步一个可观察输出，失败即停）：

| 步 | 观察 | 通过标准 |
|---|---|---|
| S3-0 | 备份原厂 flash | 16MB 读回，sha256 记录 |
| S3-1 | 最小镜像：`_start` 直接写 USB-Serial/JTAG | 串口看到 `c6os s3 boot` |
| S3-2 | 段/栈/bss 自检 | 打印 IRAM/DRAM 使用量、栈顶、`sizeof(TCB)` |
| S3-3 | CCOMPARE0 tick | 1 秒打印 100 个 tick（±1%） |
| S3-4 | 任务创建/切换 | 两个任务交替打印，计数器正确 |
| S3-5 | 抢占 | 高优先级任务被 tick 唤醒，延迟 < 100µs |
| S3-6 | 同步对象 | 与 L1 相同断言集在板上通过 |
| S3-7 | 屏 | 填充/文字/仪表盘；SPI 波形与刷新率 |
| S3-8 | 长稳 | 10 分钟压力，堆/栈哨兵无泄漏 |

板上断言输出：`c6os: 42 test(s), 0 failure(s)`；失败时打印任务名/PC/PS/a0–a15。

---

## 7. 确定性测试（不依赖时序）

- **单步调度器**：`os_test_set_ticks(0)` 后手工 `os_test_tick(1)`，断言每一步的就绪链。
- **可重复**：同一用例跑 100 次，就绪链顺序、唤醒顺序、返回码完全一致。
- **抖动上界**：记录 ISR 最大/最小周期（`CCOUNT`），报告 P99，不作为硬门禁（真板实测）。
- **压力**：`stress.c6` 用固定种子（`u32 lcg` 自实现）随机创建/删除/超时，跑 10^6 次操作，
  结束时对象计数归零。

---

## 8. 门禁清单（合并前）

```sh
make lint               # 4 个 lint + 符号白名单
make check-host         # L1 全绿
make check-rv32         # L2 全绿
make check-bare         # L3 全绿
make s3                 # 镜像可生成，size 在预算内
# L4 人工执行上板阶梯，记录串口日志到 build/logs/
```

- `git status` 干净；每个里程碑一个提交，提交信息含验收数字。
- 文档与代码同步：改 API 必须改 `docs/api.md`；改 port 接口必须改 `architecture.md §14`。
