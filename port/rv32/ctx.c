#include "c6os/config.h"
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
void *g_cur_ctx[C6OS_MAX_CORES];
void *g_sched_sp[C6OS_MAX_CORES];

void rv32_tramp(void);

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    RvCtx *c = 0;
    u8 *top;
    u8 *base;
    u32 want = stack_size;
    u32 *sp;
    u32 i = 0;

    if (want < 512u) {
        want = 512u;
    }
    want = (want + 15u) & ~15u;
    if (want > (u32)RV_STACK_SZ) {
        return 0; /* 不静默缩栈：显式拒绝，调用者得到 -3 */
    }
    while (i < (u32)RV_MAX_CTX) {
        if (g_ctx[i].stack == 0) {
            c = &g_ctx[i];
            break;
        }
        i++;
    }
    if (c == 0) {
        return 0;
    }
    c->entry = (u32)entry;
    c->arg = (u32)arg;
    c->size = want;
    c->stack = g_stacks[i] + (RV_STACK_SZ - want);
    base = c->stack;
    base[0] = 0xADu;
    base[1] = 0xDEu;
    base[2] = 0xC0u;
    base[3] = 0xC6u;

    top = g_stacks[i] + RV_STACK_SZ;
    sp = (u32 *)((u32)top - 64u);
    sp = (u32 *)((u32)sp & ~15u);
    i = 0;
    while (i < 16u) {
        sp[i] = 0;
        i++;
    }
    sp[13] = (u32)&rv32_tramp;   /* ra */
    c->sp = (u32)sp;
    return c;
}

void port_task_ctx_free(void *ctx)
{
    RvCtx *c = (RvCtx *)ctx;

    if (c != 0) {
        c->sp = 0;
        c->entry = 0;
        c->arg = 0;
        c->stack = 0;
        c->size = 0;
    }
}

u32 port_irq_save(void)
{
#ifdef C6OS_BARE
    u32 old = 0;
    __asm__ __volatile__("csrr %0, mstatus" : "=r"(old));
    __asm__ __volatile__("csrci mstatus, 8");
    return old;
#else
    return 0u;
#endif
}

void port_irq_restore(u32 state)
{
#ifdef C6OS_BARE
    if ((state & 8u) != 0u) {
        __asm__ __volatile__("csrsi mstatus, 8");
    }
#else
    (void)state;
#endif
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
