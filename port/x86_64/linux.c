#include "c6os/types.h"
#include "c6os/port.h"

/* x86-64 特权级底座：GDT（含 DPL3 用户段）+ TSS + 系统调用门。
 *
 * 选择子布局刻意与 Linux 一致，便于后续接 syscall/sysret：
 *   0x08 内核代码 DPL0      0x10 内核数据 DPL0
 *   0x18 用户 32 位代码 DPL3（占位，不用）
 *   0x20 用户数据 DPL3      0x28 用户 64 位代码 DPL3     0x30 TSS
 * SYSRET 语义：CS = STAR[63:48]+16 | 3，SS = STAR[63:48]+8 | 3，
 * 因此内核侧要写 STAR[63:48] = 0x18（L2 接入时使用）。
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

/* 与 user_entry.S 的压栈顺序严格对应（低地址在前）。 */
typedef struct {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
} X86Regs;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) X86Gdtr;

static u64 g_gdt[8];
static X86Gdtr g_gdtr;
static X86Tss g_tss;
static u8 g_kstack[16384] __attribute__((aligned(16)));

extern void x86_int80_entry(void);
extern void x86_idt_set(int n, void (*fn)(void), u8 dpl);

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

static void put_dec(u64 v)
{
    char buf[24];
    int i = 0;

    if (v == 0ull) {
        port_putc('0');
        return;
    }
    while (v != 0ull && i < 24) {
        buf[i] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
        i++;
    }
    while (i > 0) {
        i--;
        port_putc(buf[i]);
    }
}

/* 系统调用分发：L1 只有 write / exit，L2 起接完整表。 */
void x86_syscall_dispatch(X86Regs *r)
{
    u64 nr = r->rax;

    if (nr == 1ull) {                    /* write(fd, buf, len) */
        const char *buf = (const char *)r->rsi;
        u64 n = r->rdx;
        u64 i;

        for (i = 0; i < n; i++) {
            port_putc((int)(u8)buf[i]);
        }
        r->rax = n;
        return;
    }
    if (nr == 60ull || nr == 231ull) {   /* exit / exit_group */
        port_puts("\n[ring3] exit(");
        put_dec(r->rdi);
        port_puts(")\n");
        port_puts("L1: ring3 通路已验证，停机。\n");
        port_panic_halt();
        return;
    }
    r->rax = (u64)-38ll;                 /* -ENOSYS */
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
    g_tss.iomap = (u16)sizeof(X86Tss);    /* 无 IO 位图 */
    g_gdt[6] = tss_desc(base, limit);
    g_gdt[7] = (base >> 32) & 0xffffffffull;   /* TSS 描述符高半部分：必须在 GDT 限长内，
                                                * 否则 ltr 直接 #GP（64 位描述符是 16 字节）。 */

    g_gdtr.limit = (u16)(sizeof(g_gdt) - 1u);
    g_gdtr.base = (u64)&g_gdt[0];
    __asm__ __volatile__("lgdt %0" : : "m"(g_gdtr));
    /* 现有 CS=0x08 / DS=0x10 与新表同值，段缓存仍有效，无需重载。 */
    __asm__ __volatile__("ltr %0" : : "r"((u16)0x30u));

    /* ---- 系统调用门：int 0x80，DPL3 ---- */
    x86_idt_set(0x80, x86_int80_entry, 3u);
}

void port_user_set_fsbase(u64 va)
{
    u32 lo = (u32)(va & 0xffffffffull);
    u32 hi = (u32)(va >> 32);

    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(0xc0000100u));
}

u64 port_user_get_fsbase(void)
{
    u32 lo;
    u32 hi;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000100u));
    return ((u64)hi << 32) | (u64)lo;
}
