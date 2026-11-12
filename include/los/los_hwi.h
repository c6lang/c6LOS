#ifndef _LOS_HWI_H
#define _LOS_HWI_H

#include "los_typedef.h"

#define LOS_ERRNO_HWI_INVALID   0x02000A01u
#define LOS_ERRNO_HWI_NO_MEMORY 0x02000A02u

typedef void (*HWI_PROC_FUNC)(void *arg);

UINT32 LOS_HwiCreate(UINT32 irq, UINT16 prio, UINT32 mode,
                     HWI_PROC_FUNC fn, void *arg);
UINT32 LOS_HwiDelete(UINT32 irq);
UINT32 LOS_HwiEnable(UINT32 irq);
UINT32 LOS_HwiDisable(UINT32 irq);
UINT32 LOS_HwiTrigger(UINT32 irq);

#endif
