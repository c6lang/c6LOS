#ifndef _LOS_SWTMR_H
#define _LOS_SWTMR_H

#include "los/los_typedef.h"

typedef VOID (*SWTMR_PROC_FUNC)(UINT32 arg);
#define LOS_SWTMR_MODE_ONCE 0u
#define LOS_SWTMR_MODE_PERIOD 1u

UINT32 LOS_SwtmrCreate(UINT32 interval, UINT8 mode, SWTMR_PROC_FUNC proc,
                       UINT32 *timerId, UINT32 arg);
UINT32 LOS_SwtmrStart(UINT32 timerId);
UINT32 LOS_SwtmrStop(UINT32 timerId);
UINT32 LOS_SwtmrDelete(UINT32 timerId);

#endif
