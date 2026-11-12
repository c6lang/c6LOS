/* ESP32-S3 call0 上下文：内核不透明 ctx，静态栈池。
 * ctx->sp 指向该任务栈上的 80 字节陷阱帧（a0/a2..a15 + EPC + PS）；
 * r[16] 仅为保持 sp 在偏移 64 的汇编布局，已不使用。 */

#include "c6os/config.h"
#include "c6os/types.h"
#include "c6os/port.h"

extern void s3_task_tramp(void);
extern void os_task_exit(void);
extern u32 port_ps_get(void);

typedef struct {
    u32 r[16];      /* 占位，保持 sp@64 布局 */
    u32 sp;
    void *stack;
    u32 size;
} S3Ctx;

/* ctx.S 用 76 字节步长索引 g_sched[]，布局变了必须同步。 */
typedef char s3_ctx_size_check[(sizeof(S3Ctx) == 76) ? 1 : -1];

#define S3_MAX_CTX   6
#define S3_STACK_SZ  4096

S3Ctx g_sched[C6OS_MAX_CORES];
S3Ctx *g_s3_cur[C6OS_MAX_CORES];

static S3Ctx g_ctx[S3_MAX_CTX];
static u8 g_stacks[S3_MAX_CTX][S3_STACK_SZ] __attribute__((aligned(16)));

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    u32 i = 0;
    S3Ctx *c = 0;
    u32 top;
    u32 *f = 0;

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
    top = (((u32)c->stack + (u32)S3_STACK_SZ) & ~15u) - 80u;
    f = (u32 *)top;

    i = 0;
    while (i < 17u) {
        f[i] = 0u;
        i++;
    }
    f[1] = (u32)entry;                 /* a2 */
    f[2] = (u32)arg;                   /* a3 */
    f[15] = (u32)s3_task_tramp;        /* EPC */
    f[16] = port_ps_get() & ~0x1Fu;    /* PS */
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

void port_leave_task(void)
{
    g_s3_cur[port_core_id() & (C6OS_MAX_CORES - 1u)] = 0;
}

void *port_cur_ctx(void)
{
    return (void *)g_s3_cur[port_core_id() & (C6OS_MAX_CORES - 1u)];
}

void port_set_cur_ctx(void *ctx)
{
    g_s3_cur[port_core_id() & (C6OS_MAX_CORES - 1u)] = (S3Ctx *)ctx;
}

/* 把 level-1 陷阱帧复制进当前任务 ctx：frame[15]=EPC1, frame[16]=PS。 */
void port_irq_fill(u32 frame_sp)
{
    S3Ctx *c = g_s3_cur[port_core_id() & (C6OS_MAX_CORES - 1u)];

    if (c != 0) {
        c->sp = frame_sp;
    }
}

u32 port_task_stack_ok(void *ctx)
{
    S3Ctx *c = (S3Ctx *)ctx;

    if (c == 0 || c->stack == 0) {
        return 0u;
    }
    return (*(volatile u32 *)c->stack == 0xC6C0DEADu) ? 1u : 0u;
}
