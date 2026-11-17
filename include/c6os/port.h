#ifndef C6OS_PORT_H
#define C6OS_PORT_H

#include "c6os/types.h"
#include "c6os/caps.h"

/* M0 最小端口面。后续里程碑按 docs/architecture.md §14 增补。 */

void port_core_bind_self(void);
u32 port_core_id(void);
void port_init(void);
void port_idle(void);
void port_panic_halt(void);
void port_reboot(void);
void port_mark(u32 v);

/* 任务上下文（协作 port）：ctx 对内核不透明。 */
void *port_task_ctx_create(void (*entry)(void *), void *arg, u32 stack_size);
void port_task_ctx_free(void *ctx);
void port_task_switch(void *ctx);
void port_enter_task(void);
void port_leave_task(void);
void port_task_yield(void);
u32 port_task_stack_ok(void *ctx);
u32 port_virtual_tick(void);
void port_diag_dump(void);
void port_snapshot(void);
void port_switch_trace(u32 tag, u32 id, u32 ctx);
u32 port_in_isr(void);
void port_isr_enter(void);
void port_isr_exit(void);
u32 port_atomic_cas(volatile u32 *p, u32 expect, u32 new);
u64 port_cycles(void);
u32 port_irq_save(void);
void port_irq_restore(u32 state);
void os_test_set_isr(u32 v);
void port_sim_preempt_start(void);
void *os_sched_preempt_ctx(void);

void port_putc(int ch);
int port_getc(void);
void port_puts(const char *s);
void port_delay_ms(u32 ms);

/* ---- 掉电域信标（RTC_CNTL_STORE4..7 = 0x600080C0..CC）------------------
 * CPU 复位/看门狗都不会清，esptool 可以直接 read-mem 读出来。
 * 串口不可靠时用它记录"执行到哪一步了"。
 *   例：s3_beacon(0, 0x1234);  ->  esptool read-mem 0x600080C0
 */
void s3_beacon(u32 slot, u32 val);

u32 s3_win_call_cache_enable(void);

u32 s3_win_call_cache_invalidate(void);

/* ---- 用户态 / 地址空间契约（personality 专用）---------------------------
 *
 * 只有 port_caps() 含 C6OS_CAP_LINUX_ABI（MMU|USER|IRQ_USER）的移植层才实现；
 * C6OS_LINUX_ABI=0 时没有任何内核代码引用它们，因此不参与链接、零体积影响。
 * 布局与陷阱帧细节见 docs/linux-abi.md §3/§4。
 *
 * 跨边界规则（architecture.md §3）在此继续生效：只用 u32/u64，不用 long/size_t，
 * 不按值传结构体。
 */

/* 地址空间句柄（port 私有；x86-64 = 页表根物理地址，0 表示失败）。 */
u64  port_as_create(void);
/* 强制销毁（只用于"这个 AS 还是私有的"失败路径；ref 应为 1）。 */
void port_as_destroy(u64 as);
/* 引用计数：线程组共享地址空间（CLONE_VM）时 +1；ref 到 0 才真正销毁。
 * 当前 CR3 上的 AS 会延后到 port_as_gc（切页表之后）再释放。 */
void port_as_ref(u64 as);
void port_as_unref(u64 as);
void port_as_gc(void);
void port_as_activate(u64 as);
/* 建立/拆除映射。paddr==0 表示分配新物理页（匿名映射）；同一 VA 覆盖映射会
 * 先释放旧页框（MAP_FIXED 语义）。 */
int  port_as_map(u64 as, u64 vaddr, u64 paddr, u64 bytes, u32 prot);
int  port_as_unmap(u64 as, u64 vaddr, u64 bytes);
/* 改已映射页的权限（mprotect）：W/NX 立刻写回 PTE，不重新分配页框。 */
int  port_as_protect(u64 as, u64 vaddr, u64 bytes, u32 prot);
/* fork 用：复制用户区间（写时复制可后续替换此实现）。返回 0 成功。 */
int  port_as_copy(u64 dst_as, u64 src_as);   /* fork：复制全部用户页 */
void port_user_set_kstack(u64 top);          /* L5：每进程内核栈 */
/* 用户指针校验（copy_from_user 的第一道闸）。返回 1 可访问。 */
int  port_as_user_range_ok(u64 as, u64 vaddr, u64 len, u32 prot);

/* 物理帧（4KB）。返回 0 表示失败。
 * alloc 得到 ref=1；ref/put 是引用计数；put 到 0 才回 free list。
 * port_phys_free 保留为 put 的别名（旧名字，别在新代码里用）。 */
u64  port_phys_alloc(void);
void port_phys_ref(u64 pa);
void port_phys_put(u64 pa);
void port_phys_free(u64 pa);
u64  port_phys_total_kb(void);
u64  port_phys_free_kb(void);

/* MMU / 物理帧初始化：必须先于任何 port_as_* 调用。 */
void port_mmu_init(void);
/* 物理帧的内核虚拟地址（直接映射窗口），用于把用户映像写进去。 */
void *port_phys_map(u64 pa);
/* 把一段物理内存从分配器里扣掉（内核要在固定物理地址放磁盘映像/帧缓冲）。 */
void port_phys_reserve(u64 pa, u64 bytes);
/* ATA PIO 读（L2 用它把 ELF 从磁盘搬进内存；L4 的 VFS 复用）。 */
/* ---- M10-2：e1000（qemu -nic user,model=e1000）---- */
int  port_net_init(void);
int  port_net_found(void);
u64  port_net_bar(void);
void port_net_mac(u8 *out);
int  port_net_link_up(void);
int  port_net_send(const void *frame, u32 len);
int  port_net_recv(void *frame, u32 max);
u32  port_net_rx_packets(void);
u32  port_net_tx_packets(void);
u32  port_net_rx_dropped(void);

int  port_ata_read(u32 lba, u32 count, u64 dst_pa);
int  port_ata_write(u32 lba, u32 count, u64 src_pa);

/* 特权级底座：GDT/TSS/IDT、syscall 通道、每个 CPU 的内核栈。 */
void port_linux_arch_init(void);/* 进入 ring3。不返回（返回路径由 syscall 的 iretq 负责）。 */
void port_user_enter(u64 as, u64 rip, u64 rsp, u64 arg);
/* 用户线程的 FS 基址（x86-64 TLS 走 FS）。 */
void port_user_set_fsbase(u64 va);
u64  port_user_get_fsbase(void);

/* port_as_map 的保护位 */
#define C6OS_PROT_R 0x1u
#define C6OS_PROT_W 0x2u
#define C6OS_PROT_X 0x4u

#endif
