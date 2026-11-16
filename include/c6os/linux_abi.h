#ifndef C6OS_LINUX_ABI_H
#define C6OS_LINUX_ABI_H

#include "c6os/types.h"
#include "c6os/config.h"
#include "c6os/port.h"

/* Linux ELF 二进制兼容层（personality）的内部契约。
 *
 * 注意：本层用 C 而不是 c6 编写，这是**刻意的**——它必须逐字节复现 Linux 的
 * C ABI 产物（ELF64 结构、auxv、stat/sigaction 布局）。用 c6 的 LP64 模型去
 * 描述 ILP32/LP64 混合的 ABI 表，正是 architecture.md §3 警告的那类风险。
 * 规则：linux/ 下的文件只有在 C6OS_LINUX_ABI=1 时才进入编译单元列表。 */

/* x86-64 Linux 系统调用号（只列已实现/即将实现的） */
#define LX_SYS_read            0
#define LX_SYS_write           1
#define LX_SYS_open            2
#define LX_SYS_close           3
#define LX_SYS_fstat           5
#define LX_SYS_lseek           8
#define LX_SYS_mmap            9
#define LX_SYS_mprotect       10
#define LX_SYS_munmap         11
#define LX_SYS_brk            12
#define LX_SYS_ioctl          16
#define LX_SYS_pread64        17
#define LX_SYS_exit           60
#define LX_SYS_getpid         39
#define LX_SYS_clone          56
#define LX_SYS_execve         59
#define LX_SYS_wait4          61
#define LX_SYS_uname          63
#define LX_SYS_getcwd         79
#define LX_SYS_chdir          80
#define LX_SYS_arch_prctl    158
#define LX_SYS_gettid        186
#define LX_SYS_futex         202
#define LX_SYS_set_tid_address   218
#define LX_SYS_exit_group    231
#define LX_SYS_openat        257
#define LX_SYS_readlinkat    267
#define LX_SYS_set_robust_list   273
#define LX_SYS_prlimit64     302
#define LX_SYS_getrandom     318
#define LX_SYS_rseq          334

/* arch_prctl 的 code */
#define LX_ARCH_SET_FS    0x1002u
#define LX_ARCH_GET_FS    0x1003u

/* errno（负值返回） */
#define LX_EPERM   1
#define LX_ENOENT  2
#define LX_EBADF   9
#define LX_EFAULT  14
#define LX_ENOMEM  12
#define LX_EINVAL  22
#define LX_ENOSYS  38

/* 与 port/x86_64/user_entry.S 的压栈顺序严格对应（低地址在前）。 */
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
} LxRegs;

/* 映像需要多少字节（max(p_offset+p_filesz)）；0 表示不是有效 ELF。
 * 内核据此决定从磁盘读多少扇区。 */
u64 lx_elf_image_size(u64 img_pa, u64 max_bytes);

/* 把映像里的 ELF64 加载进地址空间 as。img 是文件在物理内存里的基址。
 * 全部用 out 参数而不是结构体：调用方是 c6，避免跨语言的结构体布局假设。 */
int lx_elf_load(u64 as, u64 img_pa, u64 img_size,
                u64 *o_entry, u64 *o_phdr, u64 *o_phnum, u64 *o_phentsize,
                u64 *o_brk_end);

/* 建初始用户栈（argc/argv/envp/auxv），返回初始 RSP 与栈底。 */
int lx_build_stack(u64 as, u64 entry, u64 phdr, u64 phnum, u64 phentsize,
                   u64 *out_rsp, u64 *out_base);

/* 登记当前进程（系统调用层用）。 */
void lx_proc_init(u64 as, u64 brk_start);
u64  lx_proc_brk(void);

/* 系统调用分发（从汇编进来，可改写 r->rax 作为返回值）。 */
void lx_syscall(LxRegs *r);

/* 简易用户内存访问（L2：VMA 范围校验；L5 换 .extable + 缺页修复） */
int lx_user_ok(u64 va, u64 len, u32 prot);
u64 lx_copy_from_user(void *dst, u64 src, u64 len);

#endif
