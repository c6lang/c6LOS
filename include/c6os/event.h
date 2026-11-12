#ifndef C6OS_EVENT_H
#define C6OS_EVENT_H

#include "c6os/types.h"

#define OS_EVENT_ANY 0u
#define OS_EVENT_ALL 1u

typedef struct OsEvent {
    u32 bits;
    u32 magic;
} OsEvent;

int os_event_init(OsEvent *e);
int os_event_destroy(OsEvent *e);
int os_event_set(OsEvent *e, u32 bits);
int os_event_clear(OsEvent *e, u32 bits);
u32 os_event_get(OsEvent *e);
u32 os_event_wait(OsEvent *e, u32 mask, u32 mode, u32 timeout);

#endif
