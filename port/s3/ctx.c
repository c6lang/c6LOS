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
/* §431：24576 已经不够了 —— 闭源库会建 **3 个** 6144 字节的任务
 * （ppTask / wifi task / net80211），加上我们自己的 wifi 主任务 8192：
 *   8192 + 6144*3 = 26624 > 24576
 * 于是第三次 `_task_create` 直接失败，库打印
 *   `create wifi task: failed to create task`
 * 并让 `esp_wifi_init_internal` 返回 257 (ESP_ERR_INVALID_ARG)。
 * 真板诊断 `0x50000BE0`：需要=6144 已用=20480 池大小=24576。
 *
 * 26624 正好是上面那个和；`.bss` 剩下的 2048 字节刚好够（`_stack_limit`
 * 还会再验一遍，超了链接期就报 "S3 BSS/stack collision"）。 */
/* §436：26624 仍然不够。真板读 `g_pool_off = 26624`（池满）同时
 * `0x50000BE0` 记着"需要=6144" —— 即**还有第 4 个库任务**建不出来。
 * 闭源库一共要 4 个 6144 的任务（ppTask / wifi / net80211 / 还有一个），
 * 加上我们自己的 wifi 主任务。
 *
 * §449：wifi 主任务从 8192 抬到 16384 —— 真板实测它 `sp` 超出自己 8192 的栈区
 * 5168 字节（压进了库任务栈），根因是 `esp_wifi_init_internal()` 跑得太深。
 *   16384 + 6144*4 = 40960
 * 池正好给到 40960。**不能再大**：45056 会让 `.bss` 撞上硬编码的
 * `S3_STACK_TOP=0x3FCD9500`（链接器报 "S3 BSS/stack collision"）。
 * 池从 0x9000 涨到 0xA000，`_bss_end` 上移 0x1000，
 * 主栈从约 20KB 降到约 16KB（§432 那次只剩 146 字节才出事，16KB 够用）。 */
#define S3_STACK_POOL    40960u

/* 窗口 spill 的总开关（w2c.S 的每个壳在进内核前读它）。
 *   0 = 完全不动窗口状态（和以前的行为一致，保住已通过的 PHY/串口基线）
 *   1 = 壳里按 IDF 的做法把祖先窗口整圈 spill + WINDOWSTART 归一化
 * 由 s3_wifi_osi_init()（wifi_glue.c）在进闭源库之前打开 —— 只有闭源库那几条
 * 调用链需要窗口语义，我们的 call0 任务根本不需要，别让它们冒这个险。 */
/* §458：初值改成 1。§457 修好 WINDOWBASE/WINDOWSTART 的逐任务恢复之后，
 * 露出来的下一层就是"窗口寄存器的内容没跟着任务走"：库代码在 `callx8`
 * 之后从栈上欠流恢复 `a3`，读到的却是上一个任务的残留，于是 `*(a3)==0`
 * 再 `l32i 0x15c` 就崩（真板 `cause=0x1c vaddr=0x15c pc=0x42057049`）。
 *
 * 必须**从头**就开：崩点在 PHY 阶段（`register_chipv7_phy` 之后），
 * 早于 `s3_wifi_bringup()` 里的 `s3_wifi_osi_init()`，只靠
 * `-DS3_WIN_SPILL` 在那个函数里置位来不及。
 * §447 已经把"开关形同虚设"的 bug 修掉，现在 spill 只在 `w2c.S` 的
 * 3 个壳里跑（都有 `l32r a8,12b / l32i / beqz a8, 5f` 守卫），
 * 不会再在 call0 内核帧上触发溢出处理体写飞。 */
u32 g_s3_win_spill_en = 1u;

S3Ctx g_sched[C6OS_MAX_CORES];
/* 诊断计数（ctx.S 里自增后覆写到 RTC，避免 RTC 初值随机的干扰） */
/* 下一个要创建的任务是否 windowed 入口（1=是）。
 * s3_task_tramp 用 callx0 调我们自己的 call0 任务，
 * 但库创建的 ppTask 是 windowed 的，必须用 callx8 + WOE 才能正确 entry/retw。*/
u32 g_next_task_windowed = 0;
u32 g_win_scratch[8] = {0};
/* §418：win_bridge.S 的 s3_win_call_wifi_main 用它跨 windowed 调用
 * 保存自己的帧基址（`call8` 会轮转寄存器，a 寄存器全不可靠）。 */
u32 g_win_bridge_sp = 0;
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
    /* §379：帧从 `[top-128, top-48)` 开始 —— 必须整体低于 sp-48（溢出槽区）。 */
    top = ((u32)(g_stack_pool + g_pool_off) & ~15u) - 128u;
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
    /* §372：帧自描述 —— 偏移 76 = 恢复这个帧之后的 sp。
     *
     * §420 ★（真 bug）：**这里必须是 `top`，不能是 `top + 128`。**
     *
     * 帧占 `[top, top+128)`，`c->sp = top`。而 Xtensa 的栈是**向低地址
     * 增长**的，帧是 `[a1, a1+size)`。原来写 `top+128` ⇒ 任务启动时
     * a1 = top+128 ⇒ 第一条 `entry a1,32` 就把帧写到 `[top+96, top+128)`，
     * 即**盖在自己的启动帧上**；栈再长一点就盖到偏移 76（`a1sl`）。
     * 于是 `RESTORE_FRAME` 读到的 a1sl 是 0，命中 sp 闸门
     * （`0x500018E0` 标记 `0xBAD4`、`pc` 停在 `port_task_switch` 的
     * `j .` 死循环）—— 真板实测：`ctx[0]` 的帧里 EPC 槽被覆盖成 1、
     * WS 槽被覆盖成栈地址 `3fcbee30`、`a1sl` 变成 0。
     *
     * 写 `top` ⇒ 任务从 a1 = top 起跑、往下用栈，启动帧 `[top, top+128)`
     * 落在**栈顶之上的保留区**里，永远不会被自己的栈覆盖。 */
    f[19] = top;
    f[1] = (u32)entry;                 /* a2 */
    f[3] = g_next_task_windowed;       /* a4：入口的 ABI 标志 */
    g_next_task_windowed = 0;
    f[2] = (u32)arg;                   /* a3 */
    f[15] = (u32)s3_task_tramp;        /* EPC */
    /* PS：清掉中断级别位，并**置 WOE（bit18）**。闭源库是 windowed ABI，
     * 它的 call/retw 依赖窗口溢出/下溢向量来处理寄存器窗口轮转；
     * WOE=0 时这些向量不生效，第一次窗口轮转就会取到错位的返回地址
     * （真板表现：a0 带着 CALLINC 标记被当指令地址取、cause=0x14）。 */
    /* §345：初始 PS 里**清掉 OWB(bit8-11)**，让新任务一律从窗口 0 起跑。
     * OWB 决定 `rfe` 返回后的 WINDOWBASE，继承别人的 OWB 会让新任务
     * （闭源库的 ppTask）落到一个非法/别人的窗口基址上。
     * 清 0 之后 RESTORE_FRAME 会写 WS=1、rfe 装回 OWB=0 ⇒ 干净起步。 */
    f[16] = (port_ps_get() & ~0x1Fu & ~0xF00u) | 0x40000u;
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
#ifndef S3_ISR_SAVE_CTX
            /* §376：**中断帧不能当成任务的保存点**（默认行为已改）。
             *
             * 中断帧和 port_task_switch 的帧布局不同：它的 PS 槽是"被打断者
             * 的 PS 去掉 INTLEVEL"，偏移 68/72 存的是被打断那一刻的
             * WINDOWSTART/WINDOWBASE；而 `RESTORE_FRAME` 期望的是"这个任务
             * 自己的、可以原样写回硬件的窗口状态"。两者混用会让任务恢复后的
             * 窗口基址/起始与它的寄存器内容不配套 —— 真板症状正是
             * `windowbase=9`（奇数、非法）、`windowstart=0x200`、`a1` 变成
             * 垃圾（§375 现场）。
             *
             * 只有 `-DS3_ISR_SAVE_CTX` 才沿用旧行为（配合抢占实验）。 */
            (void)frame_sp;
#else
            c->sp = frame_sp;
#endif
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
