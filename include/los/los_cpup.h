#ifndef _LOS_CPUP_H
#define _LOS_CPUP_H

#include "los/los_typedef.h"

#define LOS_CPUP_PRECISION 1000u
#define LOS_CPUP_PRECISION_MULT (LOS_CPUP_PRECISION / 100u)

typedef struct tagCpupInfo {
    UINT16 usStatus;
    UINT32 uwUsage;
} CPUP_INFO_S;

typedef enum {
    SYS_CPU_USAGE = 0,
    TASK_CPU_USAGE,
} CPUP_TYPE_E;

typedef enum {
    CPUP_IN_10S = 0,
    CPUP_IN_1S,
    CPUP_LESS_THAN_1S,
} CPUP_MODE_E;

UINT32 LOS_SysCpuUsage(VOID);
UINT32 LOS_HistorySysCpuUsage(UINT16 mode);
UINT32 LOS_TaskCpuUsage(UINT32 taskID);
UINT32 LOS_HistoryTaskCpuUsage(UINT32 taskID, UINT16 mode);
UINT32 LOS_AllTaskCpuUsage(CPUP_INFO_S *cpupInfo, UINT16 mode);
UINT32 LOS_CpupUsageMonitor(CPUP_TYPE_E type, CPUP_MODE_E mode, UINT32 taskID);

#endif
