#ifndef C6OS_TIMEOUT_H
#define C6OS_TIMEOUT_H

#include "c6os/types.h"

/* 按 deadline 有序的超时队列（M2-2）。deadline 为绝对 tick，
 * 比较用有符号差，允许 u32 回绕。 */
#define OS_TIMEOUT_MAX 128

void os_timeout_init(void);
int os_timeout_add(u32 id, u32 ticks);
int os_timeout_cancel(u32 id);
int os_timeout_pop_expired(u32 now, u32 *id);
u32 os_timeout_count(void);
u32 os_timeout_deadline_at(u32 i);

#endif
