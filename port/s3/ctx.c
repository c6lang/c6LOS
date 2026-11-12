/* ESP32-S3 call0 协作上下文：内核不透明 ctx，静态栈池。
 * 汇编侧（ctx.S）按 { u32 r[16]; u32 sp; void *stack; u32 size; } 访问。 */

#include "c6os/types.h"
#include "c6os/port.h"

extern void s3_task_tramp(void);
extern void os_task_exit(void);

typedef struct {
    u32 r[16];      /* a0..a15 */
    u32 sp;
    void *stack;
    u32 size;
} S3Ctx;

#define S3_MAX_CTX   6
#define S3_STACK_SZ  4096

S3Ctx g_sched;
S3Ctx *g_s3_cur;

static S3Ctx g_ctx[S3_MAX_CTX];
static u8 g_stacks[S3_MAX_CTX][S3_STACK_SZ] __attribute__((aligned(16)));

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    u32 i = 0;
    S3Ctx *c = 0;
    u32 top;

    (void)stack_size;
    while (i < (u32)S3_MAX_CTX) {
        if (g_ctx[i].stack == 0) {
            c = &g_ctx[i];
            break;
        }
        i++;
    }
    if (c == 0) {
        return 0;
    }

    c->stack = (void *)g_stacks[i];
    c->size = (u32)S3_STACK_SZ;
    top = ((u32)c->stack + (u32)S3_STACK_SZ) & ~15u;

    i = 0;
    while (i < 16u) {
        c->r[i] = 0u;
        i++;
    }
    c->r[0] = (u32)s3_task_tramp;
    c->r[1] = top;
    c->r[2] = (u32)entry;
    c->r[3] = (u32)arg;
    c->sp = top;
    *(volatile u32 *)c->stack = 0xC6C0DEADu;
    return c;
}

void port_task_ctx_free(void *ctx)
{
    S3Ctx *c = (S3Ctx *)ctx;

    if (c == 0) {
        return;
    }
    c->stack = 0;
    c->size = 0u;
}

u32 port_task_stack_ok(void *ctx)
{
    S3Ctx *c = (S3Ctx *)ctx;

    if (c == 0 || c->stack == 0) {
        return 0u;
    }
    return (*(volatile u32 *)c->stack == 0xC6C0DEADu) ? 1u : 0u;
}
