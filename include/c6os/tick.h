#ifndef C6OS_TICK_H
#define C6OS_TICK_H

#include "c6os/types.h"

/* 内核 tick（M2）。目标机由 TIMG0 中断推进；host 测试用 os_tick_advance。 */
u64 os_tick_get(void);
void os_tick_advance(u32 n);
u32 os_tick_hz(void);
u64 os_ms_to_ticks(u64 ms);
u64 os_ticks_to_ms(u64 ticks);

#endif
