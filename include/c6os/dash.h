#ifndef C6OS_DASH_H
#define C6OS_DASH_H

#include "c6os/gfx.h"

void os_dash_render(OsGfx *g, u32 tick, u32 tasks, u32 heap_used);
void os_dash_task_row(OsGfx *g, int y, u32 id, u32 prio, char state,
                      const char *name);
void os_dash_log(OsGfx *g, int y, const char *s);

#endif
