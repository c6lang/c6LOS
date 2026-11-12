#ifndef C6OS_LOG_H
#define C6OS_LOG_H

#include "c6os/types.h"

void kputc(int c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);

#endif
