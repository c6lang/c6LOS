#ifndef C6OS_HWI_H
#define C6OS_HWI_H

#include "c6os/types.h"

typedef void (*OsHwiHandler)(void *arg);

#define OS_HWI_MAX 32

int os_hwi_create(u32 irq, OsHwiHandler fn, void *arg);
int os_hwi_delete(u32 irq);
int os_hwi_enable(u32 irq, u32 on);
void os_hwi_dispatch(u32 irq);
u32 os_hwi_count(void);

#endif
