# c6LOS 架构设计（ESP32-S3）

> 状态：设计定稿（施工前评审版）· 相关：[toolchain.md](toolchain.md) · [api.md](api.md) · [todo.md](todo.md)

---

## 1. 定位与目标

`c6LOS` 是 **LiteOS-M 语义**的实时内核，用 c6 重写，而不是 LiteOS 源码的逐行翻译：

- 对外：`LOS_*` C ABI 与 LiteOS-M 同名同义，现有 LiteOS-M 示例/移植指南可复用；
- 对内：`os_*` c6 原生 API，用 `T!error` / `defer` / 泛型 / 切片 / `interface` 表达内核约束，
  让「半初始化对象」「漏解锁」「队列类型错配」在编译期或断言处暴露。

| 项 | 目标 |
|---|---|
| 目标机 | ESP32-S3（Xtensa LX7 双核 240MHz，512KB SRAM，8MB PSRAM，16MB flash） |
| 调度 | 32 优先级抢占 + 同优先级时间片，tick 100Hz，切换开销目标 < 3µs（240MHz，实测为准） |
| 内核对象 | 任务、信号量、互斥量（优先级继承）、事件、队列、定长内存池、软定时器 |
| 内存 | 内核 ≤ 256KB SRAM；静态池优先；best-fit 堆可选；每任务栈哨兵；内核态无 `malloc` |
| 确定性 | 所有阻塞调用可超时；ISR 只做最小工作；关中断临界区有界 |
| 可移植 | 内核核心零架构代码；port 只含指针/`u32`/`u64`/函数指针 |
| 参考机 | x86-64 裸机（抢占回归）、qemu-riscv32（ILP32 回归）、host（逻辑单测） |

### 非目标

默认形态下仍然是非目标：LiteOS-A 的进程/MMU/POSIX、文件系统、动态加载、网络中间件；
内核内的 `@T` GC、`async/co`、闭包、`f".."`；Wi-Fi/BLE/音频（属应用层，二期）。

> **修订（Linux ABI personality）**：进程地址空间 / ELF 动态加载 / POSIX 不再是绝对的
> 非目标，而是由 `C6OS_LINUX_ABI` 门禁的**可配置 personality**，只在具备
> `MMU|USER|IRQ_USER` 能力的移植层（x86-64 裸机）上参与编译；ESP32-S3 上恒关，且
> 相关代码**不在编译单元列表里**（不是 `#if 0`）。设计与验收见
> [linux-abi.md](linux-abi.md)。
>
> 这同时修订了 §1 的"单地址空间"表述：**默认单地址空间；打开 personality 时内核
> 同时支持两种执行域**（内核任务 / 用户任务），两者共用同一调度器与优先级模型。

---

## 2. 分层与依赖规则

```
app/        shell demo screen                      c6
  ↓ os_* / LOS_*
kernel/     task sched tick swtmr sem mux event
            queue membox mem hwi panic log         c6
  ↓ port.h（指针/u32/u64/函数指针）
port/       sim | x86_64 | rv32 | s3               C + .S
  ↓
rt/         c6rt：mem*、软除法、err 状态、pthread 8 入口   C
```

硬规则（CI 检查）：

1. `kernel/`、`app/` 只能是 `.c6`；`port/`、`rt/` 只能是 `.c`/`.S`。
2. 跨 C ABI 只允许 `void*`、`u32`、`u64`、函数指针。**禁止** `long`、`size_t`、
   `long double`、按值结构体、数组跨边界。
3. `port` 不得读写内核的 `struct OsTask`（TCB 对 port 不透明，port 只持有 `void *sp`）。
4. 内核核心禁止浮点（lint）；S3 工具链**没有软浮点开关**（`-msoft-float`/`-mno-fpu`
   均不被接受，`float` 直接生成 `add.s`），因此陷阱帧必须保存 `f0–f15` + `FCR/FSR`。
   浮点只允许出现在应用任务；内核核心仍然禁止。
5. 内核核心禁用 c6 特性：`@T`、`async/co`、函数字面量/闭包、`f".."`、libc 头。
6. 所有 `.c6` 源文件单独出 C（每个 TU 一个 `.c`），目标链接期不允许未声明符号。

---

## 3. ABI 模型：c6 是 LP64 前端，S3 是 ILP32

c6 前端的类型模型固定为 LP64（`long`/`size_t`/指针按 8 字节算偏移），Xtensa 与 rv32 都是
ILP32。c6 代码内部自洽（所有偏移由同一个前端计算），危险只在跨边界与依赖 C 语义处。

| 场景 | 结论 |
|---|---|
| c6 内部的 `struct`（含指针成员、指针数组） | 安全：读写都用同一套 LP64 偏移 |
| 指针作为参数/返回值在 c6 之间 | 安全：生成的 C 里是普通指针 |
| `void*`/`u32`/`u64`/函数指针跨到 C port | 安全：两侧都是指针/i32/i64 |
| `long`/`size_t` 跨边界 | **禁止**：c6 里是 64 位，C 侧是 32 位 |
| 按值结构体跨边界 | **禁止**：布局不同 |
| `long double` | **禁止**：需要软浮点/compiler-rt |
| `printf`/`qsort` 等 libc 原型 | **禁止**：`size_t`/`va_list` 不匹配；`kprintf` 放在 `rt/kprintf.c`（C + va_list），c6 侧只留 `kputc/kputs` |

制度：`tools/lint-ilp32.sh` 扫描 kernel/app/include；`tests/abi32.c6` 在 qemu-riscv32
上专项验证（指针数组、结构体内的指针、函数指针、回调、`u64` 运算、切片、泛型队列）；
S3 上用同一套断言集复验（上板断言集）。

> 说明：走 C99 后端时，c6 的 `long` 仍然按 64 位输出（`long long`），所以上述规则同样适用。

---

## 4. 启动链（ESP32-S3）

```
上电
 → ROM 从 flash 0x0 读 esp_image（我们就是二级引导器）
 → ROM 按段头把 .iram/.dram 段拷到目标地址，跳到 entry
 → port_start_asm:
      关看门狗（TIMG0/RTC WDT）
      设 SP（DRAM 顶，16B 对齐）
      设 VECBASE、初始化 PS/中断级别
      清 .bss
 → port_init: 控制台（USB-Serial/JTAG 默认，UART0 备选）、CCOMPARE0 时钟、中断
 → rt_init:   装 8 个 pthread 入口 + calloc/free + mem*
 → os_init:   对象池、空闲任务、主任务（当前上下文）
 → os_start:  开中断，进入调度（rfi 到首个任务）
```

约束：

- `rt_init()` 必须先于任何可能用到 `err`/`try` 的 c6 代码（`tcc_th()` 会调
  `pthread_key_create`）。
- 所有段都在 SRAM（IRAM/DRAM）；**不启用 flash XIP/MMU**，M0–M6 不初始化 PSRAM。
- 镜像必须能通过 `esptool elf2image` 生成合法 esp_image；烧到 `0x0`。
- 上板前先备份原厂 flash（16MB），烧写失败可完整恢复。

---

## 5. 内存预算（S3：512KB SRAM + 8MB PSRAM）

| 区域 | 预算 | 说明 |
|---|---|---|
| TCB | 16 × 192B ≈ 3KB | `C6OS_MAX_TASKS=16` |
| 任务栈 | 16 × 8KB = 128KB | 主任务 16KB；空闲任务 2KB |
| 队列 | 8 个控制块 + 元素区（默认 16×32B/个）≈ 5KB | 元素大小创建时确定 |
| 信号量/互斥/事件/软定时器 | < 4KB | 静态池 |
| 内核堆 | 64KB（可配） | best-fit，供 `LOS_MemAlloc` 与应用 |
| 控制台/日志 | 4KB | 环形日志 + kprintf 行缓冲 |
| **内核合计** | **< 210KB** | 编译期 `sizeof` 断言 + 启动自检 |
| 帧缓冲（M7） | 240×280×2B ≈ 131KB | 内部 SRAM；若不够再换 PSRAM |
| PSRAM | 8MB | 应用/图形/大缓冲，M7 之后启用 |

所有大块内存用 `static` 数组（c6 对 `static` 豁免“必须初始化”，零初始化进 `.bss`）。

---

## 6. 对象模型

### 6.1 任务（TCB）

```c
struct OsTask {
    void       *sp;            /* 调度上下文，port 私有布局 */
    struct OsTask *ready_prev, *ready_next;   /* 同优先级就绪环 */
    struct OsTask *all_prev,  *all_next;      /* 全局任务链 */
    struct OsWait  wait;       /* 等待节点（对象 pend + 超时队列双归属） */
    void      (*entry)(void *);
    void       *arg;
    u32         stack_base;    /* 低地址（32 位地址空间） */
    u32         stack_size;
    u32         id;
    u32         magic;         /* 0xC6C0DE01 */
    u16         prio;          /* 0..31，越小越高 */
    u16         base_prio;
    u8          state;         /* READY/RUNNING/BLOCKED/SUSP/DEAD */
    u8          suspend_cnt;
    u8          timeslice;
    u8          flags;
    char        name[C6OS_NAME_MAX];   /* 16 */
    void       *keys[C6OS_KEYS];       /* c6 运行库每任务状态 */
};
```

- **不变量**：`READY` ⇔ 在某个就绪环里；`BLOCKED` ⇔ `wait.obj != 0` 且在对象 pend 链里；
  `wait.deadline != 0` ⇔ 同时挂在超时队列里。
- 自删除：置 `DEAD` + 进 `reap` 链，由调度器在切换后回收（不能在自己的栈上释放栈）。
- 栈哨兵：栈底 8 字节写 `0xC6C0DEAD`，每次切换检查；命中 → `os_panic`。

### 6.2 等待节点与超时

```c
struct OsWait {
    void  *obj;
    u8     kind;        /* WAIT_SEM / WAIT_QUEUE_WRITE / ... */
    u8     result;      /* OK / TIMEOUT / DELETED */
    u16    pad;
    u64    deadline;    /* tick；0 表示不超时 */
    struct OsWait *prev, *next;   /* 对象 pend 链 */
    struct OsWait *tprev, *tnext; /* 超时队列 */
};
```

唤醒统一走 `wait_finish(w, result)`：从两处链表摘除、置结果、任务入就绪环。
**竞争处理**：`post` 与超时只可能在同一个关中断临界区里发生，谁先摘除谁决定结果。

---

## 7. 调度器

- 就绪位图 `u32 ready_bit` + `ready[32]` 环形双向链；选最高优先级用 `clz`。
- 同优先级：时间片轮转，默认 `C6OS_TIMESLICE=10` tick；只由 tick 递减。
- 抢占点：tick ISR、`post`/`event` 唤醒、`os_task_resume`、更高优先级任务创建。
- 主动让出：`os_task_yield()` → `port_yield()`（S3：`syscall`/软件中断；sim：`swapcontext`）。
- 空闲任务：优先级 31，`port_idle()`（S3：`waiti 0`；sim：推进虚拟时钟）。
- SMP：`cur` 每核一份；就绪队列分片 + IPI（M8，单核 M0–M7）。

内核给 port 的调度入口：

```c
void  os_tick_irq(void);              /* 时钟：tick++、软定时器、超时、时间片 */
void  os_resched_irq(void);           /* 标记需要重调度 */
void *os_sched_irq(void *frame);      /* 保存当前 frame、选下一个、返回其 sp */
void *os_sched_coop(void *next_sp);   /* 协作 port：返回被换出任务的旧 sp */
```

port 切换路径：

- **抢占式 port（S3/x86）**：陷阱入口压完整寄存器帧 → `os_tick_irq()` →
  `os_sched_irq(sp)` → 用返回的 sp 恢复现场并 `rfi`/`iretq`。
- **协作 port（sim/rv32）**：`os_sched_coop(next->sp)` → port 汇编保存 callee-saved 并换栈。

### Xtensa 上下文（call0 ABI）

- 编译加 `-mabi=call0`：a0=RA、a1=SP、a2–a11 参数/临时、a12–a15 callee-saved，
  没有窗口溢出异常，切换简单（实测生成 `call0`/`ret.n`；默认 windowed 生成
  `call8`/`retw.n`）。
- 陷阱帧：`a0–a15`、`pc`、`ps`、`sar`、`lbeg`、`lend`、`lcount`（循环寄存器）、
  `f0–f15`、`fcr`、`fsr`（S3 默认硬浮点，必须保存）。
- 中断入口：`VECBASE + level*64` 放跳板 → 公共入口；定时器用 `CCOMPARE0`，
  写回 `CCOMPARE0` 清中断；发 EOI 后再调度。
- `rfi <level>` 返回；`port_frame_new` 为新任务伪造同样的帧（pc=入口跳板）。

---

## 8. 时间

- `u64 tick` 单调递增，100Hz；`os_tick_get`、`os_ms_to_ticks`（向上取整）、`os_ticks_to_ms`。
- 超时队列按 `deadline` 有序（插入排序，n 小；M8 可换时间轮）。
- 软定时器：一次/周期；回调在 tick ISR 里执行，**禁止阻塞/分配**。
- 抖动测量：`port_cycles()`（Xtensa `CCOUNT`）记录 ISR 进出周期差，保留最大值。

---

## 9. 同步原语语义

| 对象 | 语义 |
|---|---|
| 信号量 | 计数 0..max；`pend(timeout)` 减一或阻塞；`post` 唤醒最高优先级等待者或加计数；同优先级 FIFO |
| 互斥量 | 递归（`count`）；持有者释放后移交所有权；**优先级继承**：owner 的 `prio = max(base, 所有等待者)`，解锁时重算；禁止 ISR pend |
| 事件 | 32 位标志；AND/OR；`clear` 按位清除；可选读后清 |
| 队列 | 固定元素大小/深度；写满阻塞、读空阻塞；超时；读写等待者分队列 |
| 内存池 | 定长块 O(1)；`alloc` 无阻塞；`free` 校验归属；耗尽 `OS_ENOMEM` |

ISR 可用：`os_sem_post`、`os_event_set`、`os_queue_put`（非阻塞）、`os_task_resume`。
ISR 禁止：`pend`/`delay`/`delete`/`mem_alloc`/`kprintf`（panic 除外）。

---

## 10. 错误模型

- 内核内部与 C 边界：`int` + `enum OsError`（`OS_OK=0`，负值错误）。
- c6 原生 API：`T!error` + `err(id,"msg")`；返回值必须能被 `try/catch` 消费；
  **禁止可能合法为 -1 的接口用 `!error`**（改用输出参数 + 状态码）。
- 兼容层：`UINT32`，`LOS_OK=0`，错误码按 api.md 映射。
- 错误槽每任务状态由 `rt/` 提供（`pthread_key` → TCB `keys[]`）；ISR 禁用 `err`/`catch`。

---

## 11. 控制台与日志

- `interface Console { int put(Console *self, int ch); int get(Console *self); }`。
- S3 默认控制台：**USB-Serial/JTAG**（USB_SERIAL_JTAG FIFO 轮询写，插同一根 USB 线即可）；
  备选 UART0（GPIO43/44，115200）。sim：stdout/管道。
- `kprintf`：自研最小格式化（`%d %u %x %s %c %p %llu`，宽度/补零），不依赖 f-string，不分配。
  实现放在 `rt/kprintf.c`：c6 后端的 `va_arg` 在 LLVM IR 路径上实测不可靠（host 输出错位），
  由 C 侧接收 `va_list` 并回调 c6 的 `kputc/kputs`；目标侧同一份 C 由 xtensa-gcc 编译。
- 环形日志 4KB，panic 时同步刷出并打印陷阱帧（pc/sp/ps/a0-a15/任务名）。
- shell 在最低优先级任务里跑，命令表用 `interface ShellCmd`。

---

## 12. c6 特性使用矩阵

| 特性 | 用在哪 | 规则 |
|---|---|---|
| `defer` / `errdefer` | 内核函数（锁、半初始化回滚） | 可放心用；状态在帧里 |
| `T!error` / `try` / `catch` | 原生 API 表层、app | 内核内部用 `int`；ISR 禁用；避免 -1 合法值 |
| 泛型 `@T` | `Queue@T` / `Vect@T` 包装 | 单态化实例集中在一个 TU，避免重复定义 |
| 切片 `T[:]` | app/shell 的缓冲区 | 内核核心用 `(buf,len)`；切片运行时由 `rt/` 提供 |
| `interface` | Console / ShellCmd / Display | vtable 编译器生成；跨 TU 各自生成，安全 |
| `module` / `import` | 可选，仅 app 层 | 内核核心不加 module（C ABI 需要裸符号） |
| `test "..." {}` | 四层测试 | 测试二进制合并编译；内核按 TU 编译 |
| 禁用 | `@T` GC、`async/co`、闭包、`f".."`、内联/全局汇编 | 后端缺口 + 确定性/内存要求 |

### 12.1 为什么内核不用 `@T` GC（决策记录）

`@T` 在 c6 里是**编译器插桩的引用计数**：retain/release 由编译器在赋值、传参、返回处
自动插入，运行时在 `tcc1`。它对应用代码很好用，但和内核对实时性、内存、错误模型的
要求正面冲突：

1. **最坏路径不可界定**：一次 release 可能递归释放整棵对象图，耗时与深度成正比；
   调度、`post`、tick 路径要求 O(1) 且可测。
2. **插桩点不可控**：编译器决定在哪 release；内核需要精确的临界区边界（例如
   `post` 与等待者摘链必须在同一个关中断区），插桩可能把释放挪到临界区外。
3. **并发代价**：双核 + ISR 并发要求原子引用计数；Xtensa 只有 `s32c1i`，
   要么关中断要么自旋，直接拉长 ISR 和调度临界区。
4. **循环引用**：RC 不回收环；内核对象图天然有环（任务↔等待节点↔对象↔就绪链），
   加 cycle collector 更不可控，靠人保证无环又不如显式生命周期。
5. **根与语义所有权**：GC 的“根”是内存引用；内核的“所有权”是语义的
   （mutex owner、queue 元素、等待节点双归属）。编译器看不到这些不变量，
   TCB/链表/ISR 栈里的裸地址也无法安全扫描。
6. **分配器不匹配**：`@T` 依赖运行库堆（host 是 libtcc1 的 arena）；目标侧是
   静态对象池 + best-fit，ISR 禁止分配，GC 的隐式分配点无法满足。
7. **错误模型冲突**：GC 的 OOM 发生在任意赋值点，只能 panic/忽略；内核要求错误在
   API 边界显式返回（`T!error` / `OS_ENOMEM`），调用点可用 `errdefer` 回滚。
8. **ISR 安全**：ISR 会移交/引用对象（`post`、`queue put`、`resume`），插桩会在
   ISR 里触发释放；内核规定 ISR 只做常数时间工作、禁止分配/释放。
9. **可观测性**：RC 的早释放/泄漏表现为随机崩溃或缓慢泄漏；内核要的是栈哨兵、
   对象 magic、池计数、确定性账本。
10. **确定性测试**：L1–L3 依赖“同一操作序列 → 同一结果”；GC 时机受插桩与堆布局
    影响，破坏可重复性。

替代方案（已采用）：静态对象池 + `u32` 句柄 + 显式生命周期 + `defer`/`errdefer`
回滚 + 关中断临界区。需要引用计数时用**显式** `os_ref_inc/dec`（应用层可选），
不依赖编译器插桩。`@T` 只允许出现在 `app/`，且不得持有内核对象；若将来要进内核，
前提是独立 arena、无环、无 ISR 路径、release 不进临界区——那等价于自己写显式池，
收益为负。

---

## 13. 双 API

```
app.c6 ──> os_task_create(...)            (T!error, 原生)
app.c  ──> LOS_TaskCreate(&id, &attr)     (UINT32, 兼容层)
                 └── 同一个 kernel/task.c6 实现
```

- 兼容层 `compat/los_*.c6` **不加 `module`**，符号保持 `LOS_*` 裸名；
  参数校验、错误码翻译、`void*` ↔ 元素拷贝在薄壳里完成。
- 原生 API 头 `include/c6os/native.inc` 含 `!error` 原型，只被 c6 源包含；
  兼容层与 port 用 `include/c6os/*.h`（纯 C 原型）。
- 一致性：每个内核对象都有「原生用例 + 兼容用例」，跑同一断言集。

---

## 14. port 接口

```c
/* include/c6os/port.h —— 只允许指针/u32/u64/函数指针 */
void  port_init(void);
void  port_start(void);                 /* 开中断并进入调度，不返回 */
void  port_irq_disable(void);
void  port_irq_enable(void);
u32   port_irq_save(void);
void  port_irq_restore(u32 state);
void  port_yield(void);
void *port_sched_coop(void *next_sp);   /* 仅协作 port */
void *port_frame_new(void *stack_top, void (*entry)(void *), void *arg);
void  port_idle(void);
u64   port_cycles(void);
u32   port_cpu_id(void);
u32   port_start_second_core(void (*entry)(void *));
void  port_panic_dump(void);
```

约定：

- `port_frame_new` 负责 FPU 区（`f0–f15`/`FCR`/`FSR`，必填）、入口跳板、16B 栈对齐。
- 抢占式 port 的陷阱入口保存**完整**上下文；协作 port 只保存 callee-saved。
- `port_irq_save/restore` 支持嵌套。
- 时钟中断先清中断源/EOI 再调度；`os_sched_irq` 返回后不得再访问旧栈。

---

## 15. 显示子系统（1.85″ SPI）

- `interface Display { void fill_rect(...); void blit(...); void flush(void); }`
- `port/s3/display/`：
  - `panel_st7789.c`（240×280，最常见 1.85″）；
  - `panel_st77916.c`（360×360 圆屏，备选）；
  - `panel_probe.c`：读 RDDID（0x04）识别型号，失败用 board profile 的默认值。
- 帧缓冲：RGB565，SRAM 起步（240×280 = 131KB），PSRAM 版在 M7 后启用。
- 字体：内置 8×16 ASCII 位图；UI 先做仪表盘（任务表/tick/内存/日志滚动）。

---

## 16. 未决问题（施工中确认）

1. 1.85″ 屏的面板 IC 与 SPI 引脚（需板卡丝印/卖家资料；`make s3-info` 清单）。
2. Xtensa GCC 的 `-mabi=call0` 已实测可用；`-msoft-float` 不可用，FPU 帧必须实现。
3. S3 的 IRAM/DRAM 精确边界与 ROM 保留区（以 IDF `soc.h` 宏为准，写进 `port/s3/memmap.h`）。
4. CCOMPARE0 定时器与中断级别在 S3 rev0.2 上的实测行为。
5. USB-Serial/JTAG 控制台的 FIFO/握手寄存器细节（以 IDF `usb_serial_jtag_ll.h` 为准）。
6. PSRAM 初始化（M7）：是否需要按 8MB AP_3v3 的 octal 模式配置；先不启用。
