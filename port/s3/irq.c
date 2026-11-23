#include "c6os/types.h"
#include "c6os/tick.h"
#include "c6os/port.h"
#include "memmap.h"

extern void os_sched_tick(void);
extern void s3_wdt_kick(void);
extern void s3_mark(u32 idx, u32 v);
extern void *port_cur_ctx(void);
extern void port_set_cur_ctx(void *ctx);
extern void port_irq_fill(u32 frame_sp);
extern void *os_sched_preempt_ctx(void);

static void (*g_hook)(void);
static u32 g_isr_cc;

u32 s3_irq_cc(void)
{
    return g_isr_cc;
}

u32 s3_ccount_now(void)
{
    u32 cc = 0;

    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    return cc;
}

void s3_irq_set_hook(void (*fn)(void))
{
    g_hook = fn;
}

#define S3_IRQ_HZ 100u
#define S3_XTAL_HZ 40000000u

static u32 g_jlast;
static u32 g_jmin = 0xFFFFFFFFu;
static u32 g_jmax;
static u32 g_jsum;
static u32 g_jn;
static u32 g_jhist[16];

static void jitter_sample(u32 cc)
{
    if (g_jlast != 0u) {
        u32 d = cc - g_jlast;
        u32 dev = (d > 400000u) ? (d - 400000u) : (400000u - d);
        u32 b = dev / 2000u;

        if (b > 15u) {
            b = 15u;
        }
        g_jhist[b] = g_jhist[b] + 1u;
        if (d < g_jmin) {
            g_jmin = d;
        }
        if (d > g_jmax) {
            g_jmax = d;
        }
        g_jsum = g_jsum + d;
        g_jn = g_jn + 1u;
    }
    g_jlast = cc;
}

u32 os_jitter_min(void) { return g_jmin; }
u32 os_jitter_max(void) { return g_jmax; }
u32 os_jitter_count(void) { return g_jn; }
u32 os_jitter_avg(void) {
    return g_jn != 0u ? g_jsum / g_jn : 0u;
}
u32 os_jitter_p99(void)
{
    u32 i = 0;
    u32 acc = 0;
    u32 need = (g_jn * 99u) / 100u;

    while (i < 16u) {
        acc = acc + g_jhist[i];
        if (acc >= need) {
            return i * 2000u;
        }
        i++;
    }
    return 15u * 2000u;
}

volatile u32 g_sw_try;
volatile u32 g_sw_done;

static void s3_timer_rearm(void)
{
    u32 cc = 0;

    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    cc = cc + (S3_XTAL_HZ / S3_IRQ_HZ);
    /* §1079：写 CCOMPARE 之后必须 rsync，否则比较值可能不生效（真板实测
     * ccount 已越过 ccompare0 却没有触发定时器中断）。 */
    __asm__ __volatile__("wsr %0, ccompare0; rsync" : : "r"(cc));
}

void s3_irq_init(void)
{
    u32 mask = (1u << 6);   /* XCHAL_TIMER0_INTERRUPT = 6, level 1 */
    u32 old = 0;
    s3_timer_rearm();
    __asm__ __volatile__("rsr %0, intenable" : "=r"(old));
    /* ★★★★ §1090：**只保留定时器中断，清掉 WiFi 的 bit0/bit1**。
     *
     * §1089 查明：库的 `_set_isr` 只记录函数指针、从不装进硬件向量表，
     * 而 `intenable` 在 ROM 阶段就已经带着 `0x42`（bit1 + bit6）。
     * 我们的 `__xt_vectors` 接管向量表后，`_vec_lvl2..6` →
     * `_vec_lvl_common` 是"闪背光 N 下后 `j 4b` 停住"的调试处理器 ——
     * 所以只要 WiFi 的那个中断一触发，整个系统就被故意停在那里
     * （真板 halt 读到 `PS=0x60225` = INTLEVEL=5，正是 `_vec_lvl5`）。
     *
     * 既然库的 ISR 现在根本进不去，那这些中断开着只会打死系统。
     * 先全部关掉；等真正把 `_set_isr` 接进向量表之后再逐个打开。 */
    old = old & ~3u;
    __asm__ __volatile__("wsr %0, intenable; rsync" : : "r"(old | mask));
#ifndef S3_IRQ_HOLD_OFF
    /* 排查用开关：定义 S3_IRQ_HOLD_OFF 时先不开中断（验证非中断路径） */
    __asm__ __volatile__("rsil a2, 0" : : : "a2");
#endif
}

static void trap_hex(u32 v)
{
    static const char h[] = "0123456789abcdef";
    int i = 28;

    while (i >= 0) {
        port_putc((int)h[(v >> i) & 0xFu]);
        i = i - 4;
    }
}

/* WiFi 闭源库通过我们的 osi_funcs 表回调；表里每个函数的调用次数记在
 * g_osi_calls[]。崩溃时在串口把它打出来，就知道库走到哪一步了。
 * 用 weak 引用，非 WiFi 构建时地址为 0，自动跳过。 */
extern volatile unsigned int g_osi_calls[128] __attribute__((weak));

void s3_trap(u32 frame_sp)
{
    u32 *f = (u32 *)frame_sp;
    u32 cause = 0;
    u32 pc = 0;
    u32 ps = 0;

    __asm__ __volatile__("rsr %0, exccause" : "=r"(cause));
    /* 直接读 EPC1（异常时它保存出错 PC），比从帧里取更可靠 */
    __asm__ __volatile__("rsr %0, epc1" : "=r"(pc));
    __asm__ __volatile__("rsr %0, ps" : "=r"(ps));
    (void)f;
    s3_mark(7u, 0xDEAD0000u | (cause & 0xFFFFu));
    /* §308：**打印之前**先把第一次异常的现场落进 RTC 0x50000140。
     * 因为我们的打印路径（port_puts / trap_hex）自己在 EXCM=1 的上下文里
     * 也可能触发窗口异常，导致"看到的 pc=0x40048c6e"其实是打印过程里
     * 二次异常的现场（§306 实测 PS=0x00060f10，INTLEVEL=15、EXCM=1）。 */
    {
        volatile u32 *t = (volatile u32 *)0x50000140u;
        u32 va = 0;
        __asm__ __volatile__("rsr %0, excvaddr" : "=r"(va));
        /* §472 ★★ 只写一次 —— 否则记下的仍然是二次异常的现场。
         *
         * 上面 §308 描述的现象，这轮把机制确认了：
         * 崩在 `0x40048c6e`，那条指令是 **`retw.n`**，而当时
         * `ps = 0x00060010` ⇒ **EXCM=1**。`retw` 在 EXCM=1 时是非法指令
         * （EXCCAUSE 0），于是打印路径（`port_puts`/`trap_hex` 都走
         * windowed 函数、结尾 `retw`）自己再炸一次，把这里刚写好的
         * **第一次**现场覆盖成二次异常的现场 —— 等于白记。
         *
         * 加 `t[11]` 魔数做闸门：只有第一次进来才写。
         * 对应地 `port/s3/start.S` 在启动时清掉这个魔数
         * （RTC 内存复位后不清零，不清就会一直挡着新记录）。 */
        if (t[11] != 0x54524150u) {             /* "TRAP" */
            t[0] = cause;
            t[1] = va;
            t[2] = pc;
            t[3] = ps;
            t[4] = frame_sp;
            t[5] = f[0];
            t[6] = f[1];
            t[7] = f[15];
            t[8] = f[16];
            t[9] = f[17];
            t[10] = f[18];
            /* §485：把**故障点的通用寄存器**也记下来。
             * 帧布局（irq.S `12:` 段）：0=a0 4=a2 8=a3 12=a4 16=a5 20=a6
             * 24=a7 28=a8 32=a9 36=a10 40=a11 44=a12 48=a13 52=a14 56=a15。
             * §484 的现场是 `0x42057083 l32i a6,a6,0x154` 且 EXCVADDR=0x154，
             * 唯一可疑的是 a3（= &g_phyFuns）到底有没有被换掉 —— 只有把 a3/a6
             * 都记下来才能分清"a3 被窗口机制搞丢"还是"g_phyFuns 真是 0"。 */
            t[12] = f[2];       /* a3 */
            t[13] = f[5];       /* a6 */
            t[14] = f[4];       /* a5 */
            t[15] = f[9];       /* a10 */
            t[11] = 0x54524150u;
            /* §486：再把**整份 16 个通用寄存器**落到 0x500001A0（0x40 字节）。
             * 只看 a0/a3/a6/a10 几个已经不够了 —— 要判"到底哪个寄存器成了
             * 跳转目标"必须全看。这段区域没人用（start.S 只清 0x50000200 起）。 */
            {
                volatile u32 *r = (volatile u32 *)0x500001A0u;
                u32 i = 0;

                while (i < 16u) { r[i] = f[i]; i++; }
            }
        }
    }

    port_puts("*** TRAP sp=");
    trap_hex(frame_sp);
    port_puts(" cause=");
    trap_hex(cause);
    port_puts(" pc=");
    trap_hex(pc);
    port_puts(" ps=");
    trap_hex(ps);
    {
        u32 va = 0;
        __asm__ __volatile__("rsr %0, excvaddr" : "=r"(va));
        port_puts(" vaddr=");
        trap_hex(va);
        /* 注意：这里原来是 " ws="，但 (ps>>16)&0xF 打的是 **PS.CALLINC**，
         * 不是 WINDOWSTART（而且 rsr ps 拿到的是异常处理器的 PS，EXCM=1、
         * CALLINC 恒为 0），之前把这个 0 当成"WINDOWSTART 被清零"误判过。
         * 真正要看窗口状态请用 JTAG 读 WINDOWSTART。 */
        port_puts(" callinc=");
        trap_hex((ps >> 16) & 0x3u);
        port_puts(" a0=");
        trap_hex(((u32 *)frame_sp)[0]);
    }
    if (&g_osi_calls[0] != 0) {
        u32 i = 0;
        port_puts("\n  osi:");
        while (i < 128u) {
            if (g_osi_calls[i] != 0u) {
                port_puts(" "); trap_hex(i); port_puts("="); trap_hex(g_osi_calls[i]);
            }
            i++;
        }
    }
    port_puts("\n");
    /* 串口不可靠时用 LCD 背光(GPIO13)把异常码闪出来：
     *   先长亮 1 下（表示"进陷阱了"），然后慢慢闪 (cause + 1) 下。 */
    {
        u32 i = 0;
        u32 n = (cause & 0xFu) + 1u;

        *(volatile u32 *)(0x60009000u + 0x04u + 4u * 13u) =
            (*(volatile u32 *)(0x60009000u + 0x04u + 4u * 13u) & ~(7u << 12)) | (1u << 12);
        *(volatile u32 *)(0x60004000u + 0x554u + 4u * 13u) = 0x100u;
        *(volatile u32 *)(0x60004000u + 0x24u) = 1u << 13;

        *(volatile u32 *)(0x60004000u + 0x08u) = 1u << 13;      /* 长亮 */
        for (i = 0; i < 4000000u; i++) { }
        *(volatile u32 *)(0x60004000u + 0x0Cu) = 1u << 13;
        for (i = 0; i < 2000000u; i++) { }

        while (1) {
            i = 0;
            while (i < n) {
                *(volatile u32 *)(0x60004000u + 0x08u) = 1u << 13;
                { u32 k = 0; while (k < 1500000u) { k++; } }
                *(volatile u32 *)(0x60004000u + 0x0Cu) = 1u << 13;
                { u32 k = 0; while (k < 1500000u) { k++; } }
                i++;
            }
            { u32 k = 0; while (k < 8000000u) { k++; } }
        }
    }
    port_panic_halt();
}

/* 诊断：ISR 分段计数（RTC 0x50000710 + 4*i，每 256 次写一次）。
 * 1=port_cur_ctx 非零 2=port_irq_fill 返回 3=port_cur_ctx 为零
 * 4=port_isr_enter 返回 5=os_sched_tick 返回 */
static void s3_isr_mark(u32 i)
{
    static u32 m[8];
    volatile u32 *r = (volatile u32 *)0x50000710u;

    if (i < 8u) {
        m[i] = m[i] + 1u;
        if ((m[i] & 0xFFu) == 0u) { r[i] = m[i]; }
    }
}

extern void os_wdt_start(u32 secs);
extern void os_wdt_feed(void);

extern volatile u32 g_s3_diag[12];   /* §699: 定义在 wifi_glue.c */

/* §705: WiFi level-1 中断在 ISR 里只置这个标志；真正的窗口化库 ISR 由
 * 任务上下文（s3_wifi_scan 轮询循环）调用，避免在任意 WINDOWBASE 上 call8。 */
volatile u32 g_wifi_isr_pending = 0u;
void *s3_timer_irq(u32 frame_sp)
{
    u32 cc = 0;
    g_s3_diag[9]++;
    /* §1076：实时钟推进与 T0 重装都放 flash 里（IRAM 只差几十字节）。 */
    /* ★★★ §1082：tick 尾部**完全内联**，不调用任何函数。
     *
     * §1081 真板实测：从 ISR 里 call8 到 windowed C 函数（s3_tick_isr_tail），
     * 在它 `entry` 上抛 cause=0 IllegalInstruction（EPC1 = 函数入口）。
     * 改用汇编非窗口函数也不行（C 侧仍生成 call8）。
     * ⇒ 唯一稳妥的写法就是在这里内联，彻底不产生任何调用/窗口轮转。
     *
     * 做两件事：递增 S3_TICK_PEND(0x50001580)；重装 TIMG0 T0 报警。 */
    {
        /* ★★★ §1082：tick 尾部只做一件事 —— 递增 S3_TICK_PEND。
         *
         * §1081 真板实测：从 ISR 里 call8 到 windowed C 函数会在它 `entry`
         * 上抛 cause=0 IllegalInstruction（EPC1 = 函数入口）⇒ 必须内联。
         *
         * 为什么不需要重装 TIMG0：真正触发本 ISR 的是 **ccompare0**
         * （§1080 实测 _vec_level1 / s3_timer_irq 都在反复命中），
         * 而 `s3_timer_rearm()` 每次都会把 ccompare0 重新推到 ccount+N，
         * 定时器自己就续上了。TIMG0 在这个链路上根本不参与。
         * 只留这一行 store，IRAM 才放得下。 */
        *(volatile u32 *)0x50001580u = *(volatile u32 *)0x50001580u + 1u;
    }
    /* §370：**自愈看门狗**。板子一旦"CPU 死转 + USB 从总线上消失"，就只能
     * 人工断电重插（这轮就吃了一次）。这里在定时器中断里挂上 RTC 看门狗：
     * 中断还在跑就每 10ms 喂一次；一旦真的卡死（中断停了/死循环），8 秒后
     * 整片复位，串口重新枚举 —— 而 RTC 慢存不清，死之前的信标还能读出来。 */
#ifdef S3_AUTO_WDT
    {
        static u32 wdt_armed;
        if (wdt_armed == 0u) { wdt_armed = 1u; os_wdt_start(8u); }
        os_wdt_feed();
    }
#endif

    /* §633: 记录 Xtensa INTERRUPT SR —— 哪个 CPU 中断在挂起（WiFi=bit?）*/
    {
        u32 ipend = 0;
        volatile u32 *pd = (volatile u32 *)0x50000A00u;
        __asm__ __volatile__("rsr %0, interrupt" : "=r"(ipend));
        pd[0] = ipend;
        pd[1] = pd[1] + 1u;
        /* §642: 把 WiFi MAC/PWR 的 level-1 中断分派给库注册的 ISR。
         * 库用 src=0/2 → intr=0，ints_on(mask=1)，所以看 bit0。
         * 只在 MAC 事件位有效时调，避免定时器中断误入。 */
        if (ipend & 1u) {
            u32 ie = 0;
            g_s3_diag[10]++;   /* §699: WiFi 中断挂起次数 */
            /* §705: 不能在 call0 的 ISR 里直接 call8 进 windowed 库 ISR ——
             * 中断可能在任意 WINDOWBASE 上打断，call8 旋转窗口后 retw 与
             * 调用者的 CALLINC 不配套，真板实测 a0 被写成垃圾、掉进
             * _vec_default。只在这里置位，由任务上下文（s3_wifi_scan 轮询
             * 循环）调用 s3_wifi_isr_dispatch()。
             * §708: 但 MAC 中断是电平触发的 —— 不 ack 就会在 rte 之后立刻
             * 再次进入，真板实测 ~42M 次 ISR、任务永远拿不到 CPU（epc1 卡在
             * port_idle）。这里先屏蔽 intenable 的 WiFi 位止住风暴；等任务
             * 上下文跑完 wDev_ProcessFiq（它会清 MAC/PWR 事件寄存器）后再
             * 由 s3_wifi_scan 重新使能。 */
            __asm__ __volatile__("rsr %0, intenable" : "=r"(ie));
            ie &= ~1u;
            __asm__ __volatile__("wsr %0, intenable; rsync" : : "r"(ie));
            g_wifi_isr_pending = 1u;
            pd[2] = pd[2] + 1u;
        }
    }
    /* 记下被打断的主流程 ccount（陷阱帧偏移 60 = epc1）：卡死时读 mark[3]
     * 就知道主流程停在哪条指令上。§715: PC 采样块已删（腾 IRAM 给 PWR 诊断）。 */
    __asm__ __volatile__("rsr %0, ccount" : "=r"(cc));
    g_isr_cc = cc;
    /* §708: 删掉旧的"WINDOWBASE 首违例记录"块（§344）——它早已完成使命，
     * 这里腾出的 IRAM 给中断屏蔽/恢复用。 */
    jitter_sample(cc);
    s3_timer_rearm();
    s3_wdt_kick();
#ifdef S3_TICK_DEFER
    /* §964：tick 只在这里计数 —— 纯内存 +1，**不做任何窗口轮转**。
     * 真正的 os_sched_tick() 交给调度主循环在任务上下文里调
     * （port_virtual_tick()，见 console.c）。
     * 原因（真板取证）：ISR 里 s3_timer_irq→s3_win_tramp→os_sched_tick
     * 是这条链上的第 3~4 次窗口旋转，到 os_sched_tick 的
     * `entry a1, 144` 时真板抛 cause=0 IllegalInstruction，
     * EPC1 正好等于 os_sched_tick 入口，PS=0x00060031（EXCM=1、WOE=0）。 */
    {
        volatile u32 *tp = (volatile u32 *)0x50001580u;

        *tp = *tp + 1u;
    }
#endif
    {
        static u32 isr_n;
        volatile u32 *ic = (volatile u32 *)0x50000700u;
        isr_n = isr_n + 1u;
        /* 诊断：每 256 次中断把计数写进 RTC，确认定时器中断在不在跑 */
        if ((isr_n & 0xFFu) == 0u) {
            ic[0] = isr_n;
            ic[1] = ic[1] + 1u;
        }
    }
#ifndef S3_MINIMAL_ISR
    if (port_cur_ctx() != 0) {
        s3_isr_mark(1u);
        port_irq_fill(frame_sp);
        s3_isr_mark(2u);
    } else {
        s3_isr_mark(3u);
    }
#else
    (void)frame_sp;
#endif
    port_isr_enter();
    s3_isr_mark(4u);
    {
        /* 诊断：记录进入 os_sched_tick 的次数（RTC 0x50000708），
         * 与 0x50000700（INR 计数）对比，判断 tick 推进是否被跳过。 */
        static u32 st_n;
        volatile u32 *ic = (volatile u32 *)0x50000708u;
        st_n = st_n + 1u;
        if ((st_n & 0xFFu) == 0u) { ic[0] = st_n; }
    }
#ifndef S3_TICK_DEFER
#ifdef S3_ONE_ABI
    /* §891 全 windowed ABI：本 ISR 在 IRAM(0x40379xxx)，os_sched_tick 在
     * flash(0x420189a4)，GCC 的 -mlongcalls 生成 `l32r a8; callx8 a8`。
     * 真板证据：epc1=os_sched_tick 入口，a1=0，excvaddr=0 —— 进 os_sched_tick
     * 时窗口没有轮转（callx8 只置 CALLINC、不推进 WINDOWSTART），
     * 于是 `entry a1,144` 拿到错的 a1。
     * 和任务入口一样，改用近处(0x40379858，call8 范围内)的 s3_win_tramp：
     * 它先由硬件 call8 轮转好窗口，再 callx8 远端真正入口。 */
    __asm__ __volatile__(
        "mov    a12, %0\n\t"
        "call8  s3_win_tramp\n\t"
        :
        : "r"(os_sched_tick)
        : "a12", "a0", "memory");
#else
    os_sched_tick();
#endif
#endif /* S3_TICK_DEFER */
    s3_isr_mark(5u);
    if (g_hook != 0) {
        g_hook();
    }
    port_isr_exit();
#ifdef S3_NO_PREEMPT
    /* 关掉"定时器抢占切换"（§235）。
     *
     * 原因：Xtensa 只有 4 个窗口，windowed 调用链在深度 ≤3 时祖先窗口还活在
     * 寄存器文件里；在中断里直接切走 = 让另一个任务去轮转窗口文件 ⇒ 回来时
     * 祖先窗口已被覆盖，`retw` 用错 a0（真板上表现为 cause=0/29、a0 垃圾、
     * a1 变成 0x3fcb95f2 这种值）。
     *
     * 关掉抢占后，所有切换都只发生在**显式阻塞点**（sem/queue pend、delay），
     * 而那些调用都经过 port/s3/w2c.S 的壳 —— 壳在进内核前会把祖先窗口
     * spill 到任务自己的栈上（WIN_SPILL），所以寄存器文件在切换那一刻是干净的。
     * 代价：任务是协作式的（不会时间片轮转），我们的任务都会阻塞，够用。 */
    return 0;
#else
    {
        void *cur = port_cur_ctx();
        void *next = 0;

        if (cur == 0) {
            return 0;
        }
        next = os_sched_preempt_ctx();
        g_sw_try = g_sw_try + 1u;
        if (next == 0 || next == cur) {
            return 0;
        }
        g_sw_done = g_sw_done + 1u;
        port_set_cur_ctx(next);
        return next;
    }
#endif
}
