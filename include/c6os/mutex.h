#ifndef C6OS_MUTEX_H
#define C6OS_MUTEX_H

#include "c6os/types.h"

typedef struct OsMutex {
    u32 owner;       /* 任务 id，0=空闲 */
    u32 count;       /* 递归计数 */
    u32 magic;
} OsMutex;

int os_mutex_init(OsMutex *m);
int os_mutex_destroy(OsMutex *m);
void os_mutex_release_owner(u32 id);
int os_mutex_pend(OsMutex *m, u32 timeout);
int os_mutex_post(OsMutex *m);
u32 os_mutex_owner(OsMutex *m);

#endif
