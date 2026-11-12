# c6LOS 深度安全性评审

- 评审基线：`rtos-c6@57b9ce3`（risc6 参考机 `7b83ec6`）
- 范围：`kernel/`、`compat/los_*`、`rt/`、`port/{sim,x86_64,s3,rv32,rv32bare}`、S3 启动/链接/中断
- 方法：代码审计 + 威胁建模 + 针对性 PoC（host `c6 -> LLVM IR`）。PoC 为评审临时程序，未提交到仓库。
- 结论：**M0–M8 的功能路径可用，但当前实现不应被当作安全边界**。共确认 6 个 Critical、5 个 High、13 个 Medium、3 个 Low。最严重的是互斥量所有权交接竞态、优先级继承递归环、堆大小整数溢出、内核对象生命周期/UAF，以及真实端口 ISR 标记失效。

> 威胁模型：C6OS 没有 MMU/MPU，所有任务同地址空间、同特权级；"安全"主要指 **任务间故障隔离、对象生命周期安全、ISR/SMP 下的内存安全**，以及串口 shell/LOS API 输入健壮性。若所有任务完全可信，部分 High 可降为健壮性缺陷；但串口 shell、可配置对象参数、跨核共享对象仍属于攻击面。

---

## 1. 结论摘要

| 级别 | 数量 | 代表问题 |
|---|---:|---|
| Critical | 6 | mutex 双持有、PI 递归爆栈、heap 溢出、对象 UAF、ISR 门禁失效、SMP 调度竞态 |
| High | 5 | 任务栈大小被忽略、超时队列满永久阻塞、删除不释放资源、LOS 队列信息泄漏、queue/membox 溢出 |
| Medium | 13 | S3 栈/BSS 无保护、timer 注册表、LOS delete、内存池别名、kprintf/log、gfx/panel、S3 IRQ、NULL name、调度器前置检查 |
| Low | 3 | shell reboot 无鉴权、诊断寄存器写、统计/诊断信息 |

### 已确认 PoC

1. **mutex 双持有**：`VULN: two owners observed`。两个任务同时认为自己是同一 mutex 的 owner。
2. **PI 链递归**：ABBA 两个 mutex + 两个任务，程序在 `A wait m2` 后挂起，`timeout 3` 返回 124。
3. **heap 整数溢出**：`os_heap_alloc(&h, 0xFFFFFFFFu)` 返回非空指针（`VULN: huge alloc returned`），调用者按 `n` 字节写入即越界。

PoC 复现命令见 §4。

---

## 2. Critical

### S-01 mutex 所有权交接竞态（双持有）

- 位置：`kernel/mutex.c6:70-86`，等待者恢复路径 `kernel/mutex.c6:61-66`
- 描述：`os_mutex_post()` 先把 `m->owner = 0`，再 `os_task_wake_first(m, 2u, 0)`；被唤醒的等待者要等调度到它时才在 `os_mutex_pend()` 返回路径里执行 `m->owner = self`。在这段窗口内，任何更高优先级任务调用 `os_mutex_pend()` 都会看到 `owner == 0` 并成功获取；随后原等待者恢复又覆盖 `owner`。
- 影响：**互斥失效**，两个任务同时进入临界区；共享队列/堆/LOS 对象可被并发修改。SMP 下窗口更大。
- 证据：临时 PoC 输出 `VULN: two owners observed`。
- 建议：
  - `os_task_wake_first()` 返回被唤醒任务 id；`os_mutex_post()` 在唤醒前把所有权直接交给该任务（`owner = waiter_id; count = 1`），而不是置 0。
  - 增加 `handoff`/`owner_pending` 状态；等待者恢复时校验 `owner == self`，否则继续等待。
  - 不要在持锁任务未完成交接时允许其他任务获取。

### S-02 优先级继承链无界递归

- 位置：`kernel/mutex.c6:6-27`
- 描述：`boost_chain()` 沿 `wait_obj` 递归；两个任务各持一个 mutex 并互相等待（ABBA）时形成环，递归永不终止。
- 影响：栈溢出/看门狗复位/系统挂死；可由两个普通任务和两个 mutex 触发，属于低门槛 DoS。
- 证据：PoC 在 `A wait m2` 后挂起，`timeout 3` 返回 124。
- 建议：递归改为迭代并带 visited 集合/深度上限（`C6OS_MAX_TASKS`）；检测到环时停止 boost 并记录告警；或引入等待图环检测拒绝会造成死锁的 pend（可选）。

### S-03 heap 大小整数溢出与块头未校验

- 位置：`kernel/heap.c6:14-18`、`:58`、`:95-103`
- 描述：
  - `need = ALIGN8(n)` 在 `n > 0xFFFFFFF8` 时回绕为 0；`best->size >= need` 成立，返回一个很小甚至 0 字节的块。调用者按原 `n` 写入即越界。
  - `os_heap_free()` 只按 `p - 16` 回推块头，不校验块头 magic、对齐、块边界、size 是否落在堆内；伪造/野指针可造成 `h->used -= b->size` 下溢，或合并时用伪造 size 跳出堆范围。
  - 分配/合并扫描没有对 `HDR_SIZE + b->size` 做溢出检查，损坏块头可导致死循环/越界读。
- 影响：堆/全局内存破坏，任意写原语（在可控块头时）。
- 证据：PoC `os_heap_alloc(&h, 0xFFFFFFFFu)` 返回非空。
- 建议：`if (n > 0xFFFFFFF8u) return 0;` 并校验 `need >= n`；块头加入 magic/自身地址/对齐；free 校验对齐、边界、free 位；所有 `size` 加法用 `if (a > limit - b)`；`used` 减法前校验；开启 double-free 检测。

### S-04 内核对象生命周期/UAF（sem/mutex/event/queue/timer）

- 位置：`kernel/task.c6:219-247`（等待节点只存裸 `obj` 指针）、`kernel/sem.c6:16-25`、`kernel/timer.c6:9-31,50-67`、`compat/los_*.c6` 的 delete
- 描述：阻塞任务把调用者对象的裸指针存进 TCB；对象没有引用计数/析构协议。调用者 `delete`/`free` 对象后：
  - 超时或 `post/set/put` 仍可能通过 `wait_obj` 访问已释放内存；
  - 同一地址被新对象复用后，旧等待者会被"误唤醒"（ABA）；
  - `os_timer_stop()` 不移除 `g_timers[]` 指针，调用者释放 timer 后 `os_timer_tick()` 仍会调用其回调（UAF/控制流劫持）。
- 影响：UAF、类型混淆、回调劫持、任意执行（若攻击者可控制复用对象内容）。
- 建议：对象句柄化（id + generation）而不是裸指针；对象 delete 必须先唤醒所有等待者并返回错误，再失效；timer 注册/注销成对，stop 后从表移除；对 caller-owned 对象要求显式 `os_*_destroy`，禁止直接 free。

### S-05 真实端口 ISR 门禁失效

- 位置：`port/s3/console.c:21-24`、`port/rv32bare/console.c:82-85`、`port/rv32/console.c:87-90` 均 `return 0`；`kernel/task.c6:376-392` 在 tick 中调用 `os_timer_tick()`；`os_task_block_arg`/`os_task_delay` 仅依赖 `port_in_isr()`
- 描述：S3 的 `s3_timer_irq()` 在中断上下文调用 `os_sched_tick()`，进而执行用户 timer 回调。端口 `port_in_isr()` 恒为 0，因此回调里调用 `os_sem_pend`、`os_task_delay`、`os_mutex_pend`（阻塞路径）不会被拦截，会从 ISR 内切换上下文/调用 `port_task_yield`。S3 的 trap 返回依赖当前栈上的陷阱帧，切换栈会破坏返回路径。
- 影响：ISR 内栈/陷阱帧破坏、随机 panic、看门狗复位；属于可被普通回调触发的内核崩溃。
- 建议：每个 hart 在 trap/IRQ 入口递增 `g_isr_nest[core]`，`port_in_isr()` 读该计数；退出递减。timer 回调额外加 `in_timer_cb` 限制，只允许非阻塞 post/set/put；在 S3 `_vec_level1`/rv32bare trap 里显式设置。修复后把 `t_isr` 扩展到真实端口（当前只在 sim 用 `os_test_set_isr` 模拟）。

### S-06 SMP 调度器竞态（共享 ready 位图/任务迁移）

- 位置：`kernel/task.c6:15`（全局 `g_ready_bit`）、`:49-70`（`ready_rebuild` 写全局）、`:395-410`（`os_sched_preempt_ctx`）、`os_core_ready`/`balance_pull`
- 描述：
  - 两个核都调用 `ready_rebuild(core)` 写同一个 `g_ready_bit`；risc6 每 2000 条指令切片轮转，一个核可在构建位图中途被切走，另一个核看到半成品位图，`sched_pick` 可能返回空/错误优先级。
  - `balance_pull` 读任务 `state/core` 后写 `core`，没有 claim/锁；目标核的 `sched_pick` 可能在迁移前已选中同一任务，导致**同一 ctx 在两个核上同时运行**，栈帧互相覆盖。
  - 任务状态、timeout 队列、对象等待链全部是全局可变数据，SMP 下无临界区。
- 影响：同任务双核运行导致栈/上下文破坏；调度饥饿；timeout 数组越界/丢失。比单核 bug 更难复现但后果严重。
- 建议：`g_ready_bit` 改为 `sched_pick` 局部变量（至少消除位图竞态）；每核独立 ready 队列 + 每核锁/关中断；任务迁移使用 `state` CAS/claim 标志，禁止迁移 RUNNING；timeout/对象操作加全局或分片锁；SMP 回归加入随机 yield/迁核压力。

---

## 3. High

### S-07 端口忽略任务栈大小，栈溢出检测太晚

- 位置：`port/rv32/ctx.c:24-32`（`(void)stack_size`，固定 8192）、`port/s3/ctx.c:36-45`（固定 4096）、`kernel/task.c6:94-119`、`:432-445`（切换返回后才 `port_task_stack_ok`）
- 描述：API 接受 `stack_size`，LOS `LOS_TaskCreate` 也把 `>4096` 的值传下来，但两个端口都忽略它。请求 64KB 的任务实际只有 4KB；溢出先破坏相邻任务栈/BSS，返回后才检查栈底 4 字节哨兵。
- 影响：栈溢出导致内存破坏；哨兵只覆盖栈底 4 字节，越界写可能不触发检测。
- 建议：按请求大小从受控池分配（带上限/失败返回），或在链接期预留每任务栈并拒绝超限；哨兵扩为 16–32 字节并定期/切换前检查；S3 若有 MPU 可加 guard region。

### S-08 超时队列满时静默永久阻塞

- 位置：`kernel/task.c6:235`、`:480`；`kernel/timeout.c6:45-61`
- 描述：`os_task_block_arg()`/`os_task_delay()` 忽略 `os_timeout_add()` 的 `-1`。`OS_TIMEOUT_MAX=128` 满时，任务已置 BLOCKED 却没有超时节点，若对象永不 post 就永久阻塞。
- 影响：低门槛资源耗尽 DoS；LOS `LOS_WAIT_FOREVER` 与有限超时行为不一致。
- 建议：先申请超时槽再改状态；失败返回 `-3`/`LOS_ERRNO`，不阻塞；或扩大队列并加断言。

### S-09 任务删除/退出不释放资源、不唤醒 join

- 位置：`kernel/task.c6:198-217`（delete 只 cancel timeout/free ctx）、`os_task_exit` 只调用 `os_task_wake_join`
- 描述：
  - `os_task_delete(id)` 不调用 `os_task_wake_join()`，`os_task_join(id, timeout)` 的等待者会一直等到超时/永久。
  - 删除持有 mutex/sem/queue 的任务时不释放所有权/不唤醒等待者，后续任务死锁。
  - 删除 READY/BLOCKED 任务后，若对象槽位复用，旧 `wait_obj` 可能造成 ABA。
- 影响：死锁、资源泄漏、任务表污染；服务型任务可被单个异常任务拖死。
- 建议：统一 `task_cleanup(t)`：唤醒 join、释放/转移持有的 mutex（或 panic 提示未释放）、从所有等待链摘除；delete 与 exit 共用；禁止删除 RUNNING 以外的状态时无锁直接 free。

### S-10 LOS 队列消息长度缺失导致信息泄漏

- 位置：`compat/los_queue.c6:93-143`
- 描述：`LOS_QueueWrite` 只把 `len` 字节写入 64 字节 slot，不记录长度；`LOS_QueueRead` 无条件从 slot 复制 `maxLen`（上限 64）字节。短消息读取会把 slot 中残留的旧消息/未初始化数据返回给调用者。`len > 64` 被静默截断而不是报错。
- 影响：跨任务/跨队列信息泄漏；数据正确性问题；若 slot 曾存敏感数据，可被低权限任务读取。
- 建议：每个 slot 增加长度头（或并行长度数组），读返回实际长度；写入前清零/只复制实际长度；超长返回 `LOS_ERRNO_QUEUE_INVALID`；delete 清理所有 slot。

### S-11 queue/membox 整数溢出与元数据未校验

- 位置：`kernel/queue.c6:8-20`、`:26-46`；`kernel/membox.c6:5-30`、`:56-64`
- 描述：
  - queue 的 `idx * item_size`、`cap * item_size` 无溢出检查；`item_size`/`cap` 由调用者给出，`slot_write` 按 `item_size` 循环可读写越界。没有 storage 容量参数，无法验证。
  - membox 的 `blk_size + 7` 可回绕为 0（随后 free 中 `% blk_size` 除零）；`blk_size * blk_count` 可回绕，free 的边界检查失效；init 按 `blk_count` 写 free-list，超出 storage。
  - membox 无 double-free 检测：同一个块可被释放两次，free-list 成环，alloc 返回同一个块给两个调用者。
- 影响：堆外读写、除零、double allocation（两个任务拿到同一块）。
- 建议：所有乘法用 `if (a != 0 && b > UINT32_MAX / a) return -1`；`blk_size` 拒绝 0/过大；membox 每块加 magic/alloc 位图；free 校验块对齐/是否已分配；queue API 增加 storage 字节数参数并校验。

---

## 4. Medium

### S-12 S3 主栈/BSS/core1 栈无保护区

- 位置：`port/s3/start.S:7`（栈顶 `0x3FCEF000`）、`port/s3/link.ld`、`port/s3/memmap.h`、`port/s3/core1.c:14-32`
- 描述：链接脚本没有为栈预留区间；实测 `_bss_end=0x3fcecdf0`，与栈顶只剩约 8.7KB。BSS 增长或深调用会静默覆盖；core1 只有 4KB 静态栈，无哨兵/MPU。
- 建议：链接脚本显式 `_stack_bottom = _bss_end + GUARD`，`ASSERT(_stack_bottom <= _stack_top - STACK_SIZE)`；core1 栈加 canary/大小检查；上板跑深调用测试。

### S-13 timer 注册表 stale/重复

- 位置：`kernel/timer.c6:9-31`、`:42-48`
- 描述：`os_timer_stop()` 只清 `active`，不移除 `g_timers[]`；对同一对象再次 `os_timer_init()` 会重复注册；槽位只有 16 个，反复 stop/init 会耗尽。调用者释放 timer 后表项悬空（与 S-04 合并）。
- 建议：stop/delete 从表移除；init 检测重复对象并返回已注册；timer 对象必须由内核句柄管理。

### S-14 LOS delete 不唤醒等待者

- 位置：`compat/los_sem.c6:39-49`、`los_mux.c6:36-45`、`los_event.c6:36-45`、`los_queue.c6:78-91`
- 描述：delete 只清 magic/used，不唤醒 BLOCKED 等待者；`LOS_WAIT_FOREVER` 等待者永久挂起。槽位复用后旧等待者可能被新对象误唤醒（ABA）。
- 建议：delete 遍历等待链，以 `LOS_ERRNO_*_DELETED` 唤醒；等待者记录 generation。

### S-15 LOS 内存池别名

- 位置：`compat/los_mem.c6:4-30`
- 描述：`LOS_MemInit(pool, size)` 只初始化一个全局 `g_heap`，忽略 `pool`；所有 pool 名称实际指向同一堆。任意任务可用别人的 pool 指针分配/释放，破坏隔离。
- 建议：按 `pool` 维护句柄表（最多 N 个），或 API 明确只支持单池并在 `LOS_MemAlloc` 校验 `pool == g_pool`。

### S-16 LOS 软件定时器槽泄漏

- 位置：`compat/los_swtmr.c6:61-68`
- 描述：`LOS_SwtmrDelete` 只清 `g_used/g_cb`，不调用 `os_timer` 注销；重复 create/delete 会在 `g_timers[]` 留下 16 个 stale 指针后 `os_timer_init` 失败。
- 建议：增加 `os_timer_deinit()` 并从表移除；delete 后允许复用同一槽。

### S-17 kprintf/log/console 并发与格式健壮性

- 位置：`rt/kprintf.c:77-115`、`kernel/log.c6:10-26`、`port/s3/console.c:52-103`
- 描述：`%d` 对 `INT_MIN` 执行 `v = -v` 是 UB；`width` 可被格式串放大到巨量输出（DoS）；`%s` 无界；若格式串来自攻击者，可做信息泄漏/日志注入（无 `%n`，不能直接写）。klog 环形缓冲和 USB FIFO/`left` 无锁，SMP/ISR 并发会交错或破坏。S3 `port_getc` 的 `static left` 跨核共享。
- 建议：`v = -(u64)v` 或先转无符号绝对值；width 上限（如 256）；对不可信字符串用精度限制；klog/console 加每核锁或关中断；S3 的 `left` 改为每核状态。

### S-18 gfx/panel 边界与整数溢出

- 位置：`rt/gfx.c:3-29`、`:65-81`、`port/s3/panel.c:167-199`、`:283-339`、`:456-485`
- 描述：`OsGfx` 不保存 framebuffer 容量，`y * w` 可 u32 溢出；`blit` 的 `yy * sw` 可溢出导致源越界读；`s3_st7789_probe`/`s3_panel_probe` 不解引用前检查 `out` 非空；`s3_lcd_blit` 的 `w*h` 可溢出，且不校验 fb 大小；`s3_gpio_loop_sel(p)` 对任意 `p` 写 `0x60009000 + p*4`（任意外设寄存器写）。
- 建议：Gfx 增加 `fb_pixels` 容量并做 checked multiply；blit 校验 src 尺寸；panel 函数检查指针/尺寸；`s3_gpio_loop_sel` 限制在合法 GPIO 范围，或从公共头移除。

### S-19 S3 IRQ 健壮性

- 位置：`port/s3/irq.c:98-104`、`:117-170`、`port/s3/irq.S`
- 描述：`s3_irq_init()` 直接写 `INTENABLE=1<<6`，覆盖已有使能位；没有嵌套计数/临界区；`s3_timer_irq` 先 rearm 再执行 `os_sched_tick`/回调，长回调会丢 tick；`s3_trap` 用传入 `frame_sp` 读 `f[15]/f[16]`，双重故障/坏 SP 时可能二次异常；默认向量只写诊断标记后死循环。
- 建议：读-改-写 `INTENABLE`；ISR 嵌套计数；rearm 放在处理尾部或用 CCOMPARE 补偿；trap 校验 frame 范围，双重故障走独立栈/最小 panic。

### S-20 任务创建 NULL name / 参数校验

- 位置：`kernel/task.c6:84-130`、`compat/los_task.c6:4-21`
- 描述：`os_task_create()` 直接 `name[i]`，未检查 `name == 0`；`LOS_TaskCreate` 未检查 `initParam->pcName`。`usTaskPrio` 为 UINT16，>31 时返回 -1 尚可；`uwStackSize` 被端口忽略（S-07）。
- 建议：`name == 0` 用默认名；限制 `C6OS_NAME_MAX`；对 LOS 参数逐项校验并返回精确错误码；创建前检查调度器状态/ISR。

### S-21 调度器前置状态与 ISR 快路径

- 位置：`kernel/mutex.c6:46-54`（`self=0` 时 `owner==self` 误判递归）、`kernel/task.c6:219-221`（仅阻塞路径检查 ISR）、`kernel/task.c6:84`（create 无 ISR/启动状态检查）
- 描述：`os_task_self()` 在调度器启动前/ISR 中返回 0；对空闲 mutex 调用 pend 会命中 `owner==self` 分支，`count++` 但不设 owner，后续真实任务可再次获取。`os_sem_post` 等 ISR 快路径可唤醒任务但不会触发调度（依赖退出 IRQ 的 preempt），在非抢占端口可能延迟。
- 建议：`if (self == 0) return -1;`；增加 `kernel_started`/`in_isr` 检查；为 ISR 提供显式 `*_from_isr` API 或统一延迟调度标志。

### S-22 `os_sched_preempt_ctx` 同任务时状态错误

- 位置：`kernel/task.c6:395-413`
- 描述：抢占选择到当前任务时只更新 `g_cur`，没有把状态从 READY 改回 RUNNING（`make_ready(g_cur)` 已把它置 READY）。之后 `os_task_info` 显示 READY，某些 `if (state == RUNNING)` 逻辑（如 delete/boost）行为不一致。
- 建议：`if (t == g_cur) t->state = RUNNING;` 或仅在切换时改状态。

### S-23 `os_event_wait` mask=0/mode 非法

- 位置：`kernel/event.c6:46-60`、`compat/los_event.c6:71-87`
- 描述：`mask=0` 时 `event_hit` 永远 false，任务永久阻塞；`mode` 非 `OS_EVENT_ALL` 一律按 ANY；`LOS_EventRead` 把返回 bits=0 当作超时错误，无法区分"事件位为 0"。
- 建议：mask==0 返回参数错误；mode 白名单校验；LOS 返回长度/状态分离。

### S-24 `os_hwi_*` 生命周期/优先级

- 位置：`kernel/hwi.c6:13-54`、`compat/los_hwi.c6:4-31`
- 描述：`os_hwi_delete` 在 dispatch 期间可能清空 fn（单核下调用已取址，风险低；SMP 下更高）；`LOS_HwiCreate` 忽略 `prio/mode`，所有 IRQ 同优先级；没有 IRQ 共享/引用计数。
- 建议：dispatch 取本地副本并加 RCU/临界区；实现或明确拒绝 prio/mode；delete 等待在途 handler。

---

## 5. Low

- **S-25 shell 无鉴权**：`rt/shell.c:141-147` 的 `reboot` 可被任何串口输入触发；物理串口通常可信，但生产镜像应加命令白名单/鉴权或编译开关。
- **S-26 诊断接口可写任意寄存器**：`port/s3/panel.c:167-199` 的 `s3_gpio_loop_sel` 对任意 `p` 计算并写 IO_MUX；只应在测试固件导出。
- **S-27 统计/诊断不可信**：`os_heap_used/peak` 直接信任可能已损坏的 `used`；`os_task_info` 不做 magic 校验；仅影响诊断，不构成独立利用链。

---

## 6. 正向发现

- `rt/cobs.c` 的 encode/decode 都有容量检查；shell 行复制上限 127，`os_shell_exec_frame` 的 `line[dn]` 有界。
- 大多数对象 API 有 `magic` 和 NULL 检查；任务数/优先级有上限；`sched_pick` 对空位图有保护。
- task 栈有 `0xC6C0DEAD` 哨兵（检测晚，但能发现部分溢出）。
- `kprintf` 不支持 `%n`，避免最直接的格式串写原语。
- 链接布局 IRAM 可执行、DRAM 可写，具备基本 W^X 雏形（但无 MPU）。
- 现有回归：host 39 PASS、risc6 参考机 6 PASS（含 SMP 均衡/跨核 sem），说明功能路径稳定；安全回归尚未建立。

---

## 7. 修复优先级

**P0（阻断发布）**
1. S-01 mutex 所有权交接（改为唤醒即转移 owner）。
2. S-02 PI 链环检测/深度上限。
3. S-03 heap 溢出检查 + 块头校验。
4. S-04 对象生命周期/析构协议（至少 delete 唤醒等待者 + timer 注销）。
5. S-05 真实端口 `port_in_isr`（每核嵌套计数）。
6. S-06 SMP ready 位图局部化 + 迁移 claim/锁。

**P1（下一个迭代）**
7. S-07 栈大小落实/guard；S-08 超时槽申请失败处理；S-09 task cleanup。
8. S-10 LOS 队列长度；S-11 queue/membox 溢出与 double-free。
9. S-12 S3 栈/BSS 链接期断言；S-19 IRQ 嵌套/INTENABLE 保存。

**P2（加固/可观测）**
10. S-13–S-18 timer/LOS delete/内存池别名/kprintf/GFX/panel。
11. S-20–S-24 参数/状态校验、HWI 生命周期。
12. 建立 `tests/security/`：把 §4 三个 PoC 改为 fail-before/pass-after 回归；host 开启 ASan/UBSan；对 cobs/shell/queue 做 fuzz；SMP 随机迁核压力。

---

## 8. PoC 复现（评审临时程序）

以下程序放在 `/tmp`，用 host `c6` 编译，与 `build/host/*.ll` 链接；不属于仓库测试。

### 8.1 mutex 双持有

关键任务：

```c
/* A prio10: 持 m；post gate 让 B 阻塞在 m；post go 让 C 就绪；
 *           mutex_post 唤醒 B；yield 让 C 先抢到 m；B 恢复后覆盖 owner。 */
A: os_mutex_pend(&m,0); os_sem_post(&gate); os_task_yield();
   os_sem_post(&go); os_mutex_post(&m); os_task_yield();

B: os_sem_pend(&gate,0); os_mutex_pend(&m,0);   /* 阻塞，被 post 唤醒 */
   g_b_owner = os_mutex_owner(&m);              /* 恢复路径把 owner 改成 B */

C: os_sem_pend(&go,0); os_mutex_pend(&m,0);      /* 在 B 恢复前抢到 */
   g_c_owner = os_mutex_owner(&m);
```

输出：

```
C start
B gate
A pend
B pend
A post go/m
C go
C mutex
B got
A exit
checker
VULN: two owners observed
```

### 8.2 PI 链递归

```c
A: os_mutex_pend(&m1,0); os_task_yield(); os_mutex_pend(&m2,0);
B: os_mutex_pend(&m2,0); os_task_yield(); os_mutex_pend(&m1,0);
```

输出 `A wait m2` 后不再前进；`timeout 3` 返回 124。

### 8.3 heap 大小回绕

```c
OsHeap h = {0};
os_heap_init(&h, arena, sizeof arena);
void *p = os_heap_alloc(&h, 0xFFFFFFFFu);   /* 返回非空 */
```

输出 `VULN: huge alloc returned`。

---

## 9. 评审限制

- 未在真实 ESP32-S3 上执行（评审时 `/dev/ttyACM0` 不在线），S3 相关结论来自代码/链接布局/编译产物（`_bss_end`、栈顶符号）。
- 未做二进制固件逆向、故障注入、功耗/电磁侧信道；未覆盖 c6 编译器自身代码生成缺陷。
- risc6 是确定性指令级模拟器，不等价于真实双核缓存一致性/中断时序；SMP 竞态在真板上可能更易触发。
- 未对 `compat/los_*` 做完整 LiteOS-M 语义一致性证明；仅审计内存/生命周期/错误路径。

---

## 10. 建议的验收门槛

- P0 全部修复并有对应 fail-before/pass-after 测试；host `check-all` 与 risc6 `check-sim` 全绿。
- `make s3` + `t_s3_smp` / `t_s3_smp_sem` 在真板连续运行 ≥30 分钟，无 panic/看门狗复位；core1 `prid=1`。
- 在 host 构建开启 `-fsanitize=address,undefined` 跑全部测试；cobs/shell/queue 做 ≥10^6 次随机输入 fuzz，无越界/崩溃。
- SMP 压力：两核随机 create/delete/yield/迁移/共享 sem/queue 10^6 次，断言无同一 ctx 双核运行、无任务表泄漏。
