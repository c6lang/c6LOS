#ifndef C6OS_LINUX_VMA_H
#define C6OS_LINUX_VMA_H

#include "c6os/types.h"

/* 地址空间描述（M13-2b）：每段"用户以为存在"的映射在 linux/ 层留一个 VMA。
 *
 * port 层只管页表：它知道"这个页可写"，不知道"这 8MB 是一个匿名映射、还没被
 * 碰过、可以等缺页再给页"。VMA 就是这层信息，负责：
 *   - 惰性 anon mmap（先只记账，第一次访问才分配页）
 *   - 栈向下增长（缺页落在栈 VMA 下方的增长窗口里 → 扩栈）
 *   - munmap/mprotect 的区间语义（拆段/挖洞/合并），以及 brk 收缩真还页
 *
 * 节点全部来自 port_kmalloc；AS 销毁时由 port 层回调触发整链释放，所以没有
 * 编译期条数上限。
 */

#define LX_VM_ANON    0x01u      /* 匿名映射（零页起步） */
#define LX_VM_FILE    0x02u      /* 文件映射（目前仍预拷内容） */
#define LX_VM_STACK   0x04u      /* 栈：允许向低地址增长 */
#define LX_VM_HEAP    0x08u      /* brk 区 */
#define LX_VM_SHARED  0x10u      /* MAP_SHARED */
#define LX_VM_LAZY    0x20u      /* 页尚未分配，缺页时补 */

#define LX_VM_STACK_GAP (8ull * 1024ull * 1024ull)   /* 栈最大向下增长窗口 */

typedef struct LxVma {
    u64 start;                   /* 含 */
    u64 end;                     /* 不含 */
    u32 prot;                    /* C6OS_PROT_* */
    u32 flags;                   /* LX_VM_* */
    u64 file_off;                /* 文件映射的文件偏移（字节） */
    struct LxVma *next;
} LxVma;

/* 注册 AS 释放回调（进程第一次 init 时调一次）。 */
void   lx_vma_init(void);
/* 找到包含 va 的 VMA；没有返回 0。 */
LxVma *lx_vma_find(u64 as, u64 va);
/* [start,end) 是否被 VMA 整段覆盖（mprotect/msync 用）。 */
int    lx_vma_covered(u64 as, u64 start, u64 end);
/* 缺页补页：惰性 anon 页 / 栈向下增长。返回 1 = 页已就位，原指令可重试；
 * 0 = 这不是"VMA 承诺过的地址"，交给信号/异常路径。 */
int    lx_vma_fault(u64 as, u64 va);
/* 内核 copy_*_user 前的补页：把区间内的惰性页补上。0 成功，-1 有页不在 VMA
 * 或权限不足。 */
int    lx_vma_fault_range(u64 as, u64 start, u64 end, u32 prot);
/* 登记 [start,end)：与已有区间重叠返回 -1；相邻且属性相同的匿名段自动合并。 */
int    lx_vma_add(u64 as, u64 start, u64 end, u32 prot, u32 flags, u64 off);
/* 删/拆区间（不碰页表；调用者负责 port_as_unmap）。 */
void   lx_vma_remove(u64 as, u64 start, u64 end);
/* 只改区间内的权限（必要时拆段）。返回 0；整段没有 VMA 覆盖返回 -1。 */
int    lx_vma_protect(u64 as, u64 start, u64 end, u32 prot);
/* 把 VMA 链格式化成 /proc/self/maps 文本；返回写入字节数。 */
u32    lx_vma_maps(u64 as, char *buf, u32 cap);
/* AS 销毁前调用：还掉整条链（回调里会调）。 */
void   lx_vma_free_all(u64 as);

#endif
