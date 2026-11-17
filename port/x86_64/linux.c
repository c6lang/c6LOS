#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

extern void x86_timer_entry(void);
extern void port_linux_timer_init(void);

/* x86-64 特权级底座：GDT（含 DPL3 用户段）+ TSS + 系统调用门。
 *
 * 选择子布局刻意与 Linux 一致，便于 syscall/sysret 的 STAR 语义：
 *   0x08 内核代码 DPL0      0x10 内核数据 DPL0
 *   0x18 用户 32 位代码 DPL3（占位，不用）
 *   0x20 用户数据 DPL3      0x28 用户 64 位代码 DPL3     0x30 TSS
 *   STAR[47:32]=0x08（内核段），STAR[63:48]=0x18（用户基）
 */

typedef struct {
    u32 rsv0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 rsv1;
    u64 ist[7];
    u64 rsv2;
    u16 rsv3;
    u16 iomap;
} __attribute__((packed)) X86Tss;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) X86Gdtr;

static u64 g_gdt[8];
static X86Gdtr g_gdtr;
static X86Tss g_tss;
static u8 g_kstack[16384] __attribute__((aligned(16)));

/* user_entry.S 用：syscall 入口换栈、以及用户栈的暂存。 */
u64 x86_kern_stack = 0;
u64 x86_user_rsp = 0;

extern void x86_int80_entry(void);
extern void x86_syscall_entry(void);
extern void x86_idt_set(int n, void (*fn)(void), u8 dpl);
extern void *x86_exc_table[32];

static void dbg_hex(u64 v)
{
    const char *d = "0123456789abcdef";
    int i;

    for (i = 15; i >= 0; i--) {
        port_putc((int)d[(u8)((v >> (u8)(4 * i)) & 0xfull)]);
    }
}

static void dbg_dec(u64 v)
{
    char b[24];
    int i = 0;

    if (v == 0ull) {
        port_putc('0');
        return;
    }
    while (v != 0ull && i < 24) {
        b[i] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
        i++;
    }
    while (i > 0) {
        i--;
        port_putc(b[i]);
    }
}

/* 异常统一出口：栈上是 LxRegs 之后紧跟 vec/err/rip/cs/rflags/rsp/ss。
 * 返回 1 表示"用户态异常已转成信号，帧已改好，直接 iretq 回用户态"；
 * 0 表示没救了（内核态异常，或者用户态连 handler 都没有）—— 调用方停机。 */
int x86_exc_dump(LxRegs *r, u64 cr2)
{
    u64 *p = (u64 *)r + 15ull;

    /* 先看是不是 COW 写缺页：ring3 直接写、内核写用户缓冲都会到这里。
     * 修好就原样 iretq 重试那条指令（不看 CPL：内核写用户页同样会缺页）。 */
    if (p[0] == 14ull && lx_cur_proc() != (LxProc *)0) {
        if (port_pf_resolve(cr2, (u32)p[1]) != 0) {
            return 1;
        }
    }
    /* L6：用户态踩到 #PF/#GP/#UD/#DE/#AC 时先看能不能变成信号投递出去。 */
    if (lx_cur_proc() != (LxProc *)0 && (p[3] & 3ull) == 3ull) {
        if (lx_sig_deliver_exc(lx_cur_proc(), r, cr2, (u32)p[0]) != 0) {
            return 1;
        }
    }
    port_puts("\n!! CPU 异常 vec=");
    dbg_dec(p[0]);
    port_puts(" err=");
    dbg_hex(p[1]);
    port_puts("\n   cr2=");
    dbg_hex(cr2);
    port_puts(" cs=");
    dbg_hex(p[3]);
    port_puts(" rip=");
    dbg_hex(p[2]);
    port_puts(" rsp=");
    dbg_hex(p[5]);
    port_puts(" rflags=");
    dbg_hex(p[4]);
    port_puts("\n   rax=");
    dbg_hex(r->rax);
    port_puts(" rcx=");
    dbg_hex(r->rcx);
    port_puts(" rdx=");
    dbg_hex(r->rdx);
    port_puts(" rsi=");
    dbg_hex(r->rsi);
    port_puts(" rdi=");
    dbg_hex(r->rdi);
    port_puts("\n   rbx=");
    dbg_hex(r->rbx);
    port_puts(" rbp=");
    dbg_hex(r->rbp);
    port_puts(" r8=");
    dbg_hex(r->r8);
    port_puts(" r9=");
    dbg_hex(r->r9);
    port_puts("\n   r10=");
    dbg_hex(r->r10);
    port_puts(" r11=");
    dbg_hex(r->r11);
    port_puts(" r12=");
    dbg_hex(r->r12);
    port_puts(" r13=");
    dbg_hex(r->r13);
    port_puts("\n   r14=");
    dbg_hex(r->r14);
    port_puts(" r15=");
    dbg_hex(r->r15);
    port_puts("\n   栈:");
    {
        u64 i;
        for (i = 0; i < 10ull; i++) {
            port_puts(" ");
            dbg_hex(*(u64 *)(p[5] + i * 8ull));
        }
    }
    port_puts("\n");
    port_panic_halt();
    return 0;
}

static u64 tss_desc(u64 base, u32 limit)
{
    u64 d = 0ull;

    d |= (u64)(limit & 0xffffu);
    d |= ((base & 0xffffffull) << 16);
    d |= (0x89ull << 40);                                 /* 64 位可用 TSS */
    d |= ((u64)((limit >> 16) & 0xfu) << 48);
    d |= (((base >> 24) & 0xffull) << 56);
    return d;
}

static void wrmsr(u32 msr, u32 lo, u32 hi)
{
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static void rdmsr32(u32 msr, u32 *lo, u32 *hi)
{
    __asm__ __volatile__("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

/* int 0x80 与 syscall 两条通道都汇到这里；语义在 linux/sys.c。 */
void x86_syscall_dispatch(LxRegs *r)
{
    lx_syscall(r);
}

void port_linux_arch_init(void)
{
    u64 base;
    u32 limit;

    /* ---- GDT ---- */
    g_gdt[0] = 0ull;
    g_gdt[1] = 0x00af9a000000ffffull;    /* 0x08 内核代码 */
    g_gdt[2] = 0x00af92000000ffffull;    /* 0x10 内核数据 */
    g_gdt[3] = 0x00cffa000000ffffull;    /* 0x18 用户 32 位代码（占位） */
    g_gdt[4] = 0x00cff2000000ffffull;    /* 0x20 用户数据 DPL3 */
    g_gdt[5] = 0x00affa000000ffffull;    /* 0x28 用户 64 位代码 DPL3 */

    base = (u64)&g_tss;
    limit = (u32)sizeof(X86Tss) - 1u;
    g_tss.rsp0 = (u64)(&g_kstack[0]) + (u64)sizeof(g_kstack);
    g_tss.iomap = (u16)sizeof(X86Tss);
    g_gdt[6] = tss_desc(base, limit);
    g_gdt[7] = (base >> 32) & 0xffffffffull;   /* 64 位描述符 16 字节，高半必须在
                                                * GDT 限长内，否则 ltr 直接 #GP */

    g_gdtr.limit = (u16)(sizeof(g_gdt) - 1u);
    g_gdtr.base = (u64)&g_gdt[0];
    __asm__ __volatile__("lgdt %0" : : "m"(g_gdtr));
    __asm__ __volatile__("ltr %0" : : "r"((u16)0x30u));

    /* ---- 系统调用门：int 0x80（兼容/调试）与 syscall（Linux 主通道）---- */
    {
        int i;

        for (i = 0; i < 32; i++) {
            x86_idt_set(i, (void (*)(void))x86_exc_table[i], 0u);
        }
    }
    x86_idt_set(0x80, x86_int80_entry, 3u);

    x86_kern_stack = (u64)(&g_kstack[0]) + (u64)sizeof(g_kstack);
    {
        u32 lo = 0u;
        u32 hi = 0u;
        u64 ent = (u64)&x86_syscall_entry;

        rdmsr32(0xc0000080u, &lo, &hi);
        lo |= 1u;                                  /* EFER.SCE */
        wrmsr(0xc0000080u, lo, hi);

        wrmsr(0xc0000081u, 0x00080000u, 0x00180000u);   /* STAR */
        wrmsr(0xc0000082u, (u32)(ent & 0xffffffffull), (u32)(ent >> 32));  /* LSTAR */
        wrmsr(0xc0000084u, 0x600u, 0u);                 /* SFMASK: 清 IF|DF */
    }
    port_linux_timer_init();                            /* L8：抢占的定时源 */
}

static void outb_(u16 port, u8 v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* L8 抢占：PIC 重映射到 0x20，只开 IRQ0；PIT 约 200Hz。
 * 中断门是 0x8E（进内核自动清 IF），所以内核里永远不会被打断 —— 抢占只发生在
 * ring3，这正是 IRQ_USER 的语义。 */
void port_linux_timer_init(void)
{
    u32 div = 1193182u / 200u;

    outb_(0x20u, 0x11u);               /* ICW1：需要 ICW4 */
    outb_(0xA0u, 0x11u);
    outb_(0x21u, 0x20u);               /* ICW2：主片 → 0x20 */
    outb_(0xA1u, 0x28u);               /*       从片 → 0x28 */
    outb_(0x21u, 0x04u);               /* ICW3：从片挂在主片 IRQ2 */
    outb_(0xA1u, 0x02u);
    outb_(0x21u, 0x01u);               /* ICW4：8086 模式 */
    outb_(0xA1u, 0x01u);
    outb_(0x21u, 0xFEu);               /* 只放开 IRQ0（定时器） */
    outb_(0xA1u, 0xFFu);

    outb_(0x43u, 0x36u);               /* 通道 0，先低后高，模式 3 */
    outb_(0x40u, (u8)(div & 0xFFu));
    outb_(0x40u, (u8)((div >> 8) & 0xFFu));

    x86_idt_set(0x20, x86_timer_entry, 0u);
}

/* 定时器中断：先 EOI（否则再也收不到 tick），再给调度器一次机会。
 * 这里可能切到别的进程；切回来以后继续返回，rax..r15 原样弹回用户态。 */
static u32 g_dbg_ticks;

void x86_timer_dispatch(LxRegs *r)
{
    (void)r;
    outb_(0x20u, 0x20u);
    lx_sched_preempt();
}

/* L5：切进程时同时换 syscall 入口用的内核栈和 TSS.RSP0（int 0x80 用后者）。 */
void port_user_set_kstack(u64 top)
{
    x86_kern_stack = top;
    g_tss.rsp0 = top;
}

void port_user_set_fsbase(u64 va)
{
    wrmsr(0xc0000100u, (u32)(va & 0xffffffffull), (u32)(va >> 32));
}

u64 port_user_get_fsbase(void)
{
    u32 lo = 0u;
    u32 hi = 0u;

    rdmsr32(0xc0000100u, &lo, &hi);
    return ((u64)hi << 32) | (u64)lo;
}
