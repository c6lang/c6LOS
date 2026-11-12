#ifndef _LOS_TASK_H
#define _LOS_TASK_H

#include "los/los_typedef.h"

typedef VOID (*TSK_ENTRY_FUNC)(VOID *arg);

/* 只保留 LiteOS-M 常用字段；顺序在本兼容层固定。 */
typedef struct tagTskInitParam {
    TSK_ENTRY_FUNC pfnTaskEntry;
    VOID *pArgs;
    UINT16 usTaskPrio;
    UINT32 uwStackSize;
    const CHAR *pcName;
} TSK_INIT_PARAM_S;

#define LOS_ERRNO_TSK_INVALID 0x02000201u
#define LOS_ERRNO_TSK_NO_MEMORY 0x02000202u

UINT32 LOS_TaskCreate(UINT32 *taskId, TSK_INIT_PARAM_S *initParam);
UINT32 LOS_TaskDelete(UINT32 taskId);
UINT32 LOS_TaskDelay(UINT32 tick);
UINT32 LOS_TaskSuspend(UINT32 taskId);
UINT32 LOS_TaskResume(UINT32 taskId);
UINT32 LOS_TaskYield(VOID);
VOID LOS_TaskLock(VOID);
VOID LOS_TaskUnlock(VOID);
UINT32 LOS_TaskPriSet(UINT32 taskId, UINT16 taskPrio);
UINT32 LOS_CurTaskPriSet(UINT16 taskPrio);
UINT32 LOS_TaskPriGet(UINT32 taskId);
UINT32 LOS_TaskSwitchInfoGet(VOID);
UINT32 LOS_TaskIDGet(VOID);
UINT32 LOS_TaskCount(VOID);

#endif
