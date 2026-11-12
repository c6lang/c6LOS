#ifndef C6OS_TASK_H
#define C6OS_TASK_H

#include "c6os/config.h"
#include "c6os/types.h"

typedef void (*OsTaskEntry)(void *);

/* 任务状态（M1 最小集；BLOCKED/SUSP 在 M3+ 接入等待节点）。 */
#define OS_TASK_FREE 0u
#define OS_TASK_READY 1u
#define OS_TASK_RUNNING 2u
#define OS_TASK_DEAD 3u
#define OS_TASK_BLOCKED 4u
#define OS_TASK_SUSP 5u
#define OS_TASK_MAGIC 0xC6A50001u

typedef struct OsTask {
    void *ctx;                 /* port 上下文（不透明） */
    OsTaskEntry entry;
    void *arg;
    u32 id;
    u32 magic;                 /* 仅 c6 侧访问；C 侧走 os_task_info */
    u32 stack_size;
    u32 slice;                 /* 剩余时间片（tick） */
    u32 delay_left;            /* 延时剩余 tick */
    u8 prio;                   /* 0..31，越小越高 */
    u8 base_prio;              /* 未继承时的优先级 */
    u8 state;
    u8 core;                  /* 所属核（SMP 静态分区） */
    u8 wait_kind;              /* 1=对象等待，2=发送，3=接收 */
    void *wait_obj;            /* 等待对象 */
    int wait_result;           /* 0=OK，-2=超时，-3=删除 */
    u32 wait_arg;              /* 事件掩码等 */
    u32 suspend_cnt;
    u32 wait_bits;             /* 事件唤醒时返回的位 */
    u64 run_cycles;            /* 累计运行周期（port_cycles 口径） */
    u32 switches;              /* 被调度次数 */
    char name[C6OS_NAME_MAX];
} OsTask;

int os_task_create(const char *name, OsTaskEntry entry, void *arg,
                   u32 prio, u32 stack_size);
int os_task_create_on(u32 core, const char *name, OsTaskEntry entry, void *arg,
                      u32 prio, u32 stack_size);
void os_task_start(void);
void os_task_start_core(u32 core);
u32 os_task_core(u32 id);
int os_task_set_core(u32 id, u32 core);
u32 os_core_ready(u32 core);
void os_task_yield(void);
void os_task_delay(u32 ticks);
void os_sched_tick(void);
int os_task_delete(u32 id);
int os_task_join(u32 id, u32 timeout);
int os_task_suspend(u32 id);
int os_task_resume(u32 id);
int os_task_suspend_count(u32 id);
int os_task_block(void *obj, u32 timeout, u8 kind);
int os_task_block_arg(void *obj, u32 timeout, u8 kind, u32 arg);
u32 os_task_wait_bits(void);
int os_task_wake_event(void *obj, u32 bits);
int os_task_wake_first(void *obj, u8 kind, int result);
u32 os_task_wake_first_id(void *obj, u8 kind, int result);
void os_task_wake_all(void *obj, int result);
void os_task_set_mutex_release_hook(void (*fn)(u32 id));
void os_task_lock(void);
void os_task_unlock(void);
int os_task_set_prio(u32 id, u32 prio);
u32 os_task_get_prio(u32 id);
u32 os_task_switch_count(void);
u64 os_task_run_cycles(u32 id);
u32 os_task_switches(u32 id);
u32 os_obj_lock(void);
void os_obj_unlock(u32 state);
int os_task_block_prepare(void *obj, u32 timeout, u8 kind, u32 arg);
int os_task_block_wait(void);
void os_task_boost(u32 id, u32 prio);
void os_task_restore(u32 id);
void os_task_exit(void);
u32 os_task_self(void);
u32 os_task_count(void);
OsTask *os_task_get(u32 id);
u32 os_task_info(u32 idx, u32 *id, u32 *prio, u32 *state, char *name, u32 cap);

#endif
