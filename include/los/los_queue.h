#ifndef _LOS_QUEUE_H
#define _LOS_QUEUE_H

#include "los/los_typedef.h"

#define LOS_ERRNO_QUEUE_NO_MEMORY 0x02000301u
#define LOS_ERRNO_QUEUE_INVALID 0x02000302u
#define LOS_ERRNO_QUEUE_TIMEOUT 0x02000303u
#define LOS_ERRNO_QUEUE_FULL 0x02000304u

UINT32 LOS_QueueCreate(const char *name, UINT16 len, UINT32 *queueId,
                       UINT32 flags, UINT16 maxMsgSize);
UINT32 LOS_QueueDelete(UINT32 queueId);
UINT32 LOS_QueueRead(UINT32 queueId, void *buf, UINT32 maxLen, UINT32 timeout);
UINT32 LOS_QueueWrite(UINT32 queueId, const void *buf, UINT32 len, UINT32 timeout);
UINT32 LOS_QueueReadCopy(UINT32 queueId, VOID *buf, UINT32 *len, UINT32 timeout);
UINT32 LOS_QueueWriteCopy(UINT32 queueId, const VOID *buf, UINT32 len, UINT32 timeout);
UINT32 LOS_QueueUsed(UINT32 queueId);

#endif
