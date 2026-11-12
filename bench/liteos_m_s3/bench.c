/* LiteOS-M 同板微基准：与 c6LOS tests/t_perf.c6 同口径（CCOUNT/op）。 */
#include "los_task.h"
#include "los_sem.h"
#include "los_queue.h"
#include "los_memory.h"
#include "los_config.h"

#define MMIO_CONSOLE 0x40000000u
#define MMIO_EXIT    0x40000004u
#define N 1000u

static void pc(int c) { *(volatile unsigned int *)MMIO_CONSOLE = (unsigned int)(unsigned char)c; }
static void ps(const char *s) { while (*s) pc(*s++); }
static void pu(unsigned int v) {
    char b[12]; int i = 0;
    if (v == 0u) { pc('0'); return; }
    while (v > 0u && i < 12) { b[i] = (char)('0' + (v % 10u)); i++; v /= 10u; }
    while (i > 0) { i--; pc(b[i]); }
}
static unsigned int cyc(void) { unsigned int v; __asm__ __volatile__("rsr %0, ccount" : "=r"(v)); return v; }
static void rep(const char *name, unsigned int d) { ps(name); ps(": "); pu(d / N); ps(" cycles/op\n"); }

static unsigned int g_yield, g_sem, g_queue, g_heap;
static UINT32 g_semid = 0;
static UINT32 g_qid = 0;

static VOID *benchTask(UINT32 arg) {
    unsigned int i, t0, t1;
    UINT32 qv = 0x12345678u, qout = 0, qlen;
    void *p;
    (void)arg;

    t0 = cyc(); for (i = 0; i < N; i++) { LOS_TaskYield(); } t1 = cyc(); g_yield = t1 - t0;
    if (LOS_SemCreate(0, &g_semid) == LOS_OK) {
        t0 = cyc(); for (i = 0; i < N; i++) { (void)LOS_SemPost(g_semid); (void)LOS_SemPend(g_semid, 0); } t1 = cyc();
        g_sem = t1 - t0;
    }
    if (LOS_QueueCreate("q", 8, &g_qid, 0, 4) == LOS_OK) {
        t0 = cyc();
        for (i = 0; i < N; i++) {
            (void)LOS_QueueWriteCopy(g_qid, &qv, 4, 0);
            qlen = 4; (void)LOS_QueueReadCopy(g_qid, &qout, &qlen, 0);
        }
        t1 = cyc(); g_queue = t1 - t0;
    }
    t0 = cyc();
    for (i = 0; i < N; i++) {
        p = LOS_MemAlloc((void *)LOSCFG_SYS_HEAP_ADDR, 32);
        if (p != 0) { (void)LOS_MemFree((void *)LOSCFG_SYS_HEAP_ADDR, p); }
    }
    t1 = cyc(); g_heap = t1 - t0;

    rep("yield", g_yield);
    rep("sem_post+pend", g_sem);
    rep("queue_put+get", g_queue);
    rep("heap_alloc+free", g_heap);
    ps("LiteOS-M perf done\n");
    *(volatile unsigned int *)MMIO_EXIT = 0u;
    while (1) {}
    return 0;
}

int bench_start(void) {
    UINT32 id = 0;
    TSK_INIT_PARAM_S p;
    p.pfnTaskEntry = benchTask;
    p.usTaskPrio = 5;
    p.uwArg = 0;
    p.stackAddr = 0;
    p.uwStackSize = 0x2000;
    p.pcName = "bench";
    p.uwResved = 0;
    return (LOS_TaskCreate(&id, &p) == LOS_OK) ? 0 : -1;
}
