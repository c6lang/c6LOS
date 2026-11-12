#include "c6os/types.h"
#include "c6os/port.h"

#define RV_MAX_CTX 6
#define RV_STACK_SZ 8192

typedef struct RvCtx {
    u32 sp;           /* offset 0 */
    u32 entry;
    u32 arg;
    u8 *stack;
    u32 size;
} RvCtx;

static RvCtx g_ctx[RV_MAX_CTX];
static u8 g_stacks[RV_MAX_CTX][RV_STACK_SZ] __attribute__((aligned(16)));
static u32 g_used;

void *g_cur_ctx;
void *g_sched_sp;

void rv32_tramp(void);

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    RvCtx *c;
    u8 *top;
    u32 *sp;

    (void)stack_size;
    if (g_used >= RV_MAX_CTX) {
        return 0;
    }
    c = &g_ctx[g_used];
    c->entry = (u32)entry;
    c->arg = (u32)arg;
    c->stack = g_stacks[g_used];
    c->size = RV_STACK_SZ;
    c->stack[0] = 0xADu;
    c->stack[1] = 0xDEu;
    c->stack[2] = 0xC0u;
    c->stack[3] = 0xC6u;

    top = c->stack + RV_STACK_SZ;
    sp = (u32 *)((u32)top - 64u);
    sp = (u32 *)((u32)sp & ~15u);
    {
        u32 i = 0;
        while (i < 16u) {
            sp[i] = 0;
            i++;
        }
    }
    sp[13] = (u32)&rv32_tramp;   /* ra */
    c->sp = (u32)sp;
    g_used++;
    return c;
}

void port_task_ctx_free(void *ctx)
{
    (void)ctx;
}

u32 port_task_stack_ok(void *ctx)
{
    RvCtx *c = (RvCtx *)ctx;

    if (c == 0 || c->stack == 0) {
        return 0;
    }
    return (c->stack[0] == 0xADu && c->stack[1] == 0xDEu
            && c->stack[2] == 0xC0u && c->stack[3] == 0xC6u) ? 1u : 0u;
}
