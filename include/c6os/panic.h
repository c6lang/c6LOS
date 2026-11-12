#ifndef C6OS_PANIC_H
#define C6OS_PANIC_H

#include "c6os/types.h"
#include "c6os/trap.h"

void os_panic(const char *msg);
void os_panic_frame(const char *msg, const OsTrapFrame *tf);

#endif
