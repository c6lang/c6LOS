#ifndef _LOS_MEM_H
#define _LOS_MEM_H

#include "los/los_typedef.h"

UINT32 LOS_MemInit(VOID *pool, UINT32 size);
VOID *LOS_MemAlloc(VOID *pool, UINT32 size);
UINT32 LOS_MemFree(VOID *pool, VOID *ptr);
UINT32 LOS_MemUsed(VOID *pool);

#endif
