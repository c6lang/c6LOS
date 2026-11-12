# API：原生 `os_*` 与兼容 `LOS_*`

> 相关：[architecture.md](architecture.md) · [api 实现状态见 todo.md](todo.md)

---

## 1. 类型与返回值

```c
/* include/c6os/types.h —— 纯 C，port/compat/rt 共用 */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed   int       i32;
typedef signed   long long i64;

typedef enum OsError {
    OS_OK = 0,
    OS_EINVAL = -1, OS_ENOMEM = -2, OS_EBUSY = -3, OS_ETIMEOUT = -4,
    OS_ENOENT = -5, OS_EFULL = -6,  OS_EEMPTY = -7, OS_ESTATE = -8,
    OS_ENOSYS = -9, OS_EPERM = -10, OS_EOVERFLOW = -11,
} OsError;
```

- 原生 API 返回 `int`（0 成功，负 `OsError`）；标注 `!error` 的函数失败时 `err(id,msg)`。
- 兼容 API 返回 `UINT32`：0 成功，非 0 为 LiteOS 错误码（见 §4）。
- 句柄：`u32 id`（索引入静态对象池），不暴露指针，避免应用持有裸指针。

---

## 2. 原生 API（c6）

```c
/* include/c6os/native.inc —— 只被 .c6 包含；含 !error 原型 */
```

### 2.1 任务

```c
int!error os_task_create(const char *name, void (*entry)(void *), void *arg,
                         u8 prio, u32 stack_bytes, u32 *out_id);
int!error os_task_delete(u32 id);          /* 允许删自己 */
int!error os_task_suspend(u32 id);
int!error os_task_resume(u32 id);
int!error os_task_delay(u32 ticks);
void      os_task_yield(void);
u32       os_task_self(void);
u32       os_task_count(void);
int!error os_task_info(u32 id, OsTaskInfo *out);   /* 名称/优先级/状态/栈水位 */
```

- `prio`：0..`C6OS_PRIOS-1`，0 最高；`C6OS_PRIO_IDLE` 保留给空闲任务。
- `stack_bytes`：0 表示默认 8KB；向上取整到 16B。
- 失败：`OS_EINVAL`（名字空/prio 越界）、`OS_ENOMEM`（TCB/栈池耗尽）、`OS_ESTATE`（未初始化）。

### 2.2 信号量 / 互斥量 / 事件

```c
int!error os_sem_create(const char *name, u32 init, u32 max, u32 *out_id);
int!error os_sem_pend(u32 id, u32 timeout_ticks);   /* C6OS_WAIT_FOREVER */
int!error os_sem_post(u32 id);
int!error os_sem_delete(u32 id);

int!error os_mux_create(const char *name, u32 *out_id);
int!error os_mux_pend(u32 id, u32 timeout_ticks);
int!error os_mux_post(u32 id);
int!error os_mux_delete(u32 id);

int!error os_event_create(const char *name, u32 *out_id);
int!error os_event_get(u32 id, u32 mask, u32 mode, u32 *out_flags, u32 timeout_ticks);
int!error os_event_set(u32 id, u32 flags);
int!error os_event_clear(u32 id, u32 flags);
```

`mode`：`OS_EVENT_AND` / `OS_EVENT_OR` / `OS_EVENT_CLEAR`（读后清）。

### 2.3 队列 / 内存池 / 堆

```c
/* 字节队列（内核核心）；元素拷贝长度在创建时固定 */
int!error os_queue_create(const char *name, u32 elem_size, u32 depth, u32 *out_id);
int!error os_queue_put(u32 id, const void *elem, u32 timeout_ticks);
int!error os_queue_get(u32 id, void *out, u32 timeout_ticks);
int!error os_queue_delete(u32 id);

/* 泛型包装（app 层）：Queue@T */
Queue@T q = os_queue_new@T("cmd", 8);       /* 创建 + 类型绑定 */
int!error q.put(T v, u32 timeout);
int!error q.get(T *out, u32 timeout);

int!error os_membox_create(const char *name, void *buf, u32 buf_bytes,
                           u32 block_bytes, u32 *out_id);
void *    os_membox_alloc(u32 id);
int!error os_membox_free(u32 id, void *block);
int!error os_membox_info(u32 id, OsMemboxInfo *out);

int!error os_mem_init(void *buf, u32 bytes);
void *    os_mem_alloc(u32 bytes);          /* 8 字节对齐；失败返回 0 */
int!error os_mem_free(void *p);
void *    os_mem_realloc(void *p, u32 bytes);
int!error os_mem_info(OsMemInfo *out);
```

### 2.4 时间 / 定时器 / 中断

```c
u64 os_tick_get(void);
u32 os_ms_to_ticks(u32 ms);
u32 os_ticks_to_ms(u32 ticks);
void os_tick_irq(void);                     /* 仅供 port 调用 */

int!error os_swtmr_create(const char *name, u32 period_ticks, void (*cb)(void *),
                          void *arg, u8 periodic, u32 *out_id);
int!error os_swtmr_start(u32 id);
int!error os_swtmr_stop(u32 id);
int!error os_swtmr_delete(u32 id);

int!error os_hwi_create(u32 vector, void (*isr)(void *), void *arg, u8 prio, u32 *out_id);
int!error os_hwi_delete(u32 id);
int!error os_hwi_trigger(u32 id);           /* 软件触发，测试用 */
```

### 2.5 系统

```c
void os_init(void);
void os_start(void);                        /* 不返回 */
void os_panic(const char *file, int line, const char *fmt, ...);
u32  os_version(void);                      /* 0xMMmmpp */
```

---

## 3. 兼容 API（LOS_*）

签名与 LiteOS-M 对齐（`include/los/*.h`），返回值 `UINT32`：

| 头文件 | 接口 |
|---|---|
| `los_task.h` | `LOS_TaskCreate/LOS_TaskDelete/LOS_TaskSuspend/LOS_TaskResume/LOS_TaskDelay/LOS_TaskYield/LOS_TaskPriGet/LOS_TaskPriSet/LOS_TaskInfoGet` |
| `los_sem.h` | `LOS_SemCreate/LOS_SemDelete/LOS_SemPend/LOS_SemPost` |
| `los_mux.h` | `LOS_MuxCreate/LOS_MuxDelete/LOS_MuxPend/LOS_MuxPost` |
| `los_event.h` | `LOS_EventCreate/LOS_EventRead/LOS_EventWrite/LOS_EventClear/LOS_EventDestroy` |
| `los_queue.h` | `LOS_QueueCreate/LOS_QueueDelete/LOS_QueueRead/LOS_QueueWrite/LOS_QueueInfoGet` |
| `los_membox.h` | `LOS_MemboxInit/LOS_MemboxAlloc/LOS_MemboxFree/LOS_MemboxClr` |
| `los_swtmr.h` | `LOS_SwtmrCreate/LOS_SwtmrStart/LOS_SwtmrStop/LOS_SwtmrDelete` |
| `los_memory.h` | `LOS_MemInit/LOS_MemAlloc/LOS_MemFree/LOS_MemRealloc/LOS_MemInfoGet` |
| `los_tick.h` | `LOS_GetSystemCycleFreq/LOS_TickDelay/LOS_Tick2Ms/LOS_Ms2Tick` |
| `los_hwi.h` | `LOS_HwiCreate/LOS_HwiDelete/LOS_HwiTrigger` |

语义差异表（兼容层必须遵守）：

| LiteOS-M | c6LOS |
|---|---|
| `LOS_QueueCreate` 长度单位是**消息个数**，`uwMaxMsg` 为元素大小 | 同义；元素区由内核从堆/静态池分配 |
| `LOS_SemPend` 超时单位是 tick | 同义；`LOS_WAIT_FOREVER` = `0xFFFFFFFF` |
| `LOS_MuxPend` 支持递归 | 同义；`LOS_MuxPost` 必须由持有者调用 |
| `LOS_EventRead` 的 `uwMode` 含 `LOS_WAITMODE_AND/OR/CLEAR` | 位定义保持一致 |
| `LOS_MemboxInit` 的 `uwBlkNum` 是块数 | 同义；`uwBlkSize` 自动 16B 对齐 |
| `LOS_MemAlloc` 的堆由 `LOS_MemInit` 指定 | 同义；内核堆默认 `C6OS_HEAP_SIZE` |

未实现（返回 `LOS_ERRNO_OS_NOSYS`）：MPU/MMU、POSIX、CMSIS-RTOS、动态加载、文件系统。

---

## 4. 错误码映射

```c
/* include/los/los_err.h */
#define LOS_OK                          0
#define LOS_ERRNO_OS_NOSYS              (0x02000001u)
#define LOS_ERRNO_OS_INVALID_PARAM      (0x02000002u)
#define LOS_ERRNO_OS_NOMEM              (0x02000003u)
#define LOS_ERRNO_OS_TIMEOUT            (0x02000004u)
#define LOS_ERRNO_OS_BUSY               (0x02000005u)
#define LOS_ERRNO_OS_STATE              (0x02000006u)
#define LOS_ERRNO_OS_FULL               (0x02000007u)
#define LOS_ERRNO_OS_EMPTY              (0x02000008u)
#define LOS_ERRNO_OS_PERM               (0x02000009u)
```

| `OsError` | `LOS_*` |
|---|---|
| `OS_OK` | `LOS_OK` |
| `OS_EINVAL` | `LOS_ERRNO_OS_INVALID_PARAM` |
| `OS_ENOMEM` | `LOS_ERRNO_OS_NOMEM` |
| `OS_ETIMEOUT` | `LOS_ERRNO_OS_TIMEOUT` |
| `OS_EBUSY` | `LOS_ERRNO_OS_BUSY` |
| `OS_ESTATE` | `LOS_ERRNO_OS_STATE` |
| `OS_EFULL` | `LOS_ERRNO_OS_FULL` |
| `OS_EEMPTY` | `LOS_ERRNO_OS_EMPTY` |
| `OS_EPERM` | `LOS_ERRNO_OS_PERM` |
| 其余 | `LOS_ERRNO_OS_NOSYS` |

---

## 5. 配置（`include/c6os/config.h`）

```c
#define C6OS_MAX_TASKS        16
#define C6OS_PRIOS            32
#define C6OS_TICK_HZ          100
#define C6OS_TIMESLICE        10
#define C6OS_STACK_DEFAULT    8192
#define C6OS_STACK_MAIN       16384
#define C6OS_STACK_IDLE       2048
#define C6OS_KEYS             8
#define C6OS_MAX_SEM          16
#define C6OS_MAX_MUX          8
#define C6OS_MAX_EVENT        8
#define C6OS_MAX_QUEUE        8
#define C6OS_MAX_SWTM R       16
#define C6OS_MAX_MEMBOX       4
#define C6OS_HEAP_SIZE        65536
#define C6OS_LOG_SIZE         4096
#define C6OS_WAIT_FOREVER     0xFFFFFFFFu
#define C6OS_NAME_MAX         16
```

编译期断言（`_Static_assert` 或 c6 等价物）锁死：`C6OS_MAX_TASKS<=32`（位图）、
`C6OS_PRIOS==32`、结构体大小预算、栈/堆合计。
