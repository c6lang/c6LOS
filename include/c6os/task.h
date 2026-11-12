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

typedef struct OsTask {
    void *ctx;                 /* port 上下文（不透明） */
    OsTaskEntry entry;
    void *arg;
    u32 id;
    u32 stack_size;
    u32 slice;                 /* 剩余时间片（tick） */
    u8 prio;                   /* 0..31，越小越高 */
    u8 state;
    char name[C6OS_NAME_MAX];
} OsTask;

int os_task_create(const char *name, OsTaskEntry entry, void *arg,
                   u32 prio, u32 stack_size);
void os_task_start(void);
void os_task_yield(void);
void os_task_exit(void);
u32 os_task_self(void);
u32 os_task_count(void);
OsTask *os_task_get(u32 id);

#endif
