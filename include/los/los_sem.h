#ifndef _LOS_SEM_H
#define _LOS_SEM_H

#include "los/los_typedef.h"

#define LOS_ERRNO_SEM_NO_MEMORY 0x02000201u
#define LOS_ERRNO_SEM_INVALID 0x02000202u
#define LOS_ERRNO_SEM_TIMEOUT 0x02000203u

UINT32 LOS_SemCreate(UINT16 count, UINT32 *handle);
UINT32 LOS_SemDelete(UINT32 handle);
UINT32 LOS_SemPend(UINT32 handle, UINT32 timeout);
UINT32 LOS_SemPost(UINT32 handle);

#endif
