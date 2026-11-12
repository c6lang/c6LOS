#ifndef C6OS_PORT_H
#define C6OS_PORT_H

#include "c6os/types.h"

/* M0 最小端口面。后续里程碑按 docs/architecture.md §14 增补。 */

void port_init(void);
void port_idle(void);
void port_panic_halt(void);
void port_mark(u32 v);

/* 任务上下文（协作 port）：ctx 对内核不透明。 */
void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size);
void port_task_ctx_free(void *ctx);
void port_task_switch(void *ctx);
void port_enter_task(void);
void port_leave_task(void);
void port_task_yield(void);
u32 port_task_stack_ok(void *ctx);
u32 port_virtual_tick(void);
u32 port_in_isr(void);
u32 port_irq_save(void);
void port_irq_restore(u32 state);
void os_test_set_isr(u32 v);
void port_sim_preempt_start(void);
void *os_sched_preempt_ctx(void);

void port_putc(int ch);
void port_puts(const char *s);
void port_delay_ms(u32 ms);

#endif
