/* host/sim 协作上下文：ucontext；只用于参考机测试，不模拟抢占。 */

#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include "c6os/types.h"
#include "c6os/port.h"

extern void os_task_exit(void);

typedef struct {
    ucontext_t uc;
    void *raw;
    void *stack;
    u32 stack_size;
    void (*entry)(void *);
    void *arg;
} SimCtx;

static SimCtx *g_cur;
static SimCtx *g_pending;
static ucontext_t g_sched;

static void tramp(void)
{
    SimCtx *c = g_pending;
    c->entry(c->arg);
    os_task_exit();
}

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    SimCtx *c = (SimCtx *)calloc(1, sizeof(*c));
    u32 sz = (stack_size < 4096u) ? 4096u : stack_size;
    u32 alloc = sz + 65536u;   /* 溢出测试留安全垫，避免踩坏宿主堆 */

    if (c == 0) {
        return 0;
    }
    c->raw = malloc(alloc);
    c->stack = (u8 *)c->raw + 65536u;
    if (c->stack == 0) {
        free(c);
        return 0;
    }
    c->stack_size = sz;
    c->entry = entry;
    c->arg = arg;
    if (getcontext(&c->uc) != 0) {
        free(c->raw);
        free(c);
        return 0;
    }
    c->uc.uc_stack.ss_sp = c->stack;
    c->uc.uc_stack.ss_size = sz;
    c->uc.uc_link = 0;
    {
        u32 sentinel = 0xC6C0DEADu;
        memcpy(c->stack, &sentinel, sizeof(sentinel));
    }
    makecontext(&c->uc, tramp, 0);
    return c;
}

void port_task_ctx_free(void *ctx)
{
    SimCtx *c = (SimCtx *)ctx;
    if (c == 0) {
        return;
    }
    free(c->raw);
    free(c);
}

void port_task_switch(void *ctx)
{
    SimCtx *c = (SimCtx *)ctx;
    g_cur = c;
    g_pending = c;
    swapcontext(&g_sched, &c->uc);
}

void port_task_yield(void)
{
    if (g_cur != 0) {
        swapcontext(&g_cur->uc, &g_sched);
    }
}

u32 port_task_stack_ok(void *ctx)
{
    SimCtx *c = (SimCtx *)ctx;
    u32 sentinel = 0;

    if (c == 0 || c->stack == 0) {
        return 0;
    }
    memcpy(&sentinel, c->stack, sizeof(sentinel));
    return sentinel == 0xC6C0DEADu ? 1u : 0u;
}
