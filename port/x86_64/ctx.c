#include "c6os/types.h"
#include "c6os/port.h"

#define X86_MAX_CTX 6
#define X86_STACK_SZ 8192

typedef struct X86Ctx {
    u64 rsp;          /* offset 0：ctx.S 直接从这里恢复 */
    void (*entry)(void *);
    void *arg;
    u8 *stack;
    u32 size;
} X86Ctx;

static X86Ctx g_ctx[X86_MAX_CTX];
static u8 g_stacks[X86_MAX_CTX][X86_STACK_SZ] __attribute__((aligned(16)));
static u32 g_used;

void *g_cur_ctx;
void *g_sched_sp;

void x86_tramp(void);

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    X86Ctx *c;
    u64 *sp;

    (void)stack_size;
    if (g_used >= X86_MAX_CTX) {
        return 0;
    }
    c = &g_ctx[g_used];
    c->entry = entry;
    c->arg = arg;
    c->stack = g_stacks[g_used];
    c->size = X86_STACK_SZ;
    c->stack[0] = 0xADu;
    c->stack[1] = 0xDEu;
    c->stack[2] = 0xC0u;
    c->stack[3] = 0xC6u;

    sp = (u64 *)(c->stack + X86_STACK_SZ);
    sp -= 1;
    *sp = (u64)&x86_tramp;   /* ret 目标 */
    sp -= 6;
    sp[0] = 0;
    sp[1] = 0;
    sp[2] = 0;
    sp[3] = 0;
    sp[4] = 0;
    sp[5] = 0;
    c->rsp = (u64)sp;
    g_used++;
    return c;
}

void port_task_ctx_free(void *ctx)
{
    (void)ctx;
}

u32 port_task_stack_ok(void *ctx)
{
    X86Ctx *c = (X86Ctx *)ctx;

    if (c == 0 || c->stack == 0) {
        return 0;
    }
    return (c->stack[0] == 0xADu && c->stack[1] == 0xDEu
            && c->stack[2] == 0xC0u && c->stack[3] == 0xC6u) ? 1u : 0u;
}
