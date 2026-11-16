# 内核子系统补课：VFS/文件系统 + 网络（SAL）

> 状态：**M9-0 已落地**（VFS 核心 + romfs，`make check-vfs` 绿）；M9-1..M9-4 / M10 待做 · 关联 [architecture.md](architecture.md) §3 ABI 规则 ·
> [liteos-comparison.md](liteos-comparison.md) 功能矩阵 · [todo.md](todo.md) M9/M10 ·
> [api.md](api.md) 错误码 · [wifi-plan.md](wifi-plan.md)（S3 闭源 WiFi，另立）
>
> 范围：只补 **c6OS 内核侧缺的子系统**，API 面**按 OpenHarmony LiteOS-M 的兼容层来
> 对齐**（`LOS_Fs*` 一族 + `sal_*` 一族），不追求 POSIX/Linux 全兼容。

## 0. 先说清楚：这是不是"硬缺口"

**不是硬件缺口，是子系统缺口。** 盘点一下两边：

| 能力 | c6OS 现状 | 硬件/工具链是否有 |
|---|---|---|
| 中断、tick、定时器 | 有（`hwi`/`tick`/`swtmr`） | ✓ |
| 任务/同步/IPC | 有（task/sem/mutex/event/queue/membox） | ✓ |
| 动态内存 | 有（`heap.c6`，best-fit，最多 4 pool） | ✓ |
| 块设备 | **只有裸扇区 PIO 读**（`port_ata_read`），无写、无设备抽象 | ✓ x86-64 ATA PIO；S3 有 SPI NOR/SDIO |
| 文件系统 | **完全没有**（无 VFS、无 inode、无 dentry） | ✓ 同上 |
| 网络 | **完全没有**（无 netif、无协议栈、无网卡驱动） | ✓ x86-64 有 PCI 网卡（qemu e1000/virtio）；S3 有闭源 WiFi 库 |
| 网络相关内核设施 | 有 sem/mutex/queue/timeout（够用） | ✓ |

所以"做不了"的部分只有两块**新增代码**：
1. **VFS 抽象层 + 块设备层**（约 1.2–1.8k 行 `.c6`），一次性投入，之后挂任何文件系统；
2. **协议栈 + netif + 网卡驱动**（约 3–5k 行），其中 TCP 状态机是大头。

另外三条是**真实约束**（不是借口，是设计输入）：
- **S3 只 512KB SRAM、无 MMU**：S3 上做 ramfs + LittleFS/FAT 只读是可行的，但**网络只做
  WiFi（闭源栈）+ UDP 薄封装**，不要指望跑完整 TCP 栈；
- **S3 的 WiFi 是 windowed ABI 闭源库**（见 `docs/wifi-plan.md`）：网络走 WiFi 时，
  驱动/协议栈大半在闭源库里，SAL 只是薄薄一层；
- **c6 的 ABI 规则**（`architecture.md` §3）：跨 C 边界只传 `void*`/`u32`/`u64`/函数指针
  —— 所有 VFS/网络接口都按这条设计，结构体一律内核内部用。

## 1. 要补的 LiteOS-M API 面（对齐清单）

> API 名按 OpenHarmony LiteOS-M 的 `components/fs/vfs`（`los_fs.h` 一族）与
> `components/network/sal`（`sal.h`）对齐。实现时**函数名与语义对齐，内部结构不复制**。

### 1.1 文件系统（`include/los/los_fs.h`，新增）

| LiteOS-M 接口 | c6OS 现状 | 本次目标 |
|---|---|---|
| `LOS_FsMount` / `LOS_FsUmount` | 无 | M9-0 |
| `LOS_FsOpen` / `LOS_FsClose` | 无 | M9-0（只读）→ M9-1（可写） |
| `LOS_FsRead` / `LOS_FsWrite` / `LOS_FsLseek` | 无 | M9-1 |
| `LOS_FsStat` / `LOS_FsFstat` / `LOS_FsReaddir` | 无 | M9-1 |
| `LOS_FsUnlink` / `LOS_FsRename` / `LOS_FsMkdir` / `LOS_FsRmdir` / `LOS_FsTruncate` | 无 | M9-1（ramfs） |
| `LOS_FsSync` / `LOS_FsFormat` | 无 | M9-2（块设备后端）/ M9-3（FAT） |
| `LOS_FsDup` / `LOS_FsFcntl` / `LOS_FsIoctl` | 无 | M9-4（接到 fd 语义时） |

### 1.2 块设备（`include/los/los_block.h`，新增）

| 接口 | 现状 | 目标 |
|---|---|---|
| `LOS_DeviceRegister` / `LOS_DeviceUnregister` | 无 | M9-2 |
| `LOS_BlockRead` / `LOS_BlockWrite`（按块，带块大小/块数） | 只有 `port_ata_read(lba, count, pa)` | M9-2（同时补 `port_ata_write`） |
| 设备枚举 `LOS_DeviceGet` | 无 | M9-2 |

### 1.3 网络（`include/los/sal.h`，新增）

| LiteOS-M 接口 | 现状 | 目标 |
|---|---|---|
| `sal_register` / `sal_unregister` / `sal_getproto` | 无 | M10-0 |
| `sal_socket` / `sal_close` | 无 | M10-0 |
| `sal_bind` / `sal_connect` / `sal_listen` / `sal_accept` | 无 | M10-3（UDP）/ M10-4（TCP） |
| `sal_send` / `sal_sendto` / `sal_recv` / `sal_recvfrom` | 无 | M10-3 / M10-4 |
| `sal_setsockopt` / `sal_getsockopt` / `sal_shutdown` / `sal_ioctl` | 无 | M10-4（最小集） |
| `sal_select` | 无 | M10-4（超时 + 可读可写） |

## 2. 文件系统设计

### 2.1 分层

```
app / Linux ABI personality (open/read/write/getdents64…)
        │            ← M9-4 把这两条接起来（LxFile.kind == VFS）
        ▼
  LOS_Fs* 兼容面            include/los/los_fs.h      kernel/fsapi.c6
        ▼
  VFS 核心                  kernel/vfs.c6
  （mount 表 / 路径解析 / file 表 / ops 分发 / 权限位只做只读-可写两档）
        ▼
  具体文件系统              kernel/ramfs.c6（M9-1）、kernel/fatfs.c6（M9-3）
        ▼
  块设备层                  kernel/blkdev.c6     include/los/los_block.h
        ▼
  port 驱动                 port/x86_64/ata.c（补 write，PIO）
                            port/s3/flash.c（已存在，SPI NOR，按扇区）
                            kernel/romfs.c6（把现有 newc cpio 包装成只读 fs）
```

**关键取舍**：现有 initramfs 的 newc cpio 解析（`linux/fs.c`）不重写，M9-0 里把它
**包成一个只读文件系统**（`romfs`），这样"路径解析、fd 语义、readdir"三件事立刻
只有一份实现，Linux ABI 侧（M9-4）换后端即可。

### 2.2 数据结构（全部内核内部，不跨 ABI）

```c
/* kernel/vfs.c6：上限都按 LiteOS 风格做成编译期常量 */
#define LOS_MAX_MOUNTS   8
#define LOS_MAX_FILES    32
#define LOS_PATH_MAX     64
#define LOS_NAME_MAX     32

typedef struct LosFsOps {
    int  (*mount)(struct LosMount *m, const char *dev, void *data);
    int  (*umount)(struct LosMount *m);
    int  (*open)(struct LosMount *m, const char *rel, u32 flags, void **fh);
    int  (*close)(struct LosMount *m, void *fh);
    i64  (*read)(struct LosMount *m, void *fh, void *buf, u32 len);
    i64  (*write)(struct LosMount *m, void *fh, const void *buf, u32 len);
    i64  (*lseek)(struct LosMount *m, void *fh, i64 off, u32 whence);
    int  (*stat)(struct LosMount *m, const char *rel, LosStat *st);
    int  (*readdir)(struct LosMount *m, const char *rel, u32 idx, LosDirEnt *de);
    int  (*unlink)(struct LosMount *m, const char *rel);
    int  (*mkdir)(struct LosMount *m, const char *rel);
    int  (*rename)(struct LosMount *m, const char *from, const char *to);
    int  (*truncate)(struct LosMount *m, const char *rel, u64 size);
    int  (*sync)(struct LosMount *m, void *fh);
} LosFsOps;

typedef struct LosMount {
    u32 used;
    char path[LOS_PATH_MAX];      /* 挂载点，"/" 或 "/sd" */
    const LosFsOps *ops;
    void *priv;                   /* fs 私有（ramfs 的根节点、fat 的 BPB…） */
    u32 dev;                      /* 块设备号，ramfs 用 0xffff */
} LosMount;

typedef struct LosFile {
    u32 used;
    u32 refs;
    u32 mount;
    u32 flags;
    u64 pos;
    void *fh;                     /* fs 私有句柄 */
} LosFile;
```

- **路径解析**：`los_path_split("/a/b/c")` → 逐段 `next_name()`；处理 `.`/`..`；
  最长 64 字节，超过返回 `LOS_ERRNO_OS_INVALID_PARAM`。挂载点匹配用"最长前缀"。
- **挂载表**：静态数组 + 一把 mutex（`os_mutex`），mount/umount 时进临界区。
- **file 表**：静态 32 项，`open` 分配、`close` 释放；`refs` 给 `dup` 用（M9-4）。
- **错误码**：内部统一用 `int`（0 成功，负 = POSIX `errno` 风格），
  `los_fs.h` 出口再翻译成 `LOS_OK`/`LOS_NOK`/`LOS_ERRNO_*`（映射表放 `kernel/fsapi.c6`）。
  理由：Linux ABI 侧要的是 `-ENOENT` 这种值，只有一处翻译最不容易错。

### 2.3 ramfs（M9-1）

- 节点：`{ used, type(FILE/DIR), name[LOS_NAME_MAX], parent, sibling, first_child, size, cap, u8 *data }`
  静态池 `LOS_RAMFS_MAX_NODES = 32`，数据从 `os_mem_alloc` 拿（4KB 起步，按需翻倍）。
- 目录项用"父指针 + 兄弟链表 + 首子指针"（不引入 dentry 缓存，32 个节点不需要）。
- 支持：create/truncate、read/write（顺序 + lseek）、readdir、unlink（仅空文件/空目录）、
  mkdir/rmdir、rename（同父改名）。
- 上限的意义：**故意做小**，用于把 VFS 语义跑对；真实容量走块设备文件系统（M9-2/M9-3）。

### 2.4 块设备层 + ATA 写（M9-2）

- `los_block.h`：`{ used, name[8], blk_size, blk_count, read(dev, blk, cnt, buf), write(...) }`，
  注册表 8 项；`buf` 是内核虚拟地址（x86-64 的 PIO 用恒等映射，S3 用内部 RAM 指针）。
- `port_ata_write(lba, count, pa)`：PIO 写（`0x1F0` 数据口 + `0x30` WRITE SECTORS），
  与现有读路径共用 `ata_wait()`；写完 flush cache 命令（`0xE7`）。
- `kernel/blkdev.c6` 提供 `los_block_read/write`，并带一个 **直通**（no cache）实现：
  先不做页缓存/回写策略，写操作立刻进设备。理由：先把语义做对，性能留给后面
  （`docs/perf.md` 那套测量方法可以复用）。

### 2.5 FAT 只读（M9-3）

- 只支持 **FAT16/FAT32、短文件名（8.3）**、只读：解析 BPB → FAT 表 → 根目录/子目录簇链 →
  按簇读文件。约 400–600 行。
- 不做：长文件名、写、格式化、碎片整理、exFAT。
- 为什么选 FAT 而不是 LittleFS：qemu 上可以直接用 `mkfs.vfat` 造镜像并 `-drive` 挂第二块盘，
  **验收不需要额外工具链**；LittleFS 需要自己造镜像工具的兼容实现。

### 2.6 与 Linux ABI 打通（M9-4）

- `LxFile` 增加一种 `kind = LX_FD_VFS`（现在只有 `FILE`/`CONSOLE`/`PIPE_*`），
  `openat/read/write/lseek/fstat/newfstatat/getdents64/close` 按 kind 分派到 VFS。
- 现有 `linux/fs.c` 的 cpio 节点表 → 变成一个 `romfs` 实现（`kernel/romfs.c6`），
  `lx_fs_init` 改为 `LOS_FsMount("/", "romfs", ...)`。
- 验收：`busybox sh -c 'echo hi > /tmp/x; cat /tmp/x'` 在 qemu 里输出 `hi`
  （`/tmp` 挂 ramfs；`/bin` 等仍来自 romfs）。

### 2.7 开关与零侵入

- `C6OS_VFS ?= 0`（Makefile 变量，传给 `-DC6OS_VFS`）；为 0 时
  `kernel/vfs.c6`/`ramfs.c6`/`blkdev.c6`/`fatfs.c6`/`romfs.c6` **不进对象列表**，
  镜像一个字节都不变（与 `LINUX_ABI` 同一套门禁写法）。
- 为 1 时新增对象预计 text ≈ 4–6KB（ramfs+romfs）/ 再加 3–4KB（fat 只读），
  数据 ≈ 2KB（挂载表 + file 表 + ramfs 节点池），进 `docs/todo.md` 的尺寸预算。

## 3. 网络设计

### 3.1 分层

```
app / Linux ABI（socket 族，M10-4 之后可选接到 41..55 号）
        ▼
  SAL 兼容面        include/los/sal.h        kernel/sal.c6
  （协议栈注册表 + socket 表 + 按 domain/type 分发）
        ▼
  协议栈            kernel/net/ip.c6  arp.c6  icmp.c6  udp.c6  tcp.c6
                    kernel/net/netif.c6（发送/接收队列、MTU、MAC/IP）
        ▼
  网卡驱动          port/x86_64/e1000.c（PCI 0xCF8/0xCFC 枚举 + MMIO + 轮询/中断收包）
                    未来：virtio-net、S3 WiFi（闭源，另立）
```

**关键取舍**：
- 协议栈自己写（约 2.5–3k 行），**不引 lwIP**：lwIP 是 C + 一堆宏 + 自己的内存管理，
  塞进 c6 的 ABI 规则（跨边界只传 void*/u32/u64）和 `.c6` 编译链要额外适配层；
  而我们要的只是"最小可靠子集"，自己写反而可控。
- 收包：**中断里只把数据搬进预分配的 ring 并唤醒 `net_rx` 任务**，协议处理全在任务上下文
  —— 避免在 IRQ 里跑 TCP 状态机（也避开"中断安全的堆"这个缺口）。
- 定时：TCP 重传/延迟 ACK 用现有 `os_timeout`/`tick` 挂一个 1ms 的软定时器。

### 3.2 数据结构（要点）

```c
typedef struct NetBuf {           /* pbuf 风格链表 */
    struct NetBuf *next;
    u16 len;
    u16 off;                      /* 协议头偏移 */
    u8 *data;
} NetBuf;

typedef struct NetIf {
    u32 used;
    u8  mac[6];
    u32 ip;                       /* 网络序 */
    u32 mask;
    u32 gw;
    int (*linkoutput)(struct NetIf *nif, NetBuf *p);
    u32 rx_packets, tx_packets, rx_dropped;
} NetIf;

typedef struct SalSock {
    u32 used;
    u32 domain, type, proto;
    const SalProto *ops;          /* 由 sal_register 注册的协议栈 */
    void *priv;                   /* 该协议栈的 socket 结构 */
    i32 last_errno;
} SalSock;
```

- 16 个 socket 槽（LiteOS 的 `SAL_SOCKET_NUM` 可配，这里先 16）。
- `sal_register(&SalProto)` 里 `SalProto` 是函数表（socket/bind/connect/send/recv/...），
  与 `LosFsOps` 同一思路：**用函数表解耦**，Linux ABI 侧或 shell 命令可以注册假协议栈做单测。

### 3.3 里程碑（细节见 §5）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M10-0 | SAL 骨架：注册表 + socket 表 + 假协议栈 | c6 单测：注册→socket→close→反注册，错误路径返回 -1 |
| M10-1 | ARP + IPv4 + ICMP echo + loopback | qemu `ping 10.0.2.2`（slirp 网关）通 |
| M10-2 | e1000 驱动（PCI 枚举、MMIO、收发包） | 收发包计数增长，ARP 有应答 |
| M10-3 | UDP + `sal_sendto/recvfrom/bind` | 与宿主 `nc -u` 互发（qemu `hostfwd` 或 slirp） |
| M10-4 | TCP 最小子集 + `listen/accept/connect/send/recv/select` | qemu 里 `GET / HTTP/1.0` 拿到宿主 `python3 -m http.server` 的 200 |

### 3.4 明确不做（网络）

IPv6、IP 分片/重组、SACK、拥塞控制（只做慢启动+固定窗口）、零拷贝/NAPI、
DHCP/DNS（先用静态 IP）、S3 闭源 WiFi 栈（`docs/wifi-plan.md`）。

## 4. 与现有内核的耦合点

| 需求 | 现有设施 | 注意事项 |
|---|---|---|
| 内存 | `os_mem_alloc/free`（`heap.c6`，best-fit） | 分配是 O(n)，**别在收包热路径反复 alloc**：pbuf 用固定池 |
| 互斥 | `os_mutex` | VFS 挂载表/file 表的临界区；**不要在中断里用 mutex** |
| 等待 | `os_sem`/`os_queue` | `net_rx` 任务等"有包"信号量 |
| 定时 | `os_timeout`/`tick`/`swtmr` | TCP 重传、`sal_select` 超时 |
| 中断 | `hwi.c6` | 网卡中断只做"搬包 + post sem"；ATA 仍用 PIO 轮询 |
| 错误码 | `LOS_ERRNO_*`（`api.md` §4） | VFS/SAL 出口统一翻译，内部用 errno 风格 |
| 运行库 | `include/c6os/string.h`（`rt/c6rt.c` / `rt/x86_libc.c` 提供实现） | **一律复用** `memcpy/memset/strlen/strcmp/strncpy`，不手搓循环；见 `architecture.md` §2.1 |
| ABI | `architecture.md` §3 | 跨 C 边界只传 `void*`/`u32`/`u64`/函数指针 |

## 5. 里程碑与验收（汇总）

| 里程碑 | 内容 | 验收命令与期望 |
|---|---|---|
| **M9-0** | 开关 + `los_fs.h`/`los_block.h` + VFS 核心（mount/路径/file 表）+ `romfs` 包装现有 cpio | `make check-vfs`：`mount("/","romfs")` 成功、`open("/bin/busybox")` 成功、`read` 前 4 字节是 `\x7fELF` |
| **M9-1** | ramfs 可读写（create/write/read/seek/readdir/unlink/mkdir/truncate） | `make check-vfs`：写 1KB → 读回一致 → readdir 有它 → unlink 后 open 失败 |
| **M9-2** | 块设备层 + `port_ata_write` | 写 8 扇区到 LBA 4096 → 读回逐字节一致（qemu） |
| **M9-3** | FAT16/32 只读（8.3 名） | 挂 `mkfs.vfat` 造的镜像：`readdir("/")` 看到文件、`read` 内容一致 |
| **M9-4** | Linux ABI 接 VFS | `busybox sh -c 'echo hi > /tmp/x; cat /tmp/x'` → `hi` |
| **M10-0** | SAL 骨架 + 假协议栈 | `make check-sal`：注册/socket/close/反注册 + 错误路径 |
| **M10-1** | ARP+IPv4+ICMP+loopback | qemu 里 `ping 10.0.2.2` 通（ICMP echo 有回） |
| **M10-2** | e1000 | 收发包计数增长；ARP 有应答 |
| **M10-3** | UDP + socket 面 | 与宿主 `nc -u` 互发一次 |
| **M10-4** | TCP 最小子集 | `GET / HTTP/1.0` 拿到 `HTTP/1.0 200` |

**零侵入验收**（每个里程碑都要过）：`C6OS_VFS=0 C6OS_NET=0 make -n s3` 的对象列表
里不含任何新增文件；打开后 S3 的尺寸增量写进 `docs/todo.md` 的预算表。

## 6. 风险表

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| TCP 状态机写错（重传/关闭/半开） | 高 | M10-4 反复 | 先 UDP 把 netif/驱动验透；TCP 只做"单向大块 + 干净关闭"，用 3 个固定用例（GET、大文件、对端先关） |
| e1000 初始化/描述符环踩坑 | 中 | M10-2 卡住 | 先**轮询**收包（不开中断），跑通再换中断；qemu `-device e1000` 固定模型 |
| 内存不够（S3 512KB） | 中 | S3 侧网络做不了 | S3 只做 FS（ramfs/romfs/FAT 只读）；网络只在 x86-64 打开 |
| FAT 兼容性（簇链、边界） | 中 | M9-3 数据不对 | 只支持 8.3 只读；用 `mkfs.vfat` 的小镜像（<8MB）做基准，逐簇打印比对 |
| 与 Linux ABI 的 fd 语义冲突 | 中 | M9-4 返工 | VFS fd 先独立编号，M9-4 只做"桥接"不改 VFS 内部 |
| 尺寸/实时性回归 | 低 | 违反项目主线 | 两个开关默认 0；每个里程碑记录 text/data/bss 增量 |
| 分配器 O(n) 拖慢收包 | 中 | M10-2 丢包 | pbuf 固定池 + ring，热路径零 alloc |

## 7. 明确不做

- POSIX 全套（`mmap` 文件映射、`inotify`、`epoll` 的 FS 语义）、权限/多用户；
- 符号链接、硬链接计数、稀疏文件；
- 网络：IPv6、分片重组、SACK、拥塞控制高级部分、DHCP/DNS、TLS；
- S3 闭源 WiFi 栈的改造（另见 `docs/wifi-plan.md`）；
- 块设备 DMA、写缓存回写、日志/掉电一致性（先直写）。

## 8. 施工记录

### M9-0（VFS 核心 + romfs）已完成

落地文件：`include/los/los_fs.h`、`include/los/los_block.h`（只有声明，实现在 M9-2）、
`include/c6os/vfs.h`、`include/c6os/string.h`、`kernel/vfs.c6`（挂载表/路径解析/file 表）、
`kernel/romfs.c6`（newc cpio 只读后端）、`kernel/fsapi.c6`（`LOS_Fs*` + 按 fs 类型分发）、
`tests/t_vfs.c6`；`Makefile` 里 `C6OS_VFS` 默认 0，为 1 才编这三个 `.c6`。

验收：

- `make check-vfs` → `PASS: vfs M9-0（romfs 挂载 + stat/open/read/readdir）`
- `make check-vfs-zero` → `PASS: C6OS_VFS=0 时 S3 不编 VFS（零侵入）`
- 回归：`check-linuxabi` 与 `-l2..-l8` 全绿（`-O1` 改动后复跑）

施工中撞到的两件事（都影响 M9-1/M10）：

1. **x86 stage2 只能装 255 扇区（≈127.5KB）**。`port/x86_64/boot16.S` 用三个 DAP
   读 LBA 1..255（127+120+8），所以 `flat.bin` 超过 130560 字节就会被**静默截断** ——
   现象是内核一句日志都不打就死（连 banner 都没有），非常像链接/入口问题。
   VFS 一上来把镜像顶到 157KB 直接撞线；现在压回 117KB。M10 网络必然会再撞，
   届时得扩 boot16 的 DAP 列表（或改成循环读）。
2. **c6 产出的 IR 需要 `-O1`**。`X86IRFLAGS` 之前没有优化级，clang 按 -O0 编 IR，
   几百行的内核文件就能吃掉 10KB+ 代码段（VFS 三个文件加了 44KB）。
   统一加 `-O1` 后：L3 镜像 111KB→108KB、vfs 镜像 158KB→117KB。
   S3 那条链（c6 → C99 → xtensa-gcc）不受影响。

另外：`LOS_Fs*` 的错误约定是 `-errno`（POSIX 风格），与 LiteOS-M 原文的
`LOS_ERRNO_*` 不同 —— 见 `include/los/los_fs.h` 顶部说明；函数名与参数顺序
保持与 LiteOS-M 一致。
