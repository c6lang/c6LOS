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

/* 任务数上限：闭源 WiFi 库自己也会建任务（libpp 的 ppTask、net80211 的
 * wifi_task 等），加上我们的 bringup 任务、监视任务、lwIP 的 tcpip 线程，
 * 3 个槽早就不够用了（§219）。 */
#define S3_MAX_CTX       8
/* 栈池：所有任务的栈从这一块里按需切（按请求大小 + 16 字节对齐），
 * 不再"每个任务固定 8KB"。总大小受 DRAM 限制（§210/§214），32KB 是权衡值：
 * 实测预算是：bringup/wifi 任务 8KB + lwIP tcpip 4KB + libpp ppTask 4KB
 * + 监视任务 2KB ≈ 18KB，24KB 与"原来 3×8KB"的 bss 占用完全一样（不涨 DRAM），
 * 但把"最多 3 个任务"变成了"最多 8 个任务、按需切栈"。
 * 注意：任务不会被删除，所以这里不做回收（和之前一样）。 */
#define S3_STACK_POOL    24576u

/* 窗口 spill 的总开关（w2c.S 的每个壳在进内核前读它）。
 *   0 = 完全不动窗口状态（和以前的行为一致，保住已通过的 PHY/串口基线）
 *   1 = 壳里按 IDF 的做法把祖先窗口整圈 spill + WINDOWSTART 归一化
 * 由 s3_wifi_osi_init()（wifi_glue.c）在进闭源库之前打开 —— 只有闭源库那几条
 * 调用链需要窗口语义，我们的 call0 任务根本不需要，别让它们冒这个险。 */
u32 g_s3_win_spill_en = 0u;

S3Ctx g_sched[C6OS_MAX_CORES];
/* 诊断计数（ctx.S 里自增后覆写到 RTC，避免 RTC 初值随机的干扰） */
/* 下一个要创建的任务是否 windowed 入口（1=是）。
 * s3_task_tramp 用 callx0 调我们自己的 call0 任务，
 * 但库创建的 ppTask 是 windowed 的，必须用 callx8 + WOE 才能正确 entry/retw。*/
u32 g_next_task_windowed = 0;
u32 g_win_scratch[8] = {0};
/* §258：恢复 WINDOWBASE 时需要先记住帧指针（寄存器换映射后 a1 就变了） */
u32 g_ctx_tmp_sp = 0;
u32 g_ts_cnt = 0;   /* 任务启动次数 */
u32 g_sw_cnt = 0;
u32 g_yl_cnt = 0;
S3Ctx *g_s3_cur[C6OS_MAX_CORES];

static S3Ctx g_ctx[S3_MAX_CTX];
static u8 g_stack_pool[S3_STACK_POOL] __attribute__((aligned(16)));
static u32 g_pool_off = 0u;
static u32 g_ctx_dbg_n = 0u;

void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size)
{
    u32 i = 0;
    S3Ctx *c = 0;
    u32 top;
    u32 want = stack_size;
    u32 *f = 0;

    if (want < 1024u) {
        want = 1024u;         /* 512 太少了：闭源库/ lwIP 的线程都要几千字节 */
    }
    want = (want + 15u) & ~15u;
    if (g_pool_off + want > S3_STACK_POOL) {
        /* 栈池不够：**必须吭声**。以前这里静默返回 0，表现是"某个任务凭空
         * 没起来"，很难查（§219）。 */
        {
            extern int ets_printf(const char *fmt, ...);
            volatile u32 *m = (volatile u32 *)0x50000BE0u;

            m[0] = want;
            m[1] = g_pool_off;
            m[2] = S3_STACK_POOL;
            m[3] = m[3] + 1u;
            ets_printf("[ctx] 栈池不够：需要 %u，已用 %u/%u\n",
                       (unsigned)want, (unsigned)g_pool_off, (unsigned)S3_STACK_POOL);
        }
        return 0;
    }
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

    /* 诊断：记录每次分配的槽号/栈基址/请求大小（BSS 计数，RTC 存副本） */
    if (g_ctx_dbg_n < 4u) {
        volatile u32 *cd = (volatile u32 *)0x50000460u;
        u32 k_ = g_ctx_dbg_n;
        g_ctx_dbg_n = k_ + 1u;
        cd[k_ * 3u] = i + 1u;
        cd[k_ * 3u + 1u] = (u32)(void *)(g_stack_pool + g_pool_off);
        cd[k_ * 3u + 2u] = want;
    }
    c->stack = (void *)(g_stack_pool + g_pool_off);
    c->size = want;
    g_pool_off = g_pool_off + want;
    top = ((u32)(g_stack_pool + g_pool_off) & ~15u) - 80u;
    f = (u32 *)top;

    /* §339：**必须把整帧 20 个字都清零**。原来只清 17 个字（a0..a15 + EPC
     * + PS），帧偏移 68（WINDOWSTART）和 72（WINDOWBASE）里是栈上的垃圾；
     * RESTORE_FRAME 看到"WINDOWSTART 非 0"就跳过归一化、直接把垃圾写进
     * WINDOWSTART —— 新任务一开始窗口账目就是错的，闭源库的 ppTask 第一次
     * 用 ROM 打印就 `retw.n` 炸掉（cause=0、a0=PS 值 0x60900，§327/§338）。 */
    i = 0;
    while (i < 20u) {
        f[i] = 0u;
        i++;
    }
    f[1] = (u32)entry;                 /* a2 */
    f[3] = g_next_task_windowed;       /* a4：入口的 ABI 标志 */
    g_next_task_windowed = 0;
    f[2] = (u32)arg;                   /* a3 */
    f[15] = (u32)s3_task_tramp;        /* EPC */
    /* PS：清掉中断级别位，并**置 WOE（bit18）**。闭源库是 windowed ABI，
     * 它的 call/retw 依赖窗口溢出/下溢向量来处理寄存器窗口轮转；
     * WOE=0 时这些向量不生效，第一次窗口轮转就会取到错位的返回地址
     * （真板表现：a0 带着 CALLINC 标记被当指令地址取、cause=0x14）。 */
    f[16] = (port_ps_get() & ~0x1Fu) | 0x40000u;
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

/* §294：切换目标是闭源库的任务时，它的 sp 必须是"自己那块栈里、且 16 字节对齐"
 * 的值。历史上出过 sp 一路走穿到 DRAM 底部（0x3fca0565，还不对齐）然后在 ROM 里
 * 取到非法指令的事故；这个守卫把第一现场打出来并停机。 */
void port_task_ctx_check(void *ctx)
{
    S3Ctx *c = (S3Ctx *)ctx;
    u32 sp;
    u32 lo;
    u32 hi;

    if (c == 0) {
        return;
    }
    lo = (u32)(c->stack);
    hi = lo + (u32)(c->size);
    sp = c->sp;
    if (lo == 0u || sp < lo || sp >= hi || (sp & 3u) != 0u) {
        extern int ets_printf(const char *fmt, ...);
        volatile u32 *m = (volatile u32 *)0x50000100u;
        m[0] = 0xBAD1u;
        m[1] = sp;
        m[2] = lo;
        m[3] = hi;
        m[4] = (u32)(void *)c;
        ets_printf("[ctx] sp 越界：sp=%08x stack=[%08x,%08x) ctx=%08x\n",
                   (unsigned)sp, (unsigned)lo, (unsigned)hi, (unsigned)(u32)(void *)c);
        while (1) {
        }
    }
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
extern u32 os_task_current_id(void);

void port_irq_fill(u32 frame_sp)
{
    S3Ctx *c = g_s3_cur[port_core_id() & (C6OS_MAX_CORES - 1u)];

    /* §276（真板实测找到的病根）：这里以前无条件把 frame_sp 记成"当前任务
     * 的保存 sp"。但 g_s3_cur 指的那个 ctx 不一定就是**真正在跑**的那个任务
     * （典型情况：闭源库的 ppTask 挂在 g_s3_cur 上，而主流程其实跑在主任务
     * 的栈上）。于是别人的陷阱帧被记进了 ppTask 的 ctx —— JTAG 实测
     * ctx[1]（ppTask，栈 0x3fcbee00+0x1800）的 sp 是 0x3fcd8ea0，那是主任务
     * 栈上的帧，比它自己的栈高 41KB。它下次被恢复时 sp 就指到别的栈上，
     * 一路弹栈弹进数据区，最后跳到 task.c6 的静态数据（§273/§274）。
     *
     * 修正：只有帧确实落在这个任务自己的栈范围内，才认它是保存点。 */
    if (c != 0 && c->stack != 0) {
        u32 lo = (u32)(void *)c->stack;
        u32 hi = lo + c->size;

        if (frame_sp >= lo && frame_sp < hi) {
            c->sp = frame_sp;
        } else {
            volatile u32 *r = (volatile u32 *)0x500017C0u;

            r[0] = r[0] + 1u;        /* 被拒的次数 */
            r[1] = frame_sp;         /* 最后一个被拒的帧 */
            r[2] = (u32)(void *)c;   /* 当时 g_s3_cur 指的 ctx */
        }
    }
    /* 陷阱帧里的 PS 也要带上 WOE：中断进来时硬件保存的是被打断代码的 PS，
     * 而被打断的多半是我们自己的 call0 代码（WOE=0）。如果不补这一位，
     * 中断返回后 WiFi 任务里的 windowed 调用就会因为 WOE=0 而失败。 */
    if (frame_sp != 0u) {
        *(volatile u32 *)(frame_sp + 64u) |= 0x40000u;
    }
    {
        /* 诊断：ISR 抢占时保存的是哪个 ctx、哪个帧（RTC 0x50001700 起，
         * 每项 3 字：序号、ctx、frame_sp） */
        static u32 n_;
        volatile u32 *d = (volatile u32 *)0x50001700u;
        u32 k_ = n_ & 7u;
        n_ = n_ + 1u;
        d[k_ * 4u]      = n_;
        d[k_ * 4u + 1u] = (u32)(void *)c;
        d[k_ * 4u + 2u] = frame_sp;
        d[k_ * 4u + 3u] = os_task_current_id();   /* 内核视角的当前任务 */
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
