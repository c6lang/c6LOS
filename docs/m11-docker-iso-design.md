# M11：docker 镜像 → ISO → 虚拟机跑起来（可读写硬盘）

> 目标一句话：把 `docker export`/`docker save` 出来的根文件系统打成一张**可引导 ISO**，
> 在 qemu 里用 `-cdrom` 直接起机，镜像里的程序能跑，并且能往一块**可读写的数据盘**上写东西；
> 新增能力一律先走 LiteOS-M 兼容接口（`LOS_Block*` / `LOS_Fs*`），Linux ABI 只做薄封装。

设计口径沿用 `docs/vfs-net-design.md`：能跑、能验收、能零侵入，再谈优雅。

---

## 0. 先说清楚：这跟 `docker run` 差多远

我们做的是**"镜像即固件"（unikernel 风格）**，不是容器编排：

| 语义 | Docker | 本方案 | 说明 |
|---|---|---|---|
| 镜像内容 | OCI layer 叠加 | **构建期扁平化**成一份 rootfs | whiteout 在打包时消解，运行期无 overlay |
| 根文件系统 | overlayfs（可写层） | **romfs（只读，内存里）** | 光盘/镜像内容只读，符合 ISO 语义 |
| 可写空间 | 可写层 + volume | **FAT16 数据盘**（ATA，可写） | 宿主可直接挂载校验 |
| 隔离 | namespace + cgroup + seccomp | **无**（单机单镜像，一个 guest） | qemu 本身就是隔离边界 |
| 启动 | runc 拉起来 | **ISO 引导 → 内核 → 跑镜像 ENTRYPOINT** | 没有 daemon、没有 registry 客户端 |
| 镜像来源 | `docker pull` | `docker export/save` 落到本地 | 拉取和解包都在宿主侧做 |

所以"能不能 `docker run` 一个 nginx"——不能；"能不能把 nginx 镜像的 rootfs 打成 ISO 让它在
c6LOS 上跑起来"——这正是本里程碑要做的事（前提是它不依赖 Linux 那些我们没实现的内核设施，
见 §8 的边界）。**先把最小可验证的闭环打通，再谈 namespace/overlay。**

---

## 1. 现状盘点（M9/M10 之后手里有什么）

| 能力 | 现状 | 证据 |
|---|---|---|
| 硬碟引导（MBR） | `boot16.bin` 读 LBA 1..382（4 个 DAP，上限 195,584 B）→ `flat.bin` 装到 `0x10000` | `port/x86_64/boot16.S` + `Makefile` 的 `disk.img` 规则 |
| 内核载荷 | `flat.bin` **自包含**（`stage1.S` 用 `.incbin` 把 `stage2.bin` 塞进 `.stage2`），stage1 建页表/长模式后跳 `0x20000` | `port/x86_64/stage1.S`、`stage2.S`（`_start64` 清 `.bss` 再 `call os_main`） |
| 块设备层（LiteOS 面） | `LOS_DeviceRegister/Unregister/Get/Count`、`LOS_BlockRead/Write/Size/Count` | `include/los/los_block.h`、`kernel/blkdev.c6` |
| ATA | PIO 读 + 写都在（`port_ata_read/write`），x86 参考目标挂在 `ata.o` | `port/x86_64/ata.c`、`include/c6os/port.h:104-105` |
| VFS + 具体 FS | 挂载表/路径解析/file 表；`romfs`（newc cpio 只读）、`ramfs`（可读写，**32 节点 / 8KB arena**）、`fatfs`（FAT16/32 **只读**） | `kernel/vfs.c6`、`romfs.c6`、`ramfs.c6`、`fatfs.c6` |
| LiteOS FS 面 | 16 个 `LOS_Fs*` 全在，但**写类只认 ramfs**（`fsapi.c6` 里用 `os_str_eq(m->type, "ramfs")` 硬判断） | `kernel/fsapi.c6:165-360` |
| Linux ABI | 66 个 syscall（fork/clone/execve/wait4/futex/mmap/pipe/poll/openat/getdents64/信号/线程…），ELF 支持 `ET_EXEC` + `ET_DYN`+`PT_INTERP` | `linux/sys.c`、`linux/elf64.c` |
| 用户态 | 静态 glibc、动态 ld.so、shell 管道、信号、线程、抢占都有验收目标 | `check-linuxabi`、`-l4..-l8` |
| 网络 | SAL + ARP/IPv4/ICMP/UDP/TCP + e1000，能 ping 网关、能 `GET / HTTP/1.0` | `check-ip/ping/udp/tcp` |
| initramfs | `tools/mk-initramfs.py` 生成 newc cpio；**放在磁盘 LBA 2048**，由测试主程序 `port_ata_read` 搬进内存 | `Makefile:1238-1254`、`tests/t_x86_fs.c6` |

**结论**：缺的不是"文件系统"和"网络"，缺的是**"怎么从光盘启动"**和**"怎么往盘上写"**这两条链路，
外加一个**打包工具**把 docker 镜像变成 ISO。

---

## 2. 缺口清单（M11 要补的）

| 编号 | 缺口 | 后果 | M11 处理 |
|---|---|---|---|
| G1 | 没有 CD 引导链（`boot16` 只会读 512 字节扇区、`int 13h` LBA 1..） | `-cdrom` 起不来 | M11-0：El Torito no-emulation 引导块 `port/x86_64/cd0.S` + `tools/mkiso.py` |
| G2 | 载荷只能装在内核镜像里（`.incbin`），rootfs 没有"随 ISO 走"的通道 | 镜像得重编内核 | M11-1：ISO 里放第二段载荷 `rootfs.cpio`；**≤1 MB 的 rootfs** 直接跟在载荷里装到内存（M11-0 的实模式读盘路径就够），更大的才上 unreal mode / ATAPI |
| G3 | 可写存储太小：ramfs 32 节点 / 8KB arena；FAT16 只读 | 镜像里的程序写不了东西 | M11-2：FAT16 **写**（创建/写/追加/删除/截断/sync）+ 挂 `/data`；M11-4 再扩 ramfs |
| G4 | `fsapi.c6` 的写类操作按 FS 名字硬判断，FAT 根本没接 | FAT 上的写路径无法落地 | M11-2：把 FAT 接进同一条分派链（`fsapi.c6` 里 `os_str_eq(type, "fatfs")` 分支）。**ops 分发表重构推迟**，理由见 §9 M11-2 |
| G5 | 安装/解包工具没有：仓库里没有 tar/gzip/OCI/whiteout 相关代码 | docker 镜像用不上 | M11-3：`tools/mkdockeriso.py`（宿主侧 Python，`tarfile` + `json`） |
| G6 | Linux ABI 没有写类 syscall（`mkdir/unlink/rename/ftruncate/fsync/statfs`） | 用户态不能建文件、不能保证落盘 | M11-2（薄封装到 `LOS_Fs*`），本轮先做镜像跑起来必需的那几个 |
| G7 | 没有 namespace/cgroup/pivot_root/overlay | 不是容器 | **本轮不做**（§8） |
| G8 | Linux ABI 没有 socket 面（`socket/bind/connect/...` 未实现） | 用户态不能联网（SAL 只在 LiteOS 面） | M11-4 延伸 |

---

## 3. 总体架构

### 3.1 一张图

```
宿主机（一次性，构建期）                          qemu（运行期）
┌──────────────────────────┐              ┌──────────────────────────────────────┐
│ docker save/export       │              │  -cdrom c6os.iso   （只读，根 /）      │
│   ↓ tarfile + json       │              │  -drive data.img   （可写，挂 /data）  │
│ tools/mkdockeriso.py     │              │                                      │
│   扁平化（whiteout）      │  ────────►   │  ① El Torito 引导块 cd0               │
│   newc cpio (rootfs)     │              │     ↓ 读 ISO 两块载荷（staging 中转）  │
│   tools/mkiso.py         │              │  ② 内核 flat.bin → 0x10000 → 0x20000  │
│     ├─ 引导块            │              │  ③ rootfs.cpio   → 0x00800000         │
│     ├─ flat.bin (内核)   │              │  ④ 引导信息块 0x7000                   │
│     ├─ rootfs.cpio       │              │  ⑤ 内核：romfs 挂 /（内存里的 cpio）   │
│     └─ ISO9660 目录      │              │         fatfs 挂 /data（ATA 盘）       │
│   tools/mkfat.py         │              │  ⑥ 读 /etc/c6os.cmd → execve ENTRYPOINT│
│     └─ data.img (FAT16)  │              │  ⑦ 镜像程序往 /data 写 → 宿主校验      │
└──────────────────────────┘              └──────────────────────────────────────┘
```

### 3.2 ISO 布局（`tools/mkiso.py` 生成，LBA 单位 2048 字节）

| LBA | 内容 | 备注 |
|---|---|---|
| 0–15 | 系统区（全 0） | ISO9660 规定 |
| 16 | PVD（Primary Volume Descriptor） | 卷标 `C6OS-M11`，根目录记录 + 路径表指针 |
| 17 | El Torito Boot Record VD | 指向引导目录（boot catalog） |
| 18 | Volume Descriptor Set Terminator | `type=255` |
| 19 | Boot Catalog | 校验项 + 默认项：no-emulation、`load_rba=20`、`sector_count=4`（2048 B）、`load_seg=0x07C0` |
| 20 | `cd0.bin`（引导块，2048 B） | 头部带 `C6ISOLDR` 魔数 + `{内核 LBA,块数, rootfs LBA,块数, 目标地址}`，由 `mkiso.py` 现场填 |
| 21 … | `flat.bin`（内核载荷） | 按 2048 对齐；尾块补 0 |
| … | `rootfs.cpio`（newc） | 同上 |
| 末尾 | 根目录 + L/M 路径表 + `C6OS.KER`/`ROOTFS.CPI` 文件记录 | 让宿主 `mount -o loop` 能看见东西（也是验收证据） |

> 为什么自己写 ISO 生成器：我们只需要**极小的 ISO9660 + El Torito**，而且要求 **LBA 布局确定**
> ——引导块要按固定 LBA 直接读载荷，不想要 ISO9660 解析器（那得再写 200 行 C）。
> `tools/mkfat.py` 已经是同类工具的先例，风格一致。

### 3.3 引导流程（`cd0.S` 干的事，以 M11-0 实际实现为准）

```
BIOS 从 El Torito 把 LBA 20 的 2048 B 装到 0x7C00 并跳进来
  ├─ 归一化 CS:IP（BIOS 可能用 0x07C0:0000 进来）、清段寄存器、栈设 0x7C00
  ├─ 存 DL（引导盘号）、初始化 COM1（-nographic 下只有串口看得见）
  ├─ 打印载荷参数（mkiso 回填的 LBA/块数），出问题一眼定位
  ├─ load_payload(内核)：int 13h AH=42h，DAP 偏移恒 0、目标段每块 +0x80（2048/16）
  │    —— 先试"一次读完"（块数 16 位，110KB 才 54 块），失败退化成一次一块
  ├─ load_payload_hi(rootfs)：块数为 0 就跳过。每次 32 块（64 KB）读到 0x80000
  │    暂存区，再进 unreal mode 用 32 位寻址搬到 dst（0x00800000）——每块进出一次，
  │    BIOS 调用始终在干净的实模式段下做
  ├─ 写引导信息块（0x7000，魔数 'C6IS'）
  └─ 切 32 位保护模式（GDT 里 4GB 平坦代码/数据段），远跳 0x10000
```

**内核走实模式直读、rootfs 走 unreal mode**：M11-0 的载荷只有内核（110 KB），目标
`0x10000` 在 1 MB 以下，实模式的"段:偏移"完全够用，`int 13h` 的 DAP 本来就是段:偏移
——让目标段跟着每块 +0x80 走，连暂存区搬运都省了。rootfs 要放到 `0x00800000`（4 MB
以上，躲开 `KERNEL_RESERVED_END`），就得靠 unreal mode：ds/es 的缓存描述符保持 4 GB
限长，回到实模式后仍能用 32 位寄存器寻址搬运。

**unreal mode 的回程别用"清 PE 之后远跳"**：那一步的 `EA` 必须按 16 位编码才会被实模式
正确译码，写成 32 位形式就飞。M11-1 改成：先 `ljmp $0x18`（GDT 里的 16 位代码段，
D=0、base 0）进 16 位 PM 存根，再清 PE 直接往下跑，不再需要远跳。

**cd0 还得自己收 E820**：盘引导路径里是 `boot16.S` 收的，光盘引导压根不经过它；
少了 `0x5000` 那张表，`port_mmu_init()` 直接 "e820=00000000 / FAIL dmap"。

### 3.4 引导信息块（`0x7000`，32 字节）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `'C6' 'I' 'S'`（4 B） | 魔数，用来判断"这次是不是 cd0 引导的" |
| 4 | `u32 version` | 1 |
| 8 | `u32 flags` | bit0 = 从 CD 启动 |
| 12 | `u32 kernel_pa` / `kernel_len` | 内核载荷地址/长度（`0x10000`） |
| 20 | `u32 rootfs_pa` / `rootfs_len` | rootfs cpio 地址/长度（默认 `0x00800000`） |
| 28 | `u32 data_lba` | 数据盘起始 LBA（FAT16 引导扇区所在，0 = 没有） |

硬碟引导路径（`boot16`）**不写这个块**，内核看到魔数不对就当"没有 rootfs 载荷"，
行为与 M9/M10 完全一致 → 零侵入。

### 3.5 内存布局

```
0x0000000 – 0x00072684   内核镜像 + .bss（_start64 会清 .bss）
0x0007000 – 0x0007020    引导信息块（cd0 写）
0x0007C00 – 0x00083FF    引导块自身
0x0010000 – 0x0031648    内核载荷（flat.bin 链接在 0x10000）
0x0020000 – 0x002FFFF    cd0 的中转暂存区（64 KB）
0x0200000 – 0x0203000    引导页表（stage1 的 PML4/PDPT/PD，**别放东西**）
0x0400000                0–4 MB 内核保留（`KERNEL_RESERVED_END`）
0x0800000 – …            rootfs.cpio（M11-1 起；用 port_phys_reserve 从 PMM 里挖掉）
```

---

## 4. 关键决策

### 4.1 可写盘用 FAT16（而不是 ext2/自研）

- 宿主能直接验证：`mount -o loop data.img /mnt` 或 `mdir -i data.img`，写进去的文件肉眼可见 ← 验收证据；
- `tools/mkfat.py` 已经在生成 FAT16 镜像（M9-3 的测试盘），格式化不用重写；
- 只读路径已经跑通（`fatfs.c6` 的 `fat_read_` + 目录遍历），写路径是在同一套结构上加；
- LiteOS-M 的 FAT 兼容面（`LOS_Fs*` + `LOS_Block*`）对 FAT 最自然，S3 那边也是 FAT/LittleFS。

代价：8.3 短名、无长文件名（LFN）、无权限位。够用，且都写进"明确不做"。

### 4.2 LiteOS 接口优先的分层

```
Linux ABI（openat/write/fsync/mkdir…）      ← 薄封装，只做参数翻译
        ↓
LOS_Fs*（LOS_FsOpen/Write/Mkdir/Sync…）     ← **所有新能力必须先在这里可用**
        ↓
VFS 核心（挂载表 / 路径解析 / file 表 / ops 分发表）
        ↓
具体 FS：romfs（只读）｜ ramfs（可写，内存）｜ fatfs（可写，磁盘）
        ↓
LOS_Block*（LOS_BlockRead/Write）→ blkdev.c6 → port_ata_read/write
```

**M11-2 的头一件事**是把 `fsapi.c6` 里"按 FS 名字 if/else"改成挂载点上的
`const LosFsOps *ops`（open/read/write/mkdir/unlink/rename/truncate/sync/readdir/stat），
否则 FAT 挂上去也没人调用它。这条是纯重构，但没它后面全是补丁。

### 4.3 FAT16 写的顺序与一致性

一次 `LOS_FsWrite` 触发的落盘顺序（**先数据、再 FAT、最后目录项**，掉电最多丢数据不毁结构）：

```
1. 簇数据 → LOS_BlockWrite(数据扇区)
2. FAT 表 → 目录项链变了，先写 FAT
3. 目录项（大小/首簇/时间）→ LOS_BlockWrite(目录扇区)
4. LOS_FsSync / fsync → 再刷一次上面三步涉及的所有脏扇区（写穿，无缓存也行）
```

分配策略：从 FAT 表头开始 first-fit 扫空闲簇；不建 free-list（盘小，扫得起）。
删除：目录项首字节写 `0xE5` + 簇链归还（FAT 项清 0）；截断：归还尾部簇。
不做：LFN、簇碎片整理、`FSInfo`（FAT32 才需要）、坏簇标记。

### 4.4 ramfs 的定位

`/tmp` 继续是 ramfs（内存、快、可写），`/data` 是 FAT16 数据盘（持久）。
ramfs 的 32 节点 / 8KB arena 在 M11 不做扩容（M11-4 再说），镜像程序如果想写大文件就写 `/data`，
这条要写进文档和镜像里的 README，免得用户踩坑。

---

## 5. 里程碑与验收

每个里程碑都要：`make check-*` 可复现、文档同步、提交推送。

| 里程碑 | 内容 | 验收命令 | 打印 |
|---|---|---|---|
| **M11-0** ISO 启动链 | `port/x86_64/cd0.S`（El Torito no-emulation + unreal mode staging）、`tools/mkiso.py`、`tests/t_iso.c6`、`iso` 变体 | `make check-iso` | `PASS: iso M11-0（El Torito no-emulation 启动链）` |
| **M11-1** ISO 内嵌只读 rootfs | `cd0` 第二段载荷 + 引导信息块 + `port_phys_reserve()` + 内核从内存挂 romfs 到 `/`（`LOS_FsReaddir/Stat/Open/Read` 能看见镜像里的文件） | `make check-iso-rootfs` | `PASS: iso M11-1（rootfs 从 ISO 进内存并挂到 /）` |
| **M11-2** 可写硬盘（FAT16 写） | VFS ops 分发表重构；`fatfs.c6` 的 create/write/append/truncate/unlink/mkdir/sync；挂 `/data`；`LOS_Fs*` 全通；Linux ABI 补 `fsync/ftruncate/mkdir/unlink` | `make check-fat-write` | `PASS: fat M11-2（/data 可读写，宿主可见）` |
| **M11-3** docker 镜像 → ISO | `tools/mkdockeriso.py`（`docker save` tar / 目录 → 扁平化 → cpio → ISO + data.img + `/etc/c6os.cmd`）；ISO 里按 ENTRYPOINT 跑镜像程序，写 `/data`，宿主校验内容 | `make check-docker-iso` | `PASS: docker M11-3（镜像 → ISO → 跑起来 + 写盘）` |
| **M11-4**（延伸） | ramfs 去 32 节点/8KB；Linux ABI 的 socket 面；ATAPI 只读 CD 块设备；动态 musl 镜像 | 各自 `check-*` | — |

### M11-3 的"镜像怎么跑起来"

1. `mkdockeriso.py` 从镜像 config（`docker save` 的 `manifest.json` → `Config` JSON 文件）里取
   `Entrypoint` + `Cmd`，拼成一行写到 rootfs 的 `/etc/c6os.cmd`；
2. ISO 变体的测试主程序：读 `/etc/c6os.cmd` → `execve("/bin/sh", ["sh","-c",cmd])`；
   镜像里没 shell 就直接 `execve(argv[0], argv)`；
3. 镜像程序写 `/data/...` → 关机/退出 → 宿主 `mtools`/`mount -o loop` 读数据盘，内容对得上即 PASS。

---

## 6. 风险表

| 风险 | 影响 | 缓解 |
|---|---|---|
| 各 BIOS 对 El Torito 的 `sector_count`/load segment 解释不一致 | 引导块没被装上 | `sector_count=4`（2048 B）留余量；qemu/SeaBIOS 是主验收环境，真机另测 |
| unreal mode + `int 13h` 组合在某些 BIOS 上不稳 | rootfs 搬运失败 | 每个块读完校验 1 个魔数（cpio 头 `070701`）；失败就打印并停机，不死循环 |
| `int 13h` 单次传输上限（有的 BIOS 限 127 扇区） | 读失败 | 每次 32 块（64 KB），远低于上限 |
| rootfs 很大（>64 MB） | 内存装不下 | `mkdockeriso.py` 检查 `rootfs_len`，超过 64 MB 直接报错并提示裁剪 |
| FAT16 写的掉电一致性 | 镜像/目录损坏 | 固定"数据→FAT→目录项"顺序 + `LOS_FsSync` 显式刷盘 |
| 镜像里的程序依赖 `/proc`、`/dev`、动态 musl 之外的库 | 跑不起来 | M11-3 先选静态 busybox 的镜像做验收；动态镜像归 M11-4，`mkdockeriso.py` 打印缺什么 |
| ISO 变体的内核尺寸（Linux ABI + VFS 全带）继续长 | 载荷超 195KB（那是硬碟引导的上限，CD 无此限制） | CD 路径按块读，不设 195KB 上限；只有 `boot16` 那条路受限 |

---

## 7. 明确不做

- namespace（mount/pid/net/uts/ipc）、cgroup、seccomp、capabilities —— 这一轮"镜像即固件"，隔离交给 qemu；
- overlayfs / 可写层 / layer 缓存 —— 镜像在构建期扁平化；
- `docker pull` / registry v2 客户端 —— 镜像来源是宿主侧的 `docker save/export`；
- LFN 长文件名、FAT32 写、exFAT、ext2/4 —— 只做 FAT16 写；
- ATAPI/SCSI 光驱驱动（M11-4 视需要）；
- 多镜像/多容器、进程 namespace 里的 PID 重映射。

---

## 8. 边界：什么样的镜像能跑

能跑（M11-3 验收口径）：

- 静态或动态（musl/glibc + `ld.so`）x86-64 ELF，`/lib`、`/bin` 结构正常；
- 不依赖 `/proc`、`/sys`、`/dev` 里的具体文件（`/dev/null` 之类我们可能会补几个字符设备桩）；
- 不需要 `pthread_create` 之外的实时特性、不需要网络；
- 大小 ≤ 64 MB（内存里放得下）。

跑不了（明确写清楚，避免误导）：nginx、postgres、node 这类要 epoll/大量线程/`/proc`/网络栈
完整语义的东西，以及任何依赖 namespace 或 cgroup 的工具链。

---

## 9. 施工记录

（每完成一个里程碑补一节：做了什么、踩了什么坑、验收输出。）

### M11-0（ISO 启动链）已完成

**做了什么**：

- `port/x86_64/cd0.S`（744 B）：El Torito no-emulation 引导块。归一化 CS:IP → 初始化
  COM1 → 打印 mkiso 回填的载荷参数 → `int 13h AH=42h` 按 2048 字节块读内核载荷
  （目标段每块 +0x80，先试一次读完、失败退化成一块一块）→ 写引导信息块 `0x7000` →
  切 32 位保护模式、远跳 `0x10000`（内核 stage1 是 `.code32`）。
- `tools/mkiso.py`：写最小 ISO9660 + El Torito。布局确定：PVD@16、El Torito 卷描述@17、
  终止符@18、引导目录@19、引导块@20、内核载荷@21…、路径表 L/M、根目录（含 `C6OS.KER;1`）。
  回填引导块里 `C6ISOLDR` 魔数后面的 6 个 u32（内核/rootfs 的 LBA、块数、目标物理地址），
  生成完自己再解析一遍关键字段做自检。
- `tests/t_iso.c6` + `iso` 变体 + `make x86-iso / run-x86-iso / check-iso`。

**验收**：

```
$ make check-iso
  kernel: lba=21 blocks=54 (109496 B) -> 0x00010000
PASS: iso M11-0（El Torito no-emulation 启动链）

# 客户机内输出：
cd0: magic0=0x00000043 k=0x00000015/0x00000036 r=0x0000004b/0x00000000
cd0: payload loaded
cd0: enter protected mode -> 0x10000
c6os iso M11-0: 从 El Torito 光盘引导
  bootinfo magic=0x53493643 version=1 flags=1
  kernel pa=0x00010000 len=110592  rootfs pa=0x00000000 len=0

# 宿主侧旁证（ISO9660 结构合法，El Torito 引导项在）：
$ 7z l build/x86/iso/c6os.iso | tail -5
2025-01-01 08:00:00 .....  109496  109496  C6OS.KER
                .....        2048    2048  [BOOT]/Boot-NoEmul.img
```

**四个坑（按踩到的顺序）**：

1. **`.code16` / `.code32` 必须成对**：我在 `enter32` 之前写了 `.code32`，结果后面的
   `print`/`put_chr` 辅助例程全被按 32 位编码，16 位调用它们直接飞掉——现象是"一个字
   都打不出来"。
2. **清 PE 之后的远跳必须用 16 位编码**：`ljmp $0, $rm16` 若按 `.code32` 编成
   `EA off32 sel16`，回到实模式后 CPU 按 `off16+sel16` 译码，直接飞到不知道哪里。
3. **打印例程别把值留在 EAX 里**：`put_chr` 会把字符放进 AL，`put_hex` 每轮从 EAX
   重新取值得出的是被改过的值（现象：常数 `0x12345678` 打成 `0x12345633`）。
4. **`movl $0, dap+4` 会把 segment 一起清零**（DAP 里 offset 在 +4、segment 在 +6）：
   于是第一次读盘写到物理地址 0，把实模式中断向量表砸了，第二次 `int 13h` 跳进垃圾
   ——现象是"读一块成功（打了一个点），然后卡死"。

前三个都属于"模式/寄存器宽度"这类看不见的错，第四个是纯粹的眼瞎；
共同点是**都表现为"没有输出"**，所以 `cd0` 里那行参数打印和每个阶段的心跳很值钱。

### M11-1（ISO 内嵌只读 rootfs）已完成

**做了什么**：

- `cd0.S` 增加 `load_payload_hi()`：每 32 块（64 KB）读进 `0x80000` 暂存区，再
  `unreal_on` → `copy_flat`（32 位寻址 `movsl` + 字节尾巴）→ `unreal_off` 搬到
  `dst_pa`；每块进出一次 unreal mode，BIOS 调用始终在干净实模式段下。
- `cd0.S` 增加 **E820 收集**（原来只有 `boot16.S` 做，光盘路径不经过它）。
- `tests/t_iso_rootfs.c6`（`iso` 变体 + `C6OS_VFS=1`）：读引导信息块 → `port_phys_reserve`
  把 rootfs 那段从 PMM 里挖掉 → 剥掉 initramfs 的 8 字节长度前缀（512 字节头）→
  `LOS_FsMount("", "/", "romfs", &img)` → `LOS_FsReaddir/Stat/Open/Read` 走一圈。
- Makefile：`ISO_CPIO`（`tools/mk-initramfs.py` 打的小 rootfs）、`x86-iso-rootfs`、
  `run-x86-iso-rootfs`、`check-iso-rootfs`。两个 iso 目标都先 `rm -rf $(ISO_DIR)`——
  变体名都是 `iso`，main 和 `C6OS_VFS` 不同，不整目录清掉就会拿旧内核去跑新 ISO。

**验收**：

```
$ make check-iso-rootfs
  kernel: lba=21 blocks=66 (133616 B) -> 0x00010000
  rootfs: lba=87 blocks=1 (920 B) -> 0x00800000
PASS: iso M11-1（ISO 内嵌 rootfs：romfs 挂 /，readdir/stat/open/read 通）

# 客户机内：
cd0: magic0=0x00000043 k=0x00000015/0x00000042 r=0x00000057/0x00000001
cd0: payload loaded
cd0: enter protected mode -> 0x10000
c6os iso M11-1: ISO 内嵌 rootfs
  bootinfo magic=0x53493643 flags=1 rootfs pa=0x00800000 len=2048
mmu: e820=00000007 free_kb=0003ef80
  pmm reserved, free_kb=257884
  cpio body=408 head=070701
  romfs 挂到 /（LOS_FsMount）
  /etc-release  size=11
  /hello.txt  size=26
  stat /hello.txt size=26
  read: hello from the ISO rootfs
```

**坑**：

1. **unreal mode 的回程不能用"清 PE + 远跳"**：`ljmp $0, $rm16` 若编成 32 位 `EA`，
   实模式会按 `off16+sel16` 读错位直接飞。改成先跳 `0x18`（16 位代码段，D=0、base 0）
   进 PM 存根，再清 PE 继续跑，不用远跳。
2. **光盘引导路径没人收 E820**：`boot16.S` 只在盘引导时跑，`cd0` 不收就
   `e820=00000000` → `port_mmu_init` 失败。抄一份 `int 15h/0xE820` 的循环进 `cd0`。
3. **`tools/mk-initramfs.py` 的镜像有 512 字节头**（8 字节小端长度 + 504 字节 0），
   内核侧消费要跳过，否则 romfs 看到的是 `98 01 00 00…` 而不是 `070701`，mount 返
   `-EINVAL`。
4. **两个 iso 目标共用 `build/x86/iso/`**：`stage2.elf`/`flat.bin` 会被复用，换了 main
   或 `C6OS_VFS` 不整目录清掉，就会"新 ISO 跑旧内核"（现象：`check-iso` 里冒出
   M11-1 的 FAIL 行）。

### M11-2（可读写硬盘：FAT16 写）已完成

**做了什么**：

- `kernel/fatfs.c6` 从"只读"扩成读写：新增 `fat_write_`（扇区写，走 `LOS_BlockWrite`）、
  `fat_get_/fat_set_`（FAT 项读改，**所有 FAT 副本都写**）、`fat_alloc_`（first-fit
  找空闲簇 + 标链尾 + 清 0）、`fat_free_chain_`、`fat_dent_sec_/fat_dent_put_/
  fat_dent_update_`（目录项定位/写入/改大小）、`fat_dir_slot_`（空槽，子目录满了还能
  扩一簇）、`fat_add_dent_`（建文件/建目录，目录顺带写 `.` 和 `..`）、
  `fat_chain_grow_/fat_chain_trim_`（簇链增长/收缩）。
  打开的文件进一张 8 槽的 `FatFile` 表（`fh` 里放槽号+1），写回时才找得到目录项。
- 后端函数：`fatfs_open` 支持 `O_CREAT/O_EXCL/O_TRUNC`；`fatfs_write`（按需扩簇、
  逐扇区读改写、最后更新目录项大小）、`fatfs_truncate`（收缩还簇/增长补簇）、
  `fatfs_unlink`、`fatfs_mkdir`、`fatfs_rmdir`（非空目录返 `-ENOTEMPTY`）、
  `fatfs_rename`（返 `-EOPNOTSUPP`，8.3 改名不做）。
- `kernel/fsapi.c6`：`LOS_FsWrite/Unlink/Mkdir/Rmdir/Rename/Truncate` 加 `fatfs` 分支。
- `tests/t_fat_write.c6` + `fatw` 变体 + `make x86-fatw/run-x86-fatw/check-fat-write`。

**写盘顺序**（掉电一致性）：先写数据簇 → 再改 FAT → 最后改目录项；
FAT16 是写穿（每个扇区立刻 `LOS_BlockWrite`），所以 `LOS_FsSync` 对它就是空操作。

**验收**：

```
$ make check-fat-write
PASS: fat M11-2（/data 可读写：创建/写/读回/追加/截断/目录/删除）
PASS: fat M11-2 宿主侧（数据盘里能看到 D1/A.TXT）

# 客户机内：
c6os fat M11-2: /data 可读写（FAT16）
  /data: HELLO.TXT NEW.TXT D1/
PASS: fat M11-2（/data 可读写：创建/写/读回/追加/截断/目录/删除）
```

宿主侧那段是 `dd` 把 LBA 16384 起的 FAT 区抠成 `/tmp/fatw-check.img`，再用 `7z l`
列出来的——**写真的落到盘上了**，不是内存里糊弄。

**偏差与坑**：

1. **ops 分发表没做**：设计里说 M11-2 第一步是把 `fsapi.c6` 的"按 FS 名字 if/else"
   改成挂载点上的 `const LosFsOps *`。实际做法是**直接在原分派链里加 `fatfs` 分支**
   —— 重构要同时改 `vfs.c6`/`ramfs.c6`/`romfs.c6`/`fsapi.c6` 四个文件，风险和
   改动面都比"加两个分支"大，而这一步真正要的是"FAT 能写"。ops 表仍留在待办里
   （见 §7 之后），`fsapi.c6` 里所有分派已经集中在 `open_/close_/read_/stat_/
   readdir_` 和写类那几个函数里，将来替换点是清楚的。
2. **测试要自己注册块设备**：`LOS_FsMount("ata0", ...)` 之前得
   `LOS_DeviceRegister("ata0", 512, 32768, ata_rd, ata_wr)`，否则 mount 返 `-ENOENT`
   （M9 的 t_vfs 里也是这么做的，我一开始漏了）。
3. **验收盘必须每次重建**：`mkfat.py` 的镜像被 guest 写脏之后，第二次跑
   `mkdir /data/D1` 会 `-EEXIST`。`check-fat-write` 现在先 `rm -f` 掉
   `fatw-disk.img` 再重建，保证可重复。
4. **M9-3 的"FAT 只读"断言作废**：`tests/t_vfs.c6` 里原来用
   `LOS_FsOpen(..., O_RDWR) < 0` 证明只读，M11-2 之后这条必然失败，改成验能打开。

### M11-3（docker 镜像 → ISO → 跑起来）已完成

**做了什么**：

- `tools/mkdockeriso.py`：镜像 → ISO 的一条龙。
  - 三种输入：`--rootfs DIR`（`docker export | tar -x` 的产物）、`--from-tar`
    （单个 rootfs tar）、`--from-save`（`docker save` 整包：读 `manifest.json`
    拿 `Config` 里的 `Entrypoint`/`Cmd`，再按 `Layers` 顺序叠加）。
  - **whiteout 扁平化**：`.wh.名字` 删对应路径，`.wh..wh..opq` 清空所在目录；
    构建期就把 overlay 语义消解掉，运行期不需要 overlay。
  - 符号链接摊平成普通文件（没有真正的 link 语义，先这样）。
  - 往 rootfs 写 `/etc/c6os.cmd`（`ENTRYPOINT`/`CMD` 或 `--cmd` 覆盖）。
  - 调 `mk-initramfs.py` 打 newc cpio → 调 `mkiso.py` 生成 ISO（内核 + rootfs）
    → 调 `mkfat.py` 造可写数据盘。
- `tests/linux/docker_app.c`：镜像里的"应用"。**不用 libc**（`-nostdlib` 直接
  `syscall`），编出来 9 KB：open → write → close → open → read → exit_group。
  这样 rootfs 小到能整段放在 1MB 以下，引导块不用切 unreal mode。
- `tests/t_iso_docker.c6`：容器的 init。挂 `/`（镜像 rootfs，romfs）+ `/data`
  （FAT16）→ 打印 `/etc/c6os.cmd` → 把 `/bin/app` 读进 `0x04000000` →
  `port_as_create`/`lx_exec_prepare`/`port_user_enter` 进 ring3。
- `linux/fs.c`：`vfs_owns_()` 从"只认 /tmp"改成**问挂载表**
  （`os_vfs_lookup` 认就交给 VFS），`/data` 这样的新挂载不用再改这里。
- `linux/sys.c`：`LX_SYS_open` 参数顺序修正（见下）。
- `port/x86_64/cd0.S`：载荷目标 < 1MB 时走实模式逐块直读，≥ 1MB 才走 unreal mode。

**验收**：

```
$ make check-docker-iso
PASS: docker M11-3（docker 镜像 → ISO → 跑起来 + 写 /data）
PASS: docker M11-3 宿主侧（数据盘里有镜像程序写的 OUT.TXT）

# 客户机内：
c6os docker M11-3: 从 ISO 起一套镜像
  rootfs: pa=589824 len=9692 cpio=070701
  / = 镜像 rootfs（romfs），/data = FAT16 数据盘
  probe /data/probe.txt rc=0
  /etc/c6os.cmd: /bin/app
  /bin/app size=9080
elf: 主程序已装载
  elf 就绪，进用户态（镜像程序开始跑）
PASS: docker M11-3（镜像里的程序跑起来，写 /data/out.txt 并读回）
[lx] exit_group(0)
```

宿主侧把数据盘整个用 `7z l` 列一遍，能看到 `OUT.TXT`——程序写的文件真的在盘上。

**坑（这一节最长，都是真踩的）**：

1. **`mkfat.py` 第二个参数是 KB**，不是字节也不是扇区。传了 8388608 直接炸。
2. **rootfs 不能落在内核 .bss 区间**：`_start64` 一上来 `rep stosb` 清
   `__bss_start..__bss_end`（现在到 `0x72684`）。第一次把 rootfs 放 `0x60000`，
   引导阶段读进去了、内核一清 .bss 就全变 0，表现为"长度前缀 = 0"。改到
   `0x0090000`（.bss 之后、`0x9F000` 之前），并让 `mkdockeriso.py` 检查放不放得下。
3. **SeaBIOS 的 ATAPI 读不能连读多块**：`int 13h AH=42h` 一次读 54 块返回 CF，
   一次读 32 块第一块成功、**第二次调用直接挂死**（DRQ 等不到）。最终内核载荷、
   rootfs 全部改成**一次一块（2048 字节）**逐块读；原来那段"先试多块、失败回退"
   的优化删掉了。
4. **`load_payload` 重写时把 `fail:` 标签弄丢了**，`ld` 报
   `undefined reference to 'fail'`——汇编里 `jc fail` 是 rel8 跳转，标签没了就成
   未定义符号。
5. **unreal mode 多次进出还没修好**：>1MB 的载荷按块"读一块→切 unreal 搬一块→
   切回来"时，第二块的 `int 13h` 挂死（第一块成功）。M11-3 用"rootfs 放 1MB
   以下"绕开，`load_payload_hi` 保留给以后大 rootfs 用，属于已知限制
   （见 §6 风险表）。
6. **`LX_SYS_open` 一直是错的**：`sys.c` 把 `open` 和 `openat` 都按
   `openat(dirfd, path, flags, mode)` 派发，而 `open(path, flags, mode)` 少了
   dirfd——glibc 的 `open()` 走的是 `openat`，所以 M9 那套 libc 程序从没踩到。
   这次裸 syscall 一用就露馅，补成 `lx_fs_openat(AT_FDCWD, path, flags, mode)`。
7. **`vfs_owns_()` 只认 `/tmp`**：Linux ABI 判断"这个路径归不归 VFS"是硬编码的，
   `/data` 挂了也进不去。改成查挂载表。
8. **`-nostdlib` 还要加 `-fno-builtin`**：自己写的 `slen()` 被 GCC 认成 `strlen`
   习惯用法，直接换成对 `strlen` 的调用，链接期报 undefined。
9. **数据盘起始 LBA 两套**：`fatw` 变体把 FAT 镜像 `dd` 到 LBA 16384（盘上还有
   initramfs），M11-3 的数据盘是**整块一个 FAT 卷**（LBA 0）。挂载参数别抄错。
10. **程序不能用 `return` 从 `_start` 返回**（没有返回地址，直接 sig=11），
    必须 `exit_group`。

### M12（真实 Alpine minirootfs）已完成

M11-3 跑的是自造的 9 KB 程序。M12 换真的：Alpine 3.20 官方 minirootfs，
`/bin/busybox` 是**动态 PIE**（808712 B，`INTERP=/lib/ld-musl-x86_64.so.1`），
解释器 649832 B，cpio 514 个成员、334 个符号链接。

**做了什么**：

- `Makefile`：`x86-alpine` / `run-x86-alpine` / `check-alpine`。minirootfs（3.5 MB）
  只在缓存里没有时从 `dl-cdn.alpinelinux.org` 下到 `build/x86/alpine/`；解开 →
  newc cpio → `mkiso.py` 打 ISO；`mkfat.py` 造 8 MB FAT16 当 `/data`。
- `tests/t_iso_alpine.c6`：minirootfs 挂 `/`、FAT16 挂 `/data`，`lx_tty_init()` 把
  0/1/2 变成串口终端，再 `lx_exec_preparev(..., kAv, 3)` 跑 `/bin/sh -c '<脚本>'`。
  脚本四件事：写终端、`uname -a`、`ls /`、写 `/data` 再读回。
- `linux/tty.c`（新）：串口行规程。串口是轮询的，所以走"每次系统调用入口
  `lx_tty_pump()` 泵一次 + `read` 阻塞轮询"；输出侧要应答 `ESC[6n`（busybox ash
  靠它测光标位置），不回的话 ash 不吐提示符。
- `linux/sys.c`：把 ash 真跑起来缺的系统调用补齐 —— `stat(4)`/`lstat(6)`、
  `nanosleep(35)`、`clock_gettime(228)`、`getpgid(121)`、`ioctl(TIOCGPGRP/TIOCGWINSZ)`。
- `linux/fs.c`：`FS_MAX_FILES` 64 → 1024、`FS_NAME` 64 → 96、加 `FS_LINK_MAX`；
  路径归一化（`norm_()` 塌掉 `.`/`..`/重复斜杠）+ 符号链接解析 —— 发行版 rootfs 的
  链接目标里有 `..`，不归一化就拼出查不到的路径。
- `tools/mk-initramfs.py`：悬空符号链接不再 `FileNotFoundError`（按 `S_IFLNK` 写）。
- `port/x86_64/cd0.S`：3.84 MB 的 cpio 放不到 1 MB 以下，走 unreal mode 的
  `load_payload_hi`，一次一块 2048 字节。

**验收**：

```
$ make check-alpine
PASS: alpine M12（Alpine minirootfs 从 ISO 起起来：sh -c 脚本跑完 + 写 /data）
PASS: alpine M12 宿主侧（数据盘里有 m12.txt）
```

串口上依次是：

```
alpine-M12-alive
Linux c6los 6.1.0-c6los #1 x86_64 Linux
bin  etc  lib  ...  usr
m12ok
alpine-M12-done
```

**坑：c6 的静态指针初始化器是空的（这次出在编译器上）**

M12 一开始卡在"脚本一个字都不输出"。查下来是 busybox 拿到的 `argv[1]` 不是 `-c`：
c6 的 LLVM IR 后端把数据段发射成一串字节，**指针槽留 0**，再靠挂在
`llvm.global_ctors` 上的 `@__llvm_init` 在启动时补。带 C 运行时的程序看不出问题；
c6LOS 的内核不跑构造函数（没有 crt），所以

```c
static const char *const av[] = { "/bin/sh", "-c", script };
```

这三个槽永远指向地址 0，取出来是野指针。必然踩到它的正是"函数内 static 的
argv 表"这种写法。

修在编译器侧（c6 `07d73b8`）：带重定位的数据段发射成 LLVM 的 **packed struct**，
指针槽是 `ptrtoint` 的真静态初始化，目标文件里就是普通重定位，加载即有效。
wasm 例外 —— wasm 没有函数符号的绝对地址，后端直接拒绝静态初始化器里的
`ptrtoint`（`Unsupported expression in static initializer`），那边保持构造函数
路径。回归用例在 c6 仓库：`tests/llvm/static_data.c6` + `regression.sh
llvm-static-data`，不带任何 C 运行时链接一个裸 `_start`，验文件作用域与函数内
static 两种指针表。M12 这边就改回原生的 `static const char *const kAv[] = {...}`，
`lx_exec_prepare_sh` 那个包装函数随之删掉 —— 它当初存在只是为了绕开这个 bug。

### M12 补充：自己敲命令（交互式 shell）

验收脚本是 `sh -c '固定脚本'`，因为"等人输入"在自动化里没法复现。但真想体验时
还是要能自己敲。这条路踩了两个坑，都属于"看上去根本不相关"的那种：

1. **`poll` 不阻塞（真正的根因）**。`lx_proc_poll` 原来的循环有个
   `rounds >= 4096u` 的保护，轮够次数就 `return cnt`（通常是 0）。它本来是给
   "协作式调度里的管道"防死等加的，对终端却是错的：busybox 的 `read` 内置命令
   是**先 poll 再 read**，拿到 0 就当 stdin EOF，于是提示符 `/ #` 一打出来 shell
   就 `exit_group(0)`；lineedit 的 DSR 等待也死在同一处（poll 返回 0 → 以为终端
   不回话 → 退出）。改法：超时为负就真阻塞（空转泵串口 —— 终端输入不来自别的
   进程，没有谁会来唤醒我们，所以**不能** `lx_proc_block()`），为正则按
   `port_cycles()` 到点返回；纯管道仍然只在"还有别的进程活着"时才阻塞。

   定位过程值得记一笔：先怀疑 lineedit 的 `ESC[6n` 握手（回与不回都退），又怀疑
   `TCSETS`，都不对；最后把系统调用号全量打出来，看到 `read -r x` 在
   `A-probe` 和 `GOT:` 之间**只发了一次 poll、一次 read 都没有**，才锁定到 poll。

2. **`TCSETS` 收下了但没用**。行规程原来固定 canonical+echo，而 busybox 的
   lineedit 会 `tcsetattr` 切 raw 模式自己回显 —— 结果命令被回显两遍。现在
   `TCSETS` 记住 `c_lflag`：`ICANON` 关掉时字节直接透传（不再攒行/退格），
   `ECHO` 关掉时不再回显。`TCGETS` 回的也是程序 `tcsetattr` 过的值，不再是硬编码
   常量，否则 `tcgetattr`→改→`tcsetattr` 这种写法读到的是假状态。

用法：

```
$ make run-x86-alpine-shell
/ # ls /
bin  etc  lib  lib64  media  mnt  opt  proc  root  run  sbin  srv  sys  tmp  usr  var
/ # uname -a
Linux c6los 6.1.0-c6los #1 x86_64 Linux
/ # cat /etc/alpine-release
3.20.0
```

已知没做：挂载点本身的 `stat`/`readdir`（`ls /data` 报 No such file，但
`echo m12ok > /data/m12.txt` 与 `cat /data/m12.txt` 都是好的），以及 `sendfile`
（`cat` 时能看到 `[lx] ENOSYS nr=40`）。

### M12 补充：cd / pwd

`cd /bin` 之后 `ls` 列的还是根目录。三个地方凑成了这个现象：`LX_SYS_chdir` 在
sys.c 里跟 `setsid`/`umask` 一起被归成"L6+ 再说"的空操作（直接回 0）；`getcwd`
死写 `"/"`；而相对路径（`ls` 不带参数就是 `.`）在 fs.c 里被 `strip_()` 去掉前导
斜杠后**直接当根路径**查 —— 即便 cwd 变了也没人用它。

现在 fs.c 有 `g_cwd`（不带前导 `'/'`，空串=根）和 `path_()`：相对路径先拼 cwd 再
`norm_()` 归一化，`openat` / `statat` / `access` 三处用户路径入口都改走它。
`lx_fs_chdir()` 真的查节点、确认是目录、再用节点名当规范路径写回 `g_cwd`；
`lx_fs_cwd()` 给 `getcwd` 拼 `"/" + cwd`。

cwd 一开始是**内核里的一份**，随即被 `top` 打脸：busybox 的 top 自己会
`chdir("/proc")`，它一退出，父 shell 的 cwd 就跟着进了 /proc，`ls` 于是列出一张
空表（`pwd` 还显示 /proc —— rootfs 里那个 /proc 是真空目录）。现在 cwd 跟着
进程走：`LxProc.cwd[]`，进程切换时在换 fd 表的同一个地方调
`lx_fs_set_cwd(p->cwd)`，fork 时子进程继承父进程的 cwd。
