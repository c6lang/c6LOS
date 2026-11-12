#ifndef C6OS_PORT_H
#define C6OS_PORT_H

#include "c6os/types.h"

/* M0 最小端口面。后续里程碑按 docs/architecture.md §14 增补。 */

void port_init(void);
void port_idle(void);
void port_panic_halt(void);

void port_putc(int ch);
void port_puts(const char *s);
void port_delay_ms(u32 ms);

#endif
