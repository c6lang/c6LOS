#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_vma.h"

/* VMA 表：AS 句柄 → VMA 链头。
 *
 * 注册表本身也是动态的（port_kcalloc 倍增），活跃 AS 有多少就多大；AS 在 port 层
 * 真正拆掉时通过 port_as_set_free_hook 回调过来，把链和条目一起还掉 —— 否则
 * PML4 的物理地址被下一个 AS 复用时，旧 VMA 会被算到别人头上（这是最阴的一类
 * bug，只在反复 fork 后偶发）。
 */

typedef struct {
    u64 as;
    LxVma *head;
} VmaEnt;

static VmaEnt *g_ent;
static u64 g_cap;
static u64 g_n;

static void vma_on_as_free(u64 as);

void lx_vma_init(void)
{
    if (g_ent != (VmaEnt *)0) {
        return;
    }
    g_ent = (VmaEnt *)port_kcalloc(8ull, (u64)sizeof(VmaEnt));
    if (g_ent != (VmaEnt *)0) {
        g_cap = 8ull;
    }
    g_n = 0ull;
    port_as_set_free_hook(vma_on_as_free);
}

static VmaEnt *ent_get(u64 as, int create)
{
    u64 i;

    for (i = 0ull; i < g_n; i++) {
        if (g_ent[i].as == as) {
            return &g_ent[i];
        }
    }
    if (create == 0) {
        return (VmaEnt *)0;
    }
    if (g_n == g_cap) {
        u64 ncap = (g_cap == 0ull) ? 8ull : g_cap * 2ull;
        VmaEnt *ne = (VmaEnt *)port_kcalloc(ncap, (u64)sizeof(VmaEnt));
        u64 k;

        if (ne == (VmaEnt *)0) {
            return (VmaEnt *)0;
        }
        for (k = 0ull; k < g_n; k++) {
            ne[k] = g_ent[k];
        }
        if (g_ent != (VmaEnt *)0) {
            port_kfree(g_ent);
        }
        g_ent = ne;
        g_cap = ncap;
    }
    g_ent[g_n].as = as;
    g_ent[g_n].head = (LxVma *)0;
    g_n++;
    return &g_ent[g_n - 1ull];
}

LxVma *lx_vma_find(u64 as, u64 va)
{
    VmaEnt *e = ent_get(as, 0);
    LxVma *c;

    if (e == (VmaEnt *)0) {
        return (LxVma *)0;
    }
    c = e->head;
    while (c != (LxVma *)0) {
        if (va >= c->start && va < c->end) {
            return c;
        }
        if (va < c->start) {
            break;                         /* 有序：再往后不可能命中 */
        }
        c = c->next;
    }
    return (LxVma *)0;
}

int lx_vma_add(u64 as, u64 start, u64 end, u32 prot, u32 flags, u64 off)
{
    VmaEnt *e = ent_get(as, 1);
    LxVma *prev = (LxVma *)0;
    LxVma *cur;
    LxVma *n;

    if (e == (VmaEnt *)0 || end <= start) {
        return -1;
    }
    cur = e->head;
    while (cur != (LxVma *)0 && cur->start < start) {
        prev = cur;
        cur = cur->next;
    }
    if (cur != (LxVma *)0 && cur->start < end) {
        return -1;                         /* 与后一段重叠 */
    }
    if (prev != (LxVma *)0 && prev->end > start) {
        return -1;                         /* 与前一段重叠 */
    }
    /* 相邻且属性相同的匿名段合并，避免 ld.so 的逐段 mmap 把链拉长 */
    if (prev != (LxVma *)0 && prev->end == start && prev->prot == prot &&
        prev->flags == flags && (flags & LX_VM_FILE) == 0u) {
        prev->end = end;
        return 0;
    }
    if (cur != (LxVma *)0 && cur->start == end && cur->prot == prot &&
        cur->flags == flags && (flags & LX_VM_FILE) == 0u) {
        cur->start = start;
        if (prev != (LxVma *)0) {          /* 合并后可能又和前一段相邻 */
            if (prev->end == cur->start && prev->prot == cur->prot &&
                prev->flags == cur->flags) {
                prev->end = cur->end;
                prev->next = cur->next;
                port_kfree(cur);
            }
        } else {
            e->head = cur;
        }
        return 0;
    }
    n = (LxVma *)port_kmalloc((u64)sizeof(LxVma));
    if (n == (LxVma *)0) {
        return -1;
    }
    n->start = start;
    n->end = end;
    n->prot = prot;
    n->flags = flags;
    n->file_off = off;
    n->next = cur;
    if (prev != (LxVma *)0) {
        prev->next = n;
    } else {
        e->head = n;
    }
    return 0;
}

void lx_vma_remove(u64 as, u64 start, u64 end)
{
    VmaEnt *e = ent_get(as, 0);
    LxVma *prev = (LxVma *)0;
    LxVma *cur;

    if (e == (VmaEnt *)0 || end <= start) {
        return;
    }
    cur = e->head;
    while (cur != (LxVma *)0) {
        LxVma *nx = cur->next;
        u64 s = cur->start;
        u64 en = cur->end;

        if (en <= start || s >= end) {
            prev = cur;
            cur = nx;
            continue;
        }
        if (s >= start && en <= end) {              /* 整段落在洞里：删 */
            if (prev != (LxVma *)0) {
                prev->next = nx;
            } else {
                e->head = nx;
            }
            port_kfree(cur);
        } else if (s < start && en > end) {         /* 中间挖洞：拆两段 */
            LxVma *tail = (LxVma *)port_kmalloc((u64)sizeof(LxVma));

            if (tail == (LxVma *)0) {
                prev = cur;                 /* 分配失败：保守保留整段 */
            } else {
                /* 必须先拷再改 cur：先改 cur->end 会把 end<start 的坏节点拷进
                 * tail，链上出现"倒挂区间"后所有查找/插入都会错乱（踩过）。 */
                *tail = *cur;
                tail->start = end;
                tail->file_off = cur->file_off + (end - s);
                tail->next = nx;
                cur->end = start;
                cur->next = tail;
                prev = tail;
            }
        } else if (s < start) {                     /* 砍尾部 */
            cur->end = start;
            prev = cur;
        } else {                                    /* 砍头部：去掉 [start,end) */
            cur->file_off += (end - s);
            cur->start = end;
            prev = cur;
        }
        cur = nx;
    }
}

int lx_vma_protect(u64 as, u64 start, u64 end, u32 prot)
{
    VmaEnt *e = ent_get(as, 0);
    LxVma *cur;
    int found = 0;

    if (e == (VmaEnt *)0 || end <= start) {
        return -1;
    }
    /* 先确认整段都被覆盖：Linux 对"保护未映射区间"返回 ENOMEM */
    cur = e->head;
    {
        u64 at = start;

        while (at < end && cur != (LxVma *)0) {
            if (at >= cur->start && at < cur->end) {
                at = cur->end;
            }
            cur = cur->next;
        }
        if (at < end) {
            return -1;
        }
    }
    /* 再按段拆：区间边界落在段中间就分裂 */
    cur = e->head;
    while (cur != (LxVma *)0) {
        LxVma *nx = cur->next;

        if (cur->end <= start || cur->start >= end) {
            cur = nx;
            continue;
        }
        found = 1;
        if (cur->start < start) {
            LxVma *tail = (LxVma *)port_kmalloc((u64)sizeof(LxVma));

            if (tail != (LxVma *)0) {
                *tail = *cur;
                tail->start = start;
                tail->prot = prot;
                tail->next = nx;
                cur->end = start;
                cur->next = tail;
                if (end < tail->end) {           /* 洞再切一刀 */
                    LxVma *r = (LxVma *)port_kmalloc((u64)sizeof(LxVma));

                    if (r != (LxVma *)0) {
                        *r = *tail;
                        r->start = end;
                        r->prot = tail->prot;
                        r->next = nx;
                        tail->prot = prot;
                        tail->end = end;
                        tail->next = r;
                    }
                }
                cur = tail->next;
            } else {
                cur = nx;
            }
        } else if (cur->end > end) {             /* 砍尾部 */
            LxVma *r = (LxVma *)port_kmalloc((u64)sizeof(LxVma));

            if (r != (LxVma *)0) {
                *r = *cur;
                r->start = end;
                r->next = nx;
                cur->end = end;
                cur->prot = prot;
                cur->next = r;
            }
            cur = nx;
        } else {                                 /* 整段在区间内 */
            cur->prot = prot;
            cur = nx;
        }
    }
    return (found != 0) ? 0 : -1;
}

void lx_vma_free_all(u64 as)
{
    VmaEnt *e = ent_get(as, 0);
    LxVma *c;
    u64 i;

    if (e == (VmaEnt *)0) {
        return;
    }
    c = e->head;
    while (c != (LxVma *)0) {
        LxVma *nx = c->next;

        port_kfree(c);
        c = nx;
    }
    /* 从注册表里摘掉，末尾条目补位 */
    for (i = 0ull; i < g_n; i++) {
        if (g_ent[i].as == as) {
            g_ent[i] = g_ent[g_n - 1ull];
            g_n--;
            break;
        }
    }
}

static void vma_on_as_free(u64 as)
{
    lx_vma_free_all(as);
}
