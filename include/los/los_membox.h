#ifndef _LOS_MEMBOX_H
#define _LOS_MEMBOX_H

#include "los/los_typedef.h"

UINT32 LOS_MemboxInit(UINT32 *handle, VOID *pool, UINT32 blkSize, UINT32 blkNum);
VOID *LOS_MemboxAlloc(UINT32 handle);
UINT32 LOS_MemboxFree(UINT32 handle, VOID *p);
UINT32 LOS_MemboxFreeCount(UINT32 handle);

#endif
