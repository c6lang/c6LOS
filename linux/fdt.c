#include <c6os/linux_abi.h>
#include <c6os/linux_proc.h>
#include <c6os/port.h>

/* 动态 fd 表（M13-3a）。
 *
 * 以前 fd 是 `LxFile fd[C6OS_LINUX_MAX_FD]` 内嵌在 PCB 里：16 个槽封顶，线程组
 * 还要靠"切换时把数组整份抄给组长"来假装共享 —— 两个毛病。现在改成对象：
 *
 *   LxFdTab { refs, cap, LxFile *ent }
 *
 * - 线程组共享**同一个** LxFdTab（clone 时 ref+1），不再抄数组；fork 才复制。
 * - cap 按需倍增（16 → …，硬上限 LX_FD_MAX_CAP），槽位数组从内核堆取，
 *   不占 .bss，也没有编译期条数上限。
 * - 表销毁时每个非空槽都要通知上层（管道要减 readers/writers），但这一层不
 *   认识管道：用 free hook 反向注册，保持 fdt 不依赖 proc。
 */

#define FDT_INIT_CAP 16u
#define FDT_MAX_CAP  LX_FD_MAX_CAP

static void (*g_free_hook)(LxFile *f);

void lx_fdtab_set_free_hook(void (*fn)(LxFile *f))
{
    g_free_hook = fn;
}

static void ent_reset_(LxFile *e, u32 n)
{
    u32 i;

    for (i = 0u; i < n; i++) {
        e[i].node = LX_FD_NONE;
        e[i].pos = 0ull;
        e[i].is_dir = 0u;
        e[i].kind = LX_FD_FILE;
    }
}

static LxFdTab *tab_new_(u32 cap)
{
    LxFdTab *t = (LxFdTab *)port_kmalloc((u64)sizeof(LxFdTab));

    if (t == (LxFdTab *)0) {
        return (LxFdTab *)0;
    }
    t->ent = (LxFile *)port_kcalloc((u64)cap, (u64)sizeof(LxFile));
    if (t->ent == (LxFile *)0) {
        port_kfree(t);
        return (LxFdTab *)0;
    }
    t->refs = 1u;
    t->cap = cap;
    ent_reset_(t->ent, cap);
    return t;
}

LxFdTab *lx_fdtab_new(void)
{
    LxFdTab *t = tab_new_(FDT_INIT_CAP);

    if (t == (LxFdTab *)0) {
        return (LxFdTab *)0;
    }
    /* 0/1/2 = 控制台，和 Linux 进程启动时一致 */
    t->ent[0].node = LX_FD_CONSOLE;
    t->ent[1].node = LX_FD_CONSOLE;
    t->ent[2].node = LX_FD_CONSOLE;
    return t;
}

LxFdTab *lx_fdtab_copy(const LxFdTab *src)
{
    LxFdTab *t;
    u32 i;

    if (src == (const LxFdTab *)0) {
        return lx_fdtab_new();
    }
    t = tab_new_(src->cap);
    if (t == (LxFdTab *)0) {
        return (LxFdTab *)0;
    }
    for (i = 0u; i < src->cap; i++) {
        t->ent[i] = src->ent[i];
    }
    return t;
}

void lx_fdtab_ref(LxFdTab *t)
{
    if (t != (LxFdTab *)0) {
        t->refs++;
    }
}

void lx_fdtab_unref(LxFdTab *t)
{
    u32 i;

    if (t == (LxFdTab *)0) {
        return;
    }
    if (t->refs > 1u) {
        t->refs--;
        return;
    }
    for (i = 0u; i < t->cap; i++) {
        if (t->ent[i].node != LX_FD_NONE &&
            g_free_hook != (void (*)(LxFile *))0) {
            g_free_hook(&t->ent[i]);
        }
        t->ent[i].node = LX_FD_NONE;
    }
    port_kfree(t->ent);
    port_kfree(t);
}

/* 保证至少 need 个槽：按 2 的幂倍增，旧内容原样搬过去。返回 0 成功。 */
int lx_fdtab_grow(LxFdTab *t, u64 need)
{
    u32 ncap;
    LxFile *ne;
    u32 i;

    if (t == (LxFdTab *)0) {
        return -1;
    }
    if (need <= (u64)t->cap) {
        return 0;
    }
    if (need > (u64)FDT_MAX_CAP) {
        return -1;
    }
    ncap = t->cap;
    while ((u64)ncap < need) {
        ncap *= 2u;
    }
    if (ncap > FDT_MAX_CAP) {
        ncap = FDT_MAX_CAP;
    }
    ne = (LxFile *)port_kcalloc((u64)ncap, (u64)sizeof(LxFile));
    if (ne == (LxFile *)0) {
        return -1;
    }
    for (i = 0u; i < t->cap; i++) {
        ne[i] = t->ent[i];
    }
    ent_reset_(ne + t->cap, ncap - t->cap);
    port_kfree(t->ent);
    t->ent = ne;
    t->cap = ncap;
    return 0;
}

/* 取一个空闲 fd（>= 3）。表满时先尝试倍增；到硬上限才报 EMFILE。 */
u64 lx_fdtab_alloc(LxFdTab *t)
{
    for (;;) {
        u32 i;

        if (t == (LxFdTab *)0) {
            return LX_FD_NONE;
        }
        for (i = 3u; i < t->cap; i++) {
            if (t->ent[i].node == LX_FD_NONE) {
                return (u64)i;
            }
        }
        if (lx_fdtab_grow(t, (u64)t->cap + 1ull) != 0) {
            return LX_FD_NONE;
        }
    }
}

LxFile *lx_fdtab_get(LxFdTab *t, u64 fd)
{
    if (t == (LxFdTab *)0 || fd >= (u64)t->cap) {
        return (LxFile *)0;
    }
    if (t->ent[fd].node == LX_FD_NONE) {
        return (LxFile *)0;
    }
    return &t->ent[fd];
}
