#ifndef C6OS_TIMER_H
#define C6OS_TIMER_H

#include "c6os/types.h"

typedef void (*OsTimerCb)(void *);

typedef struct OsTimer {
    u32 period;      /* tick */
    u32 expire;      /* 绝对 tick */
    OsTimerCb cb;
    void *arg;
    u8 active;
    u8 periodic;
    u32 magic;
} OsTimer;

int os_timer_init(OsTimer *t, u32 period, int periodic, OsTimerCb cb, void *arg);
int os_timer_start(OsTimer *t);
int os_timer_stop(OsTimer *t);
void os_timer_tick(void);

#endif
