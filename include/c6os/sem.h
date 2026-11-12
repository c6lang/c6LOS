#ifndef C6OS_SEM_H
#define C6OS_SEM_H

#include "c6os/types.h"

typedef struct OsSem {
    u32 count;
    u32 max;
    u32 magic;
} OsSem;

int os_sem_init(OsSem *s, u32 initial, u32 max);
int os_sem_pend(OsSem *s, u32 timeout);
int os_sem_post(OsSem *s);
u32 os_sem_count(OsSem *s);

#endif
