/* host/sim 上下文：ucontext + SIGALRM 模拟定时器中断抢占。
 * 信号处理栈必须落在任务自己的栈上（SA_ONSTACK 会让所有任务共用一块
 * altstack，后一次中断覆盖前一个任务的 sigreturn 帧）。 */

#include <ucontext.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "c6os/types.h"
#include "c6os/tick.h"
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
static volatile sig_atomic_t g_in_task;
static volatile sig_atomic_t g_preempt_on;

void port_enter_task(void)
{
    g_in_task = 1;
}

void port_leave_task(void)
{
    g_in_task = 0;
}

static void tick_sig(int sig)
{
    (void)sig;
    if (!g_preempt_on) {
        return;
    }
    os_sched_tick();
    if (g_in_task != 0 && g_cur != 0) {
        SimCtx *prev = g_cur;
        SimCtx *next = (SimCtx *)os_sched_preempt_ctx();

        if (next != 0 && next != prev) {
            g_cur = next;
            g_pending = next;
            swapcontext(&prev->uc, &next->uc);
            g_cur = prev;
            g_pending = prev;
            g_in_task = 1;
        }
    }
}

void port_sim_preempt_start(void)
{
    static int installed;
    struct sigaction sa;
    struct itimerval it;

    if (installed != 0) {
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tick_sig;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);

    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 10000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, 0);
    installed = 1;
    g_preempt_on = 1;
}

static void alarm_block(sigset_t *old)
{
    sigset_t s;

    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigprocmask(SIG_BLOCK, &s, old);
}

static void alarm_restore(sigset_t *old)
{
    sigprocmask(SIG_SETMASK, old, 0);
}

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
    sigset_t old;

    g_cur = c;
    g_pending = c;
    alarm_block(&old);
    g_in_task = 1;
    swapcontext(&g_sched, &c->uc);
    g_in_task = 0;
    alarm_restore(&old);
}

void port_task_yield(void)
{
    if (g_cur != 0) {
        sigset_t old;

        alarm_block(&old);
        g_in_task = 0;
        swapcontext(&g_cur->uc, &g_sched);
        g_in_task = 1;
        alarm_restore(&old);
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
