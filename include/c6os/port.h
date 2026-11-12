#ifndef C6OS_PORT_H
#define C6OS_PORT_H

#include "c6os/types.h"

/* M0 最小端口面。后续里程碑按 docs/architecture.md §14 增补。 */

void port_init(void);
void port_idle(void);
void port_panic_halt(void);

/* 任务上下文（协作 port）：ctx 对内核不透明。 */
void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size);
void port_task_ctx_free(void *ctx);
void port_task_switch(void *ctx);
void port_task_yield(void);
u32 port_task_stack_ok(void *ctx);
u32 port_virtual_tick(void);

void port_putc(int ch);
void port_puts(const char *s);
void port_delay_ms(u32 ms);

#endif
