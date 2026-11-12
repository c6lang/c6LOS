#ifndef C6OS_QUEUE_H
#define C6OS_QUEUE_H

#include "c6os/types.h"

typedef struct OsQueue {
    void **buf;
    u32 cap;
    u32 head;
    u32 tail;
    u32 count;
    u32 magic;
} OsQueue;

int os_queue_init(OsQueue *q, void **storage, u32 cap);
int os_queue_put(OsQueue *q, void *item, u32 timeout);
void *os_queue_get(OsQueue *q, u32 timeout);
u32 os_queue_count(OsQueue *q);

#endif
