# Linux ABI 平台无关化（多架构 personality）

> 状态：**设计稿，A1 起施工**。现状：L0–L8 已由 [linux-abi.md](linux-abi.md) 在
> `port/x86_64` 上验收，M13-1 已落地（`check-linuxabi-m13`）。
> 本文把 `linux/` 拆成「ABI 无关语义核心 `lx/`」+「每架构一份 `abi/<abi>/`」，
> 使 Linux ABI 能挂到任何具备 MMU+特权级的 64 位移植层上（第一个新目标是
> MaixCam 的 SG2002 `C906`，见 [maixcam-bringup.md](maixcam-bringup.md)）。
> 关联：[linux-abi.md](linux-abi.md)、[linux-abi-complete.md](linux-abi-complete.md)、
> [architecture.md](architecture.md) §1 修订、[todo.md](todo.md)

---

## 0. 结论先说

**现在这一层不能"移植"，只能"复制一份改掉"。** 原因不是 MMU 抽象没做好 ——
`port_as_*` / `port_phys_*` / `port_user_*` 这组接口已经足够跨架构；真正绑死
x86-64 的是**四类"字节级契约"**，它们散落在 `linux/` 里，和语义代码混在一起：

| # | 绑死的东西 | 位置（现状） | 换架构会怎样 |
|---|---|---|---|
| 1 | **用户寄存器帧**：15 个通用寄存器按 x86 压栈序命名 | `include/c6os/linux_abi.h:204-221`（`LxRegs`）、`include/c6os/linux_proc.h:37-42`（`LxFrame`） | `r->rax` / `r->rip` 全文件直接摸字段，RISC-V 上无处对应 |
| 2 | **系统调用号表**：x86-64 编号 | `include/c6os/linux_abi.h:30-190` 的 `LX_SYS_*` | RISC-V 用 asm-generic 编号，几乎全不同（`read` 0→63、`openat` 257→56、`mmap` 9→222…） |
| 3 | **ABI 数据结构布局**：`stat` 144B、`rt_sigframe` 1280B、greGs 偏移、auxv 平台串 | `linux/fs.c:607-633`、`linux/sig.c:11-46`、`linux/elf64.c:315` | RISC-V 用 asm-generic `stat`(128B)、自己的 `sigcontext`；写错就是 libc 静默错乱 |
| 4 | **地址空间布局常量**：47-bit 低半核 | `include/c6os/config.h:53-75` | Sv39 上限 `0x7FFFFFFFFF`，`STACK_TOP=0x7fffffffe000` 与 `MMAP_BASE=0x7ff000000000` **全部越界** |

外加两个"架构能力"层面的隐含假设：**内核也在页表里**（x86 的 CR3 对 ring0 生效，
所以内核必须被映射）和**指令缓存与数据一致**（x86 免费；C906 非一致，装载完 ELF
必须刷 I-cache）。这两条也不是"移植费"，而是必须在 port 契约里显式化。

所以本设计的做法是：**把上面 4 类 + 2 条能力抽成一张 `lx_abi` 表，语义核心
（进程/fd/VFS/信号策略/ELF 骨架/按需分页）一行 x86 字样都不留。** 换架构 =
新增一个 `abi/<abi>/`（一份表 + 一个 asm 文件）+ 一个 `port/<soc>/`。

### 0.1 对既有文档的修订

- [linux-abi-complete.md](linux-abi-complete.md) §8 原写"其他架构的 Linux personality：不做"。
  **本文修订这一条**：不做的是"多 ABI 同时在线"（x86-64 与 rv64 在同一内核里共存），
  **单一 ABI、按构建选择**是新目标。理由：MaixCam 的大核是 RV64，跑 x86-64 机器码
  在物理上不存在（除非写解释器，明确不做）。
- [linux-abi.md](linux-abi.md) §2.1「各 port 现状」的能力位判据不变，但**判据要加一维**：
  `caps=MMU|USER|IRQ_USER` 只说明"这个 port 能当宿主"，还说明不了"这个 port 能跑**这个** ABI"。
  见 §4 的二维门禁。

---

## 1. 分层：两根轴，三处不越界

```
                   ┌──────────────────────────────────────────┐
                   │ lx/           ABI 无关语义核心（.c）      │
                   │ syscall 实现 / proc / fd / VFS 桥 / 信号    │
                   │ 投递策略 / ELF 装载骨架 / 按需分页 / 等待   │
                   └───────────┬──────────────┬───────────────┘
                               │ lx_abi 表     │ OsFileOps / OsAs
                   ┌───────────┴───────┐  ┌───┴─────────────────────┐
                   │ abi/<abi>/        │  │ kernel/ (LiteOS 面)      │
                   │ 布局 + 号表 + 帧   │  │ mm / proc / fs / net / rt │
                   │ 访问器 + 入口 asm  │  └───┬─────────────────────┘
                   └───────────┬───────┘      │
                               │ port_arch    │
                   ┌───────────┴──────────────┴───────────────┐
                   │ port/<arch>+<soc>/  board/<board>/  dev/  │
                   └──────────────────────────────────────────┘
```

三条硬规则（评审用的，不是口号）：

1. **`lx/` 里不出现任何寄存器名或架构指令**（`rax`/`rip`/`rdi`/`cr3`/`int 0x80`/`pml4`/`satp`）。
   CI 用 `make check-layering` 的 grep 门禁卡住（§5.4）。
2. **`abi/` 里不出现策略**：不知道 VFS 怎么找文件、不知道进程怎么调度、不知道页框怎么分配。
   它只做两件事：**从帧里取参数 / 往帧里写结果**，**按本 ABI 的字节布局写结构体**。
3. **`port/` 里不出现 syscall 号和 Linux 结构体**。port 不知道"现在跑的是 Linux 程序"。

### 1.1 帧的归属：布局属架构，访问器属 ABI

这条容易做反，先钉死：

- **帧的内存布局属于架构**（`port/<arch>/frame.h`）：x86-64 是 15 个 GPR + iretq 尾；
  RISC-V 是 31 个 GPR + pc + f 寄存器区。它是"硬件压栈 + 汇编补压"的产物。
- **帧的语义属于 ABI**（`abi/<abi>/frame_ops.c`）：参数从哪几个寄存器取（x86-64: rdi,rsi,rdx,r10,r8,r9；
  rv64: a0–a5）、返回值写哪（rax / a0）、系统调用号在哪（rax / a7）、
  `fork` 子进程怎么"返回 0"、TLS 指针在哪（x86: MSR_FS_BASE / rv64: `tp`）。

好处：同一个 port 上换 ABI（x86-64 上将来跑 i386、rv64 上跑 ilp32）只是换一张表；
同一个 ABI 换 port 只需 port 提供同构的架构帧。

### 1.2 帧对核心不透明

```c
/* lx/lx_frame.h —— 核心只认这个壳：定长、可 memcpy、可清零。
 * 真实布局在 port/<arch>/frame.h，只有 abi/<abi>/ 被允许解释它。 */
#define LX_FRAME_MAX 512u          /* 由 abi 配置给出上界：x86-64 120B；rv64 +FP 区 ~400B */
typedef struct lx_frame { u8 b[LX_FRAME_MAX]; } lx_frame;
```

核心对帧只有 4 种操作：`lx_frame_zero()`、`lx_frame_copy()`、`abi->*()` 访问器、
`port_user_return(&f)`。**禁止**把 `lx_frame*` 强转成任何结构体指针（脚本门禁查这个）。
现有代码里所有 `r->rax` / `*(u64 *)r + 17` 这种写法（`linux/proc.c:250,500,538`、
`linux/sig.c:377,415`）全部消失。

---

## 2. `lx_abi` 契约（唯一需要每个架构写一份的东西）

```c
/* abi/<abi>/abi.h —— 每个 ABI 后端导出且只导出这一个对象 */
typedef i64 (*lx_sysfn)(struct lx_task *t, const u64 a[6]);

struct lx_sysent {
    u16          nr;          /* 本 ABI 的调用号 */
    const char  *name;        /* 只用于 -ENOSYS 日志 */
    lx_sysfn     fn;          /* 指向 lx/ 里的共享实现 */
};

typedef struct lx_abi {
    const char  *name;              /* "x86_64-linux" / "rv64-linux" */
    u32          id;               /* 写进帧头，交叉校验，防错配 */
    u16          elf_machine;      /* 62 = EM_X86_64 / 243 = EM_RISCV */
    u32          frame_size;       /* 本 ABI 需要的帧长（含 FP/向量扩展区） */
    u32          page_shift;       /* 4KB 页：12（两边同，仍显式声明） */

    /* ---- 用户地址布局（Sv39 与 x86-64 47-bit 的差别全在这里） ---- */
    u64          elf_base, interp_base, mmap_base, stack_top, mmap_min;
    u64          user_max_va;

    /* ---- 陷入帧访问器：核心碰用户寄存器的唯一途径 ---- */
    u64  (*nr)(const lx_frame *);
    u64  (*arg)(const lx_frame *, u32 i);
    void (*set_ret)(lx_frame *, i64 v);
    u64  (*pc)(const lx_frame *);   void (*set_pc)(lx_frame *, u64);
    u64  (*sp)(const lx_frame *);   void (*set_sp)(lx_frame *, u64);
    void (*set_arg0)(lx_frame *, u64);
    void (*set_tls)(lx_frame *, u64);   u64 (*get_tls)(const lx_frame *);
    void (*set_fork_child)(lx_frame *);     /* fork/clone 子进程"返回 0" */
    int  (*copy_ext)(lx_frame *d, const lx_frame *s);  /* FP/向量扩展状态 */
    void (*dump)(const lx_frame *);         /* panic / 异常打印 */

    /* ---- 异常 → 信号（编号与 si_code 是 ABI 的） ---- */
    int  (*exc_to_sig)(const lx_frame *, u32 trapno, u64 addr,
                       u32 *o_sig, u64 *o_siaddr);

    /* ---- ABI 数据结构布局：尺寸 + 写入器 ---- */
    u32  stat_size, statfs_size, sigaction_size;
    int  (*fill_stat)(u8 *dst, const struct lx_stat *);
    int  (*fill_statfs)(u8 *dst, const struct lx_statfs *);
    int  (*fill_sigaction)(u8 *dst, const struct lx_sigaction *);

    /* ---- 启动约定 ---- */
    int  (*build_auxv)(u8 *dst, u32 *n, const struct lx_exec *);  /* AT_PLATFORM/AT_HWCAP/AT_PHDR… */

    /* ---- 信号帧 ---- */
    u64  (*build_sigframe)(u8 *sp, const struct lx_sigctx *, lx_frame *);  /* 返回新 sp */
    int  (*restore_sigframe)(lx_frame *, const u8 *frame);

    /* ---- 号表 ---- */
    const struct lx_sysent *sysent;
    u32          sysent_n;
} lx_abi;

extern const lx_abi *const lx_abi_current;   /* 构建期由 Makefile 选一个 */
```

调用路径随之变成（`lx/sys.c` 只有这一段碰帧）：

```c
void lx_syscall(lx_frame *f)
{
    const lx_abi *a = lx_abi_current;
    u64 arg[6];
    const struct lx_sysent *e;
    u32 i;

    if (a->id != *(u32 *)f->b) { lx_panic_abi_mismatch(); }   /* 帧头校验 */
    for (i = 0; i < 6u; i++) { arg[i] = a->arg(f, i); }
    e = lx_sysent_find(a->sysent, a->sysent_n, a->nr(f));
    a->set_ret(f, e != (const struct lx_sysent *)0
                     ? e->fn(lx_cur_task(), arg)
                     : lx_enosys(a->nr(f), arg));
}
```

### 2.1 两份后端的差异清单（这是"必须分开写"的证据，不是可选优化）

| 项 | `abi/x86_64-linux` | `abi/rv64-linux` |
|---|---|---|
| 系统调用入口 | `syscall`（LSTAR）/ `int 0x80` | `ecall`（U 态），号在 `a7`，参数 `a0–a5` |
| `read` / `openat` / `mmap` / `clone` | 0 / 257 / 9 / 56 | 63 / 56 / 222 / 220 |
| `struct stat` | x86-64 私有 144B | asm-generic 128B |
| 信号帧 | 1280B，`pretcode` + `ucontext.gregs[23]` | `rt_sigframe{siginfo, ucontext{uc_mcontext, uc_sigmask}}`，带 f 寄存器区 |
| `sigreturn` | `RAX=15` 的 pretcode 技巧 | `a7=139` 的 restorer（libc 自带，内核只把 `ra` 指过去） |
| TLS | `arch_prctl(ARCH_SET_FS)` → MSR_FS_BASE | 无 `arch_prctl`；`clone(CLONE_SETTLS)` 写 `tp`，线程切换必须换 `tp` |
| FPU | 不声明即不保存（x86 端口用 `-mno-sse`） | **必须有**：lp64d 每个函数都可能用 `f0–f31`，帧里要有 FP 区 + `mstatus.FS` 管理 |
| 页表 | 4 级，`CR3` | Sv39 三级，`satp` + `sfence.vma` |
| 内核是否入页表 | **必须**（ring0 也走 CR3） | **不必**（M/S 模式 trap 后不看 `satp`）→ `CAP_KERNEL_UNMAPPED` |
| 指令一致性 | 天然一致 | 装载/写代码后必须 `fence.i`（+ 非一致 cache 的 CMO） |

---

## 3. 现有代码改造映射（A1 的施工清单）

| 现状 | 目标 | 说明 |
|---|---|---|
| `include/c6os/linux_abi.h:204-221` `LxRegs` | `port/x86_64/frame.h` | 布局归架架；名字保留，只是搬家 |
| `include/c6os/linux_proc.h:37-42` `LxFrame` | `lx/lx_frame.h` 的 `lx_frame` | 壳 + `frame_size`，内容不透明 |
| `include/c6os/linux_abi.h:30-190` `LX_SYS_*` | `abi/x86_64-linux/sysent.c` | 编译期常量变表项；`LX_SYS_*` 只留给 abi 目录内部 |
| `linux/sys.c` 的 `switch (r->rax)` | `abi/*/sysent.c` 号表 + `lx/sys.c` 查表 | 语义函数按 `(t, a[6])` 签名改写；`goto` 式 fallthrough 消失 |
| `linux/fs.c:607` `fill_stat` 144B | `abi/x86_64-linux/layout.c` | `lx/` 只产 `struct lx_stat`（中性） |
| `linux/sig.c:11-46` 帧偏移宏 | `abi/x86_64-linux/sigframe.c` | `lx/sig.c` 只决定"投哪个信号、掩哪些位、栈放哪" |
| `linux/elf64.c:315` `"x86_64"` | `abi/*/auxv.c` | 同时补 `AT_HWCAP`（rv64: `COMPRESSED/ FP/ D`） |
| `linux/elf64.c:120,153` 缺 `e_machine` 校验 | `abi->elf_machine` 校验 | **现在不查**：把 x86 二进制丢给 rv64 会静默跑飞。顺手修 |
| `linux/proc.c:62-65` `x86_resume_user` | `port_user_return(&f)` | 恢复用户态是 port 的事 |
| `linux/proc.c:250` `irq_frame`（20 qword） | `lx_frame.kind` + `abi->pc/sp` | 帧种类由 abi 解释，核心不再有 x86 帧长度假设 |
| `linux/proc.c:500` 伪造 `&x86_resume_user` | `port_user_enter(as,&f)` | 换架构只剩这一处 asm |
| `port/x86_64/linux.c` GDT/TSS/LSTAR/FS_BASE | 原样保留在 port | 它们本来就是 port 的活 |
| `include/c6os/config.h:53-75` 布局常量 | `abi/*/layout.c` 常量 | `config.h` 只留上限（`LX_MAX_PROC` 等） |
| `include/c6os/port.h` 里 `port_ata_read/write`、`port_net_*` | `dev/` 的 `os_blkdev` / `os_netdev` | 见 §6；架构头里不该出现 IDE/网卡 |

改造顺序（每步都能编、都能验收）：

1. **A1-a** 建 `lx/` 与 `abi/x86_64-linux/` 骨架，搬 `LxRegs`/`LxFrame`，把帧访问器抽出来。
   `linux/*.c` 保留文件名做转发的薄壳，一次只动一个文件。验收：`check-linuxabi-l1` 绿。
2. **A1-b** 号表化：`lx/sys.c` 从 `switch` 变查表。验收：`-l2..-l8` 绿。
3. **A1-c** 布局外移：`stat`/`sigframe`/`auxv`/布局常量。验收：`check-linuxabi-m9`、`-l6`、`check-alpine` 绿。
4. **A1-d** 入口外移：`port_user_enter/return`。验收：`-l5`、`-l7`、`-l8` 绿（`-l8` 是最能证明帧没写坏的）。
5. **A1-e** 删掉转发壳，`linux/` 目录消失。验收：全量 + 二进制体积对比（§5.3）。

> A1 全程**不改任何语义**。判断标准只有一条：`check-linuxabi-l1..l8` + `-m13` + `check-alpine` 的输出与改动前逐字一致。

---

## 4. 能力位与构建门禁（二维）

现在的门禁是一维的（"有没有 MMU"）。加一维"ABI 的架构是否等于 port 的架构"：

```
C6OS_ABI ?= none            # none | x86_64-linux | rv64-linux
C6OS_LINUX_ABI=1            # 兼容别名：等价于 C6OS_ABI=x86_64-linux（旧命令行不受影响）
C6OS_PORT_ARCH              # 由 port 目标声明：x86_64 | rv64 | rv32 | xtensa
```

`tools/chk-abi.sh <port> <arch> <caps> <abi>`，规则按顺序：

1. `abi=none` → 放行，`lx/`、`abi/` 不参与编译（对象列表门禁，不是 `#if 0`）。
2. `abi != none` 且 `caps & (MMU|USER|IRQ_USER) != 0x7` → 拒绝（沿用 `chk-caps.sh` 的措辞）。
3. `abi != none` 且 `abi.arch != port.arch` → **拒绝**，错误信息给出可选后端列表。
   逃生阀 `C6OS_ABI_EMU=1` 保留但当前无实现（将来若真要做 x86-on-riscv 解释器，从这儿进）。
4. `abi != none` 且 `abi` 要求 `CAP_FPU` 而 port 没有 → 拒绝（rv64-lp64d 需要 D 扩展）。
5. 解析期 `$(error)` 一道（对 `all`/`s3` 这类隐含目标），`caps-check-*` recipe 一道 —— 与现状同构。

### 4.1 能力位补充（`include/c6os/caps.h`）

| 新位 | 含义 | 谁需要 |
|---|---|---|
| `CAP_KERNEL_UNMAPPED` | 内核地址空间不进用户页表（trap 后内核直接物理寻址） | rv64 M-mode 内核为真；x86-64 为假 |
| `CAP_FPU` | 有 FPU 且 ABI 会用到 | ABI 后端声明需求 |
| `CAP_ICACHE_SYNC` | 写代码后需要显式同步指令缓存 | C906 为真；x86 为假（实现为空函数即可） |
| `CAP_DMA_NONCOHERENT` | 设备 DMA 与 CPU cache 非一致 | C906 为真；x86 为假 |

对应 port 契约（`include/c6os/port_arch.h`，新文件）：

```c
void port_icache_sync(u64 va, u64 len);      /* x86: {}；C906: fence + dcache CMO + fence.i */
void port_dma_sync(u64 pa, u64 len, u32 dir);/* x86: {}；C906: th.dcache.cipa + th.sync.s */
```

`lx/` 在装载完任何可执行映像后调用 `port_icache_sync()`（**不是** `abi` 调 —— 这是架构属性）。

### 4.2 门禁矩阵（施工后应长这样）

| port | arch | caps | 可开 ABI |
|---|---|---|---|
| `port/x86_64` | x86_64 | `MMU|USER|IRQ_USER` | `x86_64-linux` |
| `port/rv64virt`（新，A2 引入） | rv64 | `MMU|USER|IRQ_USER|FPU|ICACHE_SYNC` | `rv64-linux` |
| `port/sg2002`（新，V0 引入） | rv64 | 同上 + `DMA_NONCOHERENT|KERNEL_UNMAPPED` | `rv64-linux` |
| `port/s3` | xtensa | 0 | ❌（解析期拒绝，现状不变） |
| `port/rv32` / `rv32bare` | rv32 | 0 | ❌ |
| `port/sim` | host | 0 | ❌（只做语义回归） |

### 4.3 零侵入的证明方式（沿用既有做法）

`C6OS_ABI=none` 时：
- `make s3` 的 `size` 与改动前逐字节相同（S3 是"零影响"的守门员）；
- `nm build/s3/c6os.elf | grep -c lx_` 为 0；
- `make check-all` 绿。

---

## 5. 里程碑与验收

| 编号 | 内容 | 验收（可复现命令 + 期望输出） |
|---|---|---|
| **A1** | `lx/` + `abi/x86_64-linux/` 拆分，x86 行为零变化 | `make check-linuxabi-l1..l8`、`check-linuxabi-m13`、`check-alpine` 输出与改动前逐字一致；`make check-all` 绿 |
| **A2** | `port/rv64virt` + `abi/rv64-linux`（qemu `-M virt`），静态 ELF | `make check-abi-rv64-l2` → `PASS: rv64-abi L2（未修改的 riscv64 静态 ELF 在 U 态跑通）` |
| **A3** | 动态链接 + 信号 + 线程 + 抢占 | `check-abi-rv64-l4/l5/l6/l7/l8` 五条，输出对齐 x86 侧的验收串 |
| **A4** | FP 扩展状态（`copy_ext`）与 `mstatus.FS` 管理 | 浮点密集的 4 线程 + 信号嵌套用例计数正确（x86 侧无此用例，新增 `tests/linux/fpthr.c`） |
| **A5** | 门禁二维化 + `check-layering` grep 检查 + 负测试 | `make check-abi-gates`：① `C6OS_ABI=x86_64-linux` + rv64 port → 解析期 error；② `lx/` 里出现 `rax` → CI 失败 |
| **A6** | 文档与表格同步 | `linux-abi.md` §2.1 增补 ABI 矩阵；`todo.md` 勾选 |

A2 之前不碰硬件：`rv64virt` 与 `sg2002` 在 ABI 层上看不出区别（都 Sv39 + U 态 + `ecall`），
所以**先把逻辑在 qemu 上跑绿，再上板**（见 [maixcam-bringup.md](maixcam-bringup.md) §7 的 V2）。
本机当前缺 `qemu-system-riscv64` 与 `gcc-riscv64-linux-gnu`，A2 第一件事是装它们。

---

## 6. 与设备模型的关系（只列接口，细节见板级文档）

Linux ABI 的 `/dev` 面（`openat("/dev/fb0")` 之后能 `ioctl`/`mmap`）需要"设备对象"，
这层同样是平台无关的，且与 ABI 无关：

```c
typedef struct OsFileOps {          /* 已有，见 linux-abi-complete.md §3.5 */
    ssize_t (*read)(OsFile *, void *, size_t);
    ssize_t (*write)(OsFile *, const void *, size_t);
    int     (*poll)(OsFile *, u32 events);
    int     (*ioctl)(OsFile *, u32 req, u64 arg);
    int     (*mmap)(OsFile *, OsVma *);
    int     (*close)(OsFile *);
} OsFileOps;
```

- **块设备**：`os_blkdev` 替换 `port_ata_read/port_ata_write`；x86 的 ATA 与板子的 SD 都是它的实现。
- **帧缓冲/相机/输入/音频**：各是一个 `OsFileOps` + 一个 `dev/<chip>/` 驱动 + 一张 `board/<board>/` 接线表。
- `abi/` 不参与：`ioctl` 的**请求号**是 Linux 的（属 ABI 常量表），**实现**属驱动。

这样 A1 的"ABI 无关化"和板级的"设备点亮"是两条互不阻塞的线，可以并行施工。

---

## 7. 明确不做

- **同一内核里多 ABI 共存**（x86-64 + rv64 同时在线）——每次构建选一个。
- **x86-64 二进制在 RISC-V 上执行**（解释器/JIT）——不在本轮，也不推荐。
- i386 / ilp32 / x32 等 32 位 ABI；其他架构（arm64/loongarch）后端——结构上留了口子，不实现。
- 把 `lx/` 改写成 c6 语言——`linux-abi.md` §1 的理由不变（必须逐字节复现 C ABI 产物），
  `lx/` 与 `abi/` 继续用 C，只是多了一层约束检查。
- 为跨架构保留兼容层（"让 x86 的表也能跑"）——宁可解析期报错，不要运行期猜。
