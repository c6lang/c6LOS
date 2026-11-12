#ifndef _LOS_QUEUE_H
#define _LOS_QUEUE_H

#include "los/los_typedef.h"

#define LOS_ERRNO_QUEUE_NO_MEMORY 0x02000301u
#define LOS_ERRNO_QUEUE_INVALID 0x02000302u
#define LOS_ERRNO_QUEUE_TIMEOUT 0x02000303u
#define LOS_ERRNO_QUEUE_FULL 0x02000304u
#define LOS_ERRNO_QUEUE_PTR_NULL 0x02000305u
#define LOS_ERRNO_QUEUE_NOT_CREATE 0x02000306u
#define LOS_ERRNO_QUEUE_ISFULL LOS_ERRNO_QUEUE_FULL
#define LOS_ERRNO_QUEUE_ISEMPTY 0x02000307u
#define LOS_ERRNO_QUEUE_READ_SIZE_TOO_SMALL 0x02000308u
#define LOS_ERRNO_QUEUE_WRITE_SIZE_TOO_BIG 0x02000309u

UINT32 LOS_QueueCreate(const char *name, UINT16 len, UINT32 *queueId,
                       UINT32 flags, UINT16 maxMsgSize);
UINT32 LOS_QueueDelete(UINT32 queueId);
UINT32 LOS_QueueRead(UINT32 queueId, void *buf, UINT32 maxLen, UINT32 timeout);
UINT32 LOS_QueueWrite(UINT32 queueId, const void *buf, UINT32 len, UINT32 timeout);
UINT32 LOS_QueueReadCopy(UINT32 queueId, VOID *buf, UINT32 *len, UINT32 timeout);
UINT32 LOS_QueueWriteCopy(UINT32 queueId, const VOID *buf, UINT32 len, UINT32 timeout);
#define OS_WAIT_TASK_ARRAY_LEN 1u

typedef struct tagQueueInfo {
    UINT32 queueID;
    UINT16 queueLen;
    UINT16 queueSize;
    UINT16 queueHead;
    UINT16 queueTail;
    UINT16 writableCnt;
    UINT16 readableCnt;
    UINT32 waitReadTask[OS_WAIT_TASK_ARRAY_LEN];
    UINT32 waitWriteTask[OS_WAIT_TASK_ARRAY_LEN];
    UINT32 waitMemTask[OS_WAIT_TASK_ARRAY_LEN];
} QUEUE_INFO_S;

UINT32 LOS_QueueInfoGet(UINT32 queueId, QUEUE_INFO_S *info);
UINT32 LOS_QueueUsed(UINT32 queueId);

#endif
