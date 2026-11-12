#ifndef C6OS_SHELL_H
#define C6OS_SHELL_H

#include "c6os/types.h"
#include "c6os/heap.h"

void os_shell_bind_heap(OsHeap *h);
int os_shell_exec(const char *line);

#endif
