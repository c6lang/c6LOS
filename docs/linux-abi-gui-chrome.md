# Linux ABI GUI 支持与 Chrome 运行（G 线）详细设计

> 状态：**设计稿 R1（已自评审，待代码 owner 评审后开工）**。本文把"Linux ABI
> 支持 GUI，最终让 Chrome 跑起来"拆成可验收的 G0–G8 里程碑，并给出测试先行
> （TDD）的施工顺序。评审记录见附录 C。
> 关联：[linux-abi.md](linux-abi.md)、[linux-abi-complete.md](linux-abi-complete.md)、
> [linux-abi-portable.md](linux-abi-portable.md)、[maixcam-bringup.md](maixcam-bringup.md)、
> [todo.md](todo.md)。

> 施工方式：独立分支 `feat/linux-abi-gui-chrome`（从 `main` 切出），
> 每个里程碑一个提交；先提交失败测试（RED），再提交实现（GREEN），
> 验收数字写进提交信息。未通过本文 §8 的验收矩阵前不合并 `main`。

---

## 0. 目标与口径

### 0.1 什么叫"Chrome 能跑起来"

"能跑"按五级定义，每级都有独立、可复现的验收；前一级不退才能进下一级：

| 级别 | 含义 | 验收 |
|---|---|---|
| **C0 可执行** | Alpine 的 Chromium 二进制能加载、`--version` 正常退出 | `chromium --version` 打印版本号，退出码 0 |
| **C1 headless 可用** | 无显示服务器，完整浏览器进程树能跑 Skia 软件渲染并输出 DOM/截图 | `chromium --headless=new --dump-dom` 输出预期 DOM；`--screenshot` 产出合法 PNG |
| **C2 屏上有画面** | 通过 fbdev 在 QEMU 屏上看到 Chromium 渲染的页面（kiosk 路线） | QEMU `screendump` 校验页面色块/文字区域；按键/点击能改变页面 |
| **C3 原生 GUI** | 最小 Wayland 合成器 + `--ozone-platform=wayland`，Chrome 窗口正常显示与输入 | 合成器上跑 Wayland 演示客户端 PASS；Chrome 窗口截图非空；键盘/鼠标事件生效 |
| **C4 可日常演示** | 本地页面连续运行 30 分钟不崩、内存回基线、输入流畅 | `check-linuxabi-g5` 长稳脚本 + 内存/帧率阈值 |

**本设计只承诺 C0–C4；不承诺 GPU 加速、WebGL、音视频硬解、多标签重负载。**

### 0.2 目标二进制与平台

- **目标二进制**：Alpine 3.20 community 的 `chromium`（Chromium 131.x，musl 动态链接，
  安装后约 243 MB、约 70 个直接依赖）。不选 Google 官方 Chrome deb：它依赖
  glibc/NSS 打包链，会把问题从"GUI/内核能力"变成"发行版兼容"。
- **目标架构**：`port/x86_64`（当前唯一能让 x86-64 Linux ELF 跑起来的 port）。
  RV64/SG2002 没有可用的 Chrome 二进制，**不在本线范围**；S3 只能复用本文的
  设备层/合成器设计（见 G8，可选）。
- **目标机器**：QEMU `-M pc -m 3072 -cpu max -vga std`（VBE LFB），
  2–3 GB 物理内存、512 MB 以上数据盘、rootfs 约 400 MB。
- **显示**：QEMU 标准 VGA 的 VBE 线性帧缓冲（32bpp XRGB8888 / 16bpp RGB565），
  后续可换 virtio-gpu；S3 复用 240×240 RGB565 面板通路。

### 0.3 非目标（本线明确不做）

- 硬件 GPU（QEMU 的 virtio-gpu 3D、S3 无 GPU）、EGL/GLES/WebGL 硬件加速；
- 音频输出/采集、摄像头、视频硬解（Chrome 的 `<video>` 可后续接 G8 之外的线路）；
- Chrome sandbox（namespace/seccomp/user namespace）：一律 `--no-sandbox`；
- 多用户、X11、Wayland 全协议、桌面环境、窗口管理器；
- S3/MaixCam 上跑 Chrome（架构不同）；DRM/KMS 只作为 G7 可选项。

### 0.4 TDD 施工规则（本线强制）

1. **先有失败测试**：每个里程碑第一个提交只加测试、测试夹具和 `check-*` 目标；
   在当前树上执行必须得到明确的 `FAIL: ...`（不是构建错误）。
2. **再写实现**：第二个提交让同一测试变 `PASS`；测试文件不允许在实现提交里改弱。
3. **验收脚本自身先失败**：`tools/check-linuxabi-gN.sh` 在实现前必须能跑完并报 FAIL
   （例如 `ENOTTY`/`ENOSYS`/`No such device`），不能"因为没有二进制而跳过"。
4. **分层测试**：纯逻辑（JSON、base64、fb 结构布局、VMA 记账）放 host 单测
   `tests/host/`；内核语义放 `tests/linux/*.c` 在 QEMU 里跑；UI 结果用 QEMU
   `screendump` + Python 像素断言。
5. **每级不退**：G 线新测试并入 `tools/check-linuxabi-all.sh` 后，
   `make check-linuxabi-all` 必须保持全绿（历史项只允许因本线修复而更绿）。

> 完整测试架构、目录、用例清单、RED/GREEN 提交协议和需求追溯矩阵见 §7；
> 本节只是总则，§7 是每个里程碑开工前必须逐条执行的检查表。

---

## 1. 现状盘点（代码实测，不是推测）

### 1.1 Linux ABI 已经有什么

| 能力 | 现状 | 位置 |
|---|---|---|
| syscall 面 | **189 个 `LX_SYS_*` 定义、约 187 个 `case`**，覆盖进程/信号/线程/fd/VFS/网络/事件/时间 | `include/c6os/linux_abi.h`、`linux/sys.c` |
| ELF 装载 | ET_EXEC/ET_DYN、PT_INTERP、动态链接、auxv、TLS(fs_base)、shebang | `linux/elf64.c`、`linux/proc.c` |
| 进程/线程 | 动态进程表（4096 上限）、动态 fd 表（1024）、`clone(CLONE_VM)` 线程、COW fork、64 信号、抢占式调度 | `linux/proc.c`、`linux/sig.c` |
| 事件 | `poll/select/epoll/eventfd/timerfd/signalfd` 已有 | `linux/proc.c`、`linux/sys.c` |
| 网络 | AF_INET UDP/TCP、AF_UNIX path socket、`sendmsg/recvmsg`、SCM_RIGHTS、DNS | `linux/socket.c`、`linux/afunix.c` |
| Rootfs | newc cpio initramfs（只读）+ FAT16 `/data`（8 MB） | `linux/fs.c`、`tools/mk-initramfs.py`、`tools/mkfat.py` |
| 裸机图形 | `rt/gfx.c` 的 `OsGfx`、S3 240×240 RGB565 面板 + SPI/GDMA blit | `rt/gfx.c`、`port/s3/panel.c` |

已验收证据：Alpine busybox/ash、musl libc-test、真实 `curl` HTTPS、`ping`、
TCP/UDP 全链路；见 `docs/linux-abi.md`、`docs/linux-abi-complete.md`。

### 1.2 与 Chromium 的差距（P0 = 不修就跑不起来）

| # | 需求 | 现状（证据） | 差距 | 优先级 |
|---|---|---|---|---|
| A1 | futex 按地址等待/唤醒、超时、bitset、robust | `linux/proc.c:4167 lx_futex`：`WAKE` 唤醒**所有** `LX_P_BLOCK` 进程；不按地址；`utime` 直接 `(void)` 丢弃；无 bitset/requeue | 重写等待队列（按物理地址哈希）、超时、`FUTEX_WAIT_BITSET/WAKE_BITSET`、`REQUEUE`、robust list | **P0** |
| A2 | 共享内存对象 `memfd_create(319)` + seals | 无定义、无实现；`fcntl` 无 `F_ADD_SEALS` | 新增 `LxShm` 对象、`ftruncate`、`mmap` 共享映射、`F_ADD_SEALS/F_GET_SEALS` | **P0** |
| A3 | `MAP_SHARED` 真共享、fork 不 COW 共享页 | `linux/sys.c:281 sys_mmap2` 对文件映射逐页 `port_phys_alloc + pread` 私有拷贝；`LX_VM_SHARED` 只是记账；`msync` 空操作 | 共享对象映射、`port_as_copy` 跳过 `LX_VM_SHARED`、页引用计数 | **P0** |
| A4 | `mremap(25)` | 未定义、未实现 | 至少支持 `MAYMOVE`/`FIXED` 的扩/缩/搬移（musl `realloc`、V8 需要） | **P0** |
| A5 | `madvise` 真语义 | `linux/sys.c:1429` 直接返回 0；`MADV_DONTNEED/MADV_FREE` 不释放页 | 实现匿名页释放；其余返回 0 | P1 |
| A6 | 文件 mmap 零拷贝 | `sys_mmap2` 每个 4 KB 页分配新页 + `lx_fs_pread_k` 拷贝；加载 150 MB Chrome/资源时 CPU/内存翻倍 | initramfs 文件页直接映射（只读、引用计数；写时 COW） | **P0** |
| A7 | initramfs 节点容量 | `linux/fs.c:19 FS_MAX_FILES 1024` 静态 `LxNode g_node[1024]`；超出**静默丢弃** | 动态节点表 + 哈希索引；Chrome 依赖包 > 10k 文件 | **P0** |
| A8 | rootfs 容量/加载速度 | `ROOTFS_DST=0x00800000`，`cd0.S` 用 `int 15h AH=87` **逐 2 KB 块**搬；7.8 MB 要 3755 次 BIOS 调用（几十秒） | 300–400 MB rootfs 改走独立磁盘（ATA PIO/`os_blkdev`），或改批量加载 | **P0** |
| A9 | `/proc/self/fd/N` | procfs 只有 stat/status/cmdline/uptime 等；无 fd 目录 | 新增 `/proc/self/fd` 目录 + 打开即 dup 的语义（memfd 只读副本依赖） | **P0** |
| A10 | `/proc/meminfo`、`sysinfo()` 真实内存 | `linux/fs.c:1053` 固定 `MemTotal 24576 kB`；`lx_proc_sysinfo` 固定 24 MB/12 MB | 接 `port_phys_total_kb/free_kb`，并支持 >1 GB | **P0** |
| A11 | 物理内存 >1 GB | `port/x86_64/mmu.c:43 PMM_MAX_FRAMES=262144`（1 GB），E820 上限硬截 1 GB；`-m 256M` | PMM 支持 2–8 GB（含 4 GB 以上的 E820 条目）；位图移出 `.bss`（内核镜像必须 <0xA0000） | **P0** |
| A12 | TCP/SAL socket 容量 | `include/los/sal.h:28 LOS_SAL_MAX_SOCKETS 16`；`kernel/net/tcp.c6:20 TCP_MAX_SOCKS 4` | Chrome 网络进程同时开多条连接；动态扩到 128–256 | **P0** |
| A13 | `sched_setaffinity(203)` | 无定义、无实现（只有 `sched_getaffinity`） | 补实现（单核返回 1 即可，但要能写回 cpumask） | P1 |
| A14 | `clone3(435)`、`close_range(436)`、`pidfd_open(434)`、`faccessat2(439)` | 未定义 | glibc 回退路径可用；musl 不依赖；可后补 | P2 |
| A15 | `inotify` | 无 | Chrome `FilePathWatcher` 可选；缺失只降功能 | P2 |
| A16 | `O_TMPFILE`、`O_PATH`、`O_NOFOLLOW` | `linux_proc.h` 只定义 8 个 `LX_O_*` | 视 Chrome 实际 strace 补；`O_TMPFILE` 可能是共享内存回退路径 | P1 |
| A17 | `prctl` 真语义 | `linux/sys.c:1430` 一律返回 0 | 至少存 `PR_SET_NAME` 的 comm、`PR_SET_DUMPABLE`、`PR_SET_NO_NEW_PRIVS` | O1 |
| A18 | `getrandom` 质量 | LCG（`g_dev_seed` 算法） | Chrome 用于 ASLR/salt；接 RDRAND 或 TSC 混合 | P2 |
| A19 | 时钟精度 | `clock_gettime` 只分辨 1 ms；只认 `CLOCK_MONOTONIC=1`/其余全走 REALTIME | 补 `CLOCK_MONOTONIC_RAW/BOOTTIME/PROCESS_CPUTIME_ID`；ns 级 | P1 |
| A20 | `AF_UNIX` 抽象命名空间 | `linux/afunix.c` 头注释明确不做抽象名字（`\0`） | Chrome 主要用 `socketpair`；D-Bus 可用路径 socket；可选补 | P2 |
| A21 | POSIX 记录锁 `F_SETLK/F_GETLK/F_SETLKW` | `lx_proc_fcntl` 只支持 `F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL`，default `EINVAL` | Chrome profile 的 SQLite/LevelDB 初始化需要；实现按 `LxNode`+范围的内存锁表，冲突返回 `EAGAIN`/`EACCES`，`F_SETLKW` 阻塞 | **P0**（profile 必需） |

### 1.3 GUI 相关现状：Linux ABI 下一个像素都没有

| 项目 | 现状 | 结论 |
|---|---|---|
| `/dev/fb0` | `dev_node_()` 只认 `dev/null/zero/full/random/urandom/ptmx`（`linux/fs.c:955`） | 不存在 |
| `ioctl` | 只分派 socket / tty / pty / console（`linux/sys.c:1160` 起）；其余 `-ENOTTY` | 无 fb/evdev ioctl |
| `mmap` 设备页 | `sys_mmap2` 只有 anon 和"文件内容拷贝"两条路；`port_as_map` 不支持 UC/设备页标记 | 无 |
| `/dev/input/event0` | 无输入子系统；只有串口 tty | 无 |
| 显示服务器 | 无 X11、无 Wayland、无 DRM | 无 |
| x86 帧缓冲 | `boot16.S`/`cd0.S` 只有 E820 + int13 读盘，**从未设置 VBE/GOP 模式**；所有 QEMU 目标都是 `-nographic -serial stdio` | 无 |
| x86 MMIO 访问 | `port/x86_64/mmu.c:450 build_dmap` 把物理 1–4 GB 按 2 MB UC 页映射到 `0xFFFF8000_00000000`；e1000 的 BAR 就这么访问 | 有基础，可直接映射 VBE LFB |
| S3 图形 | `rt/gfx.c OsGfx` + `port/s3/panel.c` 240×240 RGB565、SPI/GDMA；`port/s3` 的 `port_caps()=0`，Linux ABI 解析期直接拒绝（`linux-abi-portable.md` §4.2） | 只能复用驱动，不能直接跑 Chrome |

### 1.4 人为资源上限（G 线要拆掉的）

| 上限 | 现值 | 位置 | 目标 |
|---|---|---|---|
| 物理内存 | 1 GB | `port/x86_64/mmu.c:43` | 8 GB 物理地址空间（QEMU `-m 3072` 全量可用） |
| initramfs 节点 | 1024 | `linux/fs.c:19` | 动态（>64k） |
| rootfs | 7–8 MB（0x800000 起） | `tests/t_iso_alpine.c6`、`port/x86_64/cd0.S` | 400 MB 磁盘 rootfs |
| 进程/线程 | 4096 | `LX_PROC_MAX_CAP` | 保持（Chrome 约 30–100 线程） |
| fd | 1024 | `LX_FD_MAX_CAP` | 保持（Chrome 默认 soft limit 1024） |
| TCP socket | 4 | `kernel/net/tcp.c6` | 256 |
| SAL socket | 16 | `include/los/sal.h` | 256 |
| `/data` | 8 MB FAT16 | `Makefile ALPINE_DATA` | 512 MB–2 GB |

---

## 2. 总体架构

### 2.1 分层

```
┌────────────────────────────────────────────────────────────────────┐
│ Chromium 131 (Alpine/musl, Ozone)                                  │
│   --headless=new（C1/C2）   或   --ozone-platform=wayland（C3/C4）  │
└───────────────┬───────────────────────────┬────────────────────────┘
                │ CDP over pipe（C2）        │ Wayland（C3）
┌───────────────▼───────────┐   ┌───────────▼────────────────────────┐
│ c6chrome-display（kiosk） │   │ c6wl（最小 Wayland 合成器）        │
│  JSON/base64/JPEG/screencast│  │  wl_compositor/wl_shm/xdg_shell/seat│
│  → /dev/fb0  + evdev      │   │  自写 ARGB 合成 → /dev/fb0          │
└───────────────┬───────────┘   └───────────┬────────────────────────┘
                └──────────────┬────────────┘
┌───────────────────────────────▼────────────────────────────────────┐
│ Linux ABI 设备层（G2）                                             │
│  OsFileOps 分发表：fb0 / evdev / null / zero / full / urandom / ptmx│
│  ioctl(FBIO*, EVIOC*) + 设备 mmap + poll + 阻塞唤醒               │
└───────────────────────────────┬────────────────────────────────────┘
                                │ port 层
┌───────────────────────────────▼────────────────────────────────────┐
│ x86_64：VBE LFB（QEMU -vga std，0xE0000000 级 BAR，DMAP UC 访问）   │
│ S3（G8 可选复用）：240×240 RGB565 + SPI/GDMA                         │
└────────────────────────────────────────────────────────────────────┘
```

### 2.2 三条显示路线对比

| 路线 | 组成 | 优点 | 缺点 | 采用 |
|---|---|---|---|---|
| **A. kiosk：headless + CDP 桥** | `chromium --headless=new --remote-debugging-pipe` + `c6chrome-display` | 不依赖显示服务器；Chrome 自身就是软件合成器；先把"屏上看到 Chrome"跑通 | CDP 每帧 JPEG 编码/解码，5–15 fps；非原生窗口语义 | **G3 首选** |
| **B. 最小 Wayland 合成器** | 自包含 Wayland wire protocol + 自写 ARGB 合成（不链接 libwayland-server/pixman）；fbdev/evdev 后端；备选 libwayland-server | 原生 GUI、窗口/输入语义；Chrome 官方支持 `--ozone-platform=wayland`；无 musl/glibc 库链接问题 | 自研协议 2–3k 行；需要 memfd/MAP_SHARED/seals 正确 | **G4/G5 主线** |
| C. X11/Xorg | Xorg + xf86-video-fbdev | 生态成熟 | 需要 VT 子系统（`VT_*` ioctl、/dev/tty0、KDSKBMODE）——本内核没有，移植成本高 | 不做 |

> **为什么不用 DRM 打头**：Chromium 的 `ozone-platform=drm` 依赖 GBM/EGL（即使
> `--disable-gpu` 也常回落到 EGL surfaceless），而本 ABI 没有 GEM/DRM。DRM 先作为
> G7 的"合成器后端优化"，不是 Chrome 的前置。

### 2.3 推荐施工顺序

```
G0 底座（rootfs/内存/procfs）
 ├─► G1 Headless Chromium（futex/memfd/mmap/mremap）
 ├─► G2 fb0 + evdev（与 G1 并行，互不阻塞）
 │     └─► G3 kiosk 桥（C2）
 └─► G4 Wayland 合成器（可在 G2 后并行）─► G5 Chrome on Wayland（C3/C4）
                                              └─► G6 性能收口
                                                    └─► G7 DRM/GPU（可选）
G8 S3/MaixCam 复用（可选，前置 V3/V4）
```

---

## 3. 设备层设计（G2）

### 3.1 设备对象模型

现状 `OsFile` 只有 `void *fh`，没有 ops；`linux/fs.c` 按 `kind` 硬编码。
引入与平台无关的 `OsFileOps`（形状沿用 `linux-abi-portable.md` §6）：

```c
typedef struct OsFileOps {
    ssize_t (*read)(OsFile *f, void *buf, size_t n);
    ssize_t (*write)(OsFile *f, const void *buf, size_t n);
    int     (*poll)(OsFile *f, u32 events);
    int     (*ioctl)(OsFile *f, u32 req, u64 arg);
    int     (*mmap)(OsFile *f, LxVma *vma);      /* ABI 私有，见 3.2 */
    int     (*close)(OsFile *f);
} OsFileOps;

typedef struct {
    const char *name;          /* "fb0" / "input/event0" */
    const OsFileOps *ops;
    void *priv;
    u32 refs;
} LxDev;
```

- `dev_node_()`（`linux/fs.c:955`）从"字符串 → `LX_DEV_*` 哨兵"改成
  "字符串 → `LxDev` id"，内建 null/zero/full/urandom 用简单 ops 实现；
- `LxFile` 增加 `u32 dev`（设备 id）；`kind=LX_FD_DEV` 时 read/write/poll/ioctl/mmap
  全部走 `ops`；
- `ioctl` 分派在 `linux/sys.c` 的 `LX_SYS_ioctl` 里加：
  `if (f->kind == LX_FD_DEV && dev->ops->ioctl) return ops->ioctl(...)`；
- `close` 释放 `LxDev` 引用；`fork/dup` 按 fd 复制引用。

### 3.2 `/dev/fb0`

#### ABI 结构（x86_64 Linux 布局，必须逐字节一致）

```c
struct fb_bitfield { u32 offset, length, msb_right; };      /* 12 B */

struct fb_var_screeninfo {                                  /* 160 B */
    u32 xres, yres, xres_virtual, yres_virtual;
    u32 xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    u32 nonstd, activate, height, width, accel_flags;
    u32 pixclock, left_margin, right_margin, upper_margin, lower_margin;
    u32 hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    u32 reserved[4];
};

struct fb_fix_screeninfo {                                  /* 80 B */
    char id[16];
    u64 smem_start;
    u32 smem_len, type, type_aux, visual;
    u16 xpanstep, ypanstep, ywrapstep;
    u32 line_length;
    u64 mmio_start;
    u32 mmio_len, accel, capabilities;
    u16 reserved[2];
};
```

#### ioctl 集（最小可用）

```c
struct c6_fb_rect { u32 x, y, w, h; };
#define C6FBIOFLUSH _IOW('F', 0x10, struct c6_fb_rect)   /* = 0x40104610 */
```

| 请求 | 值 | 语义 |
|---|---|---|
| `FBIOGET_VSCREENINFO` | `0x4600` | 返回当前 `fb_var_screeninfo` |
| `FBIOPUT_VSCREENINFO` | `0x4601` | 只接受同分辨率/同 bpp；否则 `EINVAL` |
| `FBIOGET_FSCREENINFO` | `0x4602` | 返回 `fb_fix_screeninfo`（`smem_start=fb_pa`、`smem_len`、`line_length`、`visual=FB_VISUAL_TRUECOLOR`） |
| `FBIOBLANK` | `0x4611` | `FB_BLANK_UNBLANK/NORMAL` 返回 0，其余 0 |
| `C6FBIOFLUSH`（私有） | `0x40104610` | 参数 `{x,y,w,h}`，把脏矩形刷到面板（S3 必需；x86 LFB 可空操作） |

#### mmap 语义

- `mmap(NULL, smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)` 返回 fb 的
  **同一批物理页**，不是拷贝；
- x86 VBE LFB 是 MMIO：用新增的 `C6OS_PROT_DEV`（0x100）`| C6OS_PROT_UC`
  （0x200）映射（PTE 置 `PCD|PWT` 和软件位 `PTE_DEV`，且 **不参与 PMM 引用计数**）；
- S3/DRAM fb 是 PPM 分配的连续页：驱动持有一份引用，用户映射逐页
  `port_phys_ref`，unmap 时 `port_phys_put`；若面板由 GDMA 读 fb，用户写完后
  需要 `C6FBIOFLUSH` 或 `msync(MS_SYNC)` 触发 cache 清理 + blit。

#### `port_as_map` 需要的改动（关键坑）

现状 `port/x86_64/mmu.c:86 port_as_map()` 对 `paddr==0` 分配页，对外部 paddr
不增加引用；而 `unmap_leaf_()`（同文件）对**任何**带 `PTE_U` 的叶子都
`port_phys_put()`。直接映射 fb 的物理页会在 unmap/进程退出时把驱动的帧缓冲
"释放"进 PMM。修法（三选一，推荐第一个）：

1. 新增软件位 `PTE_DEV`（bit 10）：`port_as_map(..., C6OS_PROT_DEV)` 置位；
   `unmap_leaf_()`/`port_as_destroy()` 看到 `PTE_DEV` 跳过 `port_phys_put`；
   `port_as_copy()` 对 `PTE_DEV` 页原样共享、不置 COW。**只用于 MMIO（VBE LFB）**；
   S3 的 DRAM 帧缓冲、memfd/wl_shm 页是普通 PMM 页，必须走 `PTE_SHARED`（bit 11）
   + 逐页 `port_phys_ref` + unmap put 的引用计数路径，不能混用 `PTE_DEV`。
2. 映射前逐页 `port_phys_ref()`、unmap 的 put 正好抵消；但驱动自己的引用要
   长期持有，容易漏。
3. 设备页一律不挂在用户页表，走 `write()` 拷贝；性能不可接受，不采用。

### 3.3 `/dev/input/event0`

#### ABI

```c
struct input_event {              /* 24 B（x86_64） */
    struct timeval time;          /* 16 B：u64 sec + u64 usec */
    u16 type, code;
    s32 value;
};
```

- 事件类型：`EV_SYN=0`、`EV_KEY=1`、`EV_REL=2`、`EV_ABS=3`；
- 常见 code：`KEY_A=30`…、`BTN_LEFT=0x110`、`BTN_TOUCH=0x14a`、`ABS_X=0`、`ABS_Y=1`；
- `read()`：无事件则阻塞（`lx_proc_block` + 环形缓冲 + `lx_proc_wake`），
  非阻塞返回 `EAGAIN`；
- `poll/epoll`：有事件报 `POLLIN`；
- ioctl：`EVIOCGVERSION`（0x80044501）、`EVIOCGID`（0x80084502）、
  `EVIOCGNAME(len)`（0x80xx4506）、`EVIOCGBIT(ev,len)`（0x80xx4520+ev）、
  `EVIOCGABS(abs)`（0x80xx4540+abs）。`EVIOCGNAME` 返回 `"c6os virtual keyboard"` 等。

#### x86 驱动（第一版）

- `i8042` 键盘：IRQ1，扫描码 Set-1 → Linux keycode 表；按下/抬起都发
  `EV_KEY`，每次一批事件后补 `EV_SYN/SYN_REPORT`；
- `i8042` PS/2 鼠标：IRQ12，3 字节包 → `EV_REL/REL_X,REL_Y` + `BTN_LEFT/RIGHT`；
- PIC 当前只开 IRQ0（`port/x86_64/linux.c:port_linux_timer_init` 写 `0xFE`），
  要加 IRQ1/IRQ12 的 IDT 门和 EOI；
- 如果 i8042 在 QEMU `-M pc` 下不稳定，退路是 `-device virtio-keyboard-pci` +
  最小 virtio-input 驱动（工作量大）；第一版必须先把 i8042 跑通。

#### 测试注入

为 TDD 提供 `C6EVIOCINJECT`（私有 ioctl）在内核里注入事件，测试程序不用模拟
真实键盘即可验证 read/poll 语义；集成测试再用 QEMU monitor `sendkey` 走
i8042 真路径。生产内核默认关闭该 ioctl（`C6OS_TEST_HOOKS` 宏）。

### 3.4 控制台与 VT

- 第一版**不做 VT/fbcon**：串口 tty 保持现状，GUI 程序独占 `/dev/fb0`；
- Chrome 的 kiosk/窗口模式不需要 VT；Wayland 也不需要 VT；
- 若未来跑 Xorg 或需要 Ctrl+Alt+F1，再补 VT 子系统（另立里程碑）。

### 3.5 x86 帧缓冲来源：VBE

- 在 `boot16.S`（或 `cd0.S`）进入保护模式前加 `int 0x10`：
  - `VBE 0x4F00` 取控制器信息（512 B）到 `0x5400`，校验 `"VESA"`；
  - 遍历控制器返回的 mode list，用 `VBE 0x4F01` 逐个取 `ModeInfoBlock`
    （512 B，暂存 `0x5200`），选择第一个 **800×600×32 且支持 LFB** 的模式；
    没有则退 640×480×32；不写死 `0x144` 这类厂商相关模式号；
  - `VBE 0x4F02` 设选中模式（带 `0x4000` LFB 位）；
  - 把 `PhysBasePtr / XResolution / YResolution / BitsPerPixel / BytesPerLine`
    放进 boot info（`0x7000` 区）传给内核；
- 内核 `port/x86_64/fb.c` 读 boot info，`port_as_map` 用户映射；LFB 地址通常在
  `0xE0000000`–`0xFD000000`，落在 `build_dmap` 的 1–8 GB UC 窗口内；
- QEMU 参数：`-vga std -display none`（`screendump` 可用）或 `-display gtk`。
- 若 VBE 失败：回退到 `0xFFFFFFFF` 表示无 fb，`/dev/fb0` 打开返回 `ENODEV`，
  测试必须明确报 `FAIL: no VBE framebuffer`。

### 3.6 S3/MaixCam 复用（G8）

- `port/s3/panel.c` 的 `s3_lcd_blit_rect(fb, fb_w, x, y, w, h)` 已支持任意
  `fb_w` 行跨度（注意 `docs/todo.md` §1125：x/y 同时是 fb 内源坐标）；
- 把面板驱动包成 `OsFileOps`（fb0），RGB565、240×240；`C6FBIOFLUSH` 调
  `s3_lcd_blit_rect`；
- `port/s3` 仍不能开 Linux ABI（无 MMU/U 态）；这条只给 MaixCam 的 rv64 ABI
  或裸机 GUI 复用，Chrome 不适用。

---

## 4. Chrome 所需内核能力设计

### 4.1 进程/线程：futex 是 P0

现状 `lx_futex` 的 `WAKE` 遍历**所有** `LX_P_BLOCK` 并把它们设为 READY；不校验
`uaddr`。两个线程用不同 futex 时一次唤醒会误唤醒无关线程；`FUTEX_WAIT` 的
超时/信号/`EAGAIN` 语义也没有。Chrome 的线程池、V8、PartitionAlloc 全部依赖
glibc/musl 的 pthread mutex/condvar。

设计：

```
全局哈希桶：futex_bucket[256]
struct LxFutexWaiter { u64 key; u32 bitset; LxProc *p; struct ... *next; };
```

- **key 规则**：`LX_VM_SHARED` 映射用物理地址 `lx_user_pa(uaddr)`（跨进程
  MAP_SHARED futex 必须命中同一桶）；私有映射用 `(as_id << 48) | uaddr`。
  不能一律用物理地址：私有页 COW 后 PA 会变，等待者 key 会漂移；
- `FUTEX_WAIT`：校验 uaddr 值 == val → 入桶 → `lx_proc_block`（带 deadline）；
- `FUTEX_WAKE`：只唤醒 `key` 匹配且 `bitset & WAKE_BITSET` 的等待者，最多
  `val` 个；
- `FUTEX_WAIT_BITSET`/`WAKE_BITSET`：完整 bitset 语义；`CLOCK_REALTIME` 绝对
  超时与 `CLOCK_MONOTONIC` 都要支持；`FUTEX_CLOCK_REALTIME`（256）位要识别；
- `FUTEX_REQUEUE`/`CMP_REQUEUE`：至少实现 `pthread_cond_signal` 使用的
  `CMP_REQUEUE`；
- `set_robust_list`：保存 `p->robust_list`；线程退出时遍历 robust list，把
  `FUTEX_OWNER_DIED` 写回并 wake；`get_robust_list` 返回它；
- `FUTEX_PRIVATE_FLAG (128)` 与可共享 futex 都要支持。

其他：

- `clone3`：按 `clone` 参数翻译（`CLONE_VM|CLONE_THREAD` 等）；musl 不依赖，
  但 glibc 程序（如果以后换 Debian rootfs）会探测；
- `sched_setaffinity`：解析 cpumask 并返回 0；`sched_getaffinity` 按内核
  写回的 CPU 数（1）设置 `a1` 长度；
- `prctl`：`PR_SET_NAME/PR_GET_NAME` 存 16 B comm（供 `/proc/self/stat`），
  `PR_SET_DUMPABLE`、`PR_SET_NO_NEW_PRIVS` 返回 0。

### 4.2 内存：memfd / MAP_SHARED / mremap / madvise

#### memfd 对象

```c
typedef struct LxShm {
    u32 refs;
    u64 size;
    u32 seals;                 /* F_SEAL_* */
    u64 npages;
    u64 *pages;                /* 懒分配：页号数组或 radix 树 */
} LxShm;
```

- `memfd_create(name, flags)`：分配 `LxShm`，名字存 16 B（供 `/proc/self/fd` 显示）；
  支持 `MFD_CLOEXEC`（按 flags 生效）、`MFD_ALLOW_SEALING`（未设置时
  `F_ADD_SEALS` 返回 `EPERM`）；
- `fstat`：memfd 报 `S_IFREG|0600`、`st_size=size`、`st_nlink=1`（Chrome 用它
  校验共享内存大小）；`fstatfs` 返回 tmpfs 参数；
- `ftruncate`：扩展/缩小 `size`，缩小时释放超出页；
- `read/write/pread/pwrite`：按 `pages[]` 懒分配 4 KB 页，写时分配、读未分配
  页返回 0；`fallocate` 预分配；
- `mmap`：`LX_FD_SHM` fd + `MAP_SHARED` → 遍历页表，未分配页在缺页时分配；
  `MAP_PRIVATE` 走 COW；`mmap` 的 `offset` 必须 4 KB 对齐；
- `fcntl F_ADD_SEALS/F_GET_SEALS`（命令号 1033/1034）：`F_SEAL_SEAL=0x1`、`F_SEAL_SHRINK=0x2`、
  `F_SEAL_GROW=0x4`、`F_SEAL_WRITE=0x8`、`F_SEAL_FUTURE_WRITE=0x10`；
  设置后对应操作返回 `EPERM`（`F_SEAL_WRITE` 还要求当前没有可写映射）；
- `LxShm` 生命周期：fd 和每个 `mmap` 的 VMA 各持一份引用；`close(fd)` 后映射
  仍有效，最后一个 VMA unmap 才释放页；fork 时映射双方各 `ref` 一次；
- `/proc/self/fd/N`：`open("/proc/self/fd/N")` 等价 `dup(N)`（去掉 `CLOEXEC`），
  这样 Chromium 的只读共享内存副本可用。

#### mmap 共享与 fork

- `LxVma` 增加 `u32 kind`（ANON/FILE/SHM/DEV）、`void *obj`、`u64 obj_off`；
- 页表新增软件位 `PTE_SHARED`（bit 11）：`MAP_SHARED` 的页在映射时置位；
  `port_as_copy` 看到 `PTE_SHARED` **不置 COW**，只 `port_phys_ref` 后原样映射；
  `PTE_DEV`（bit 10）同样不置 COW、unmap 不 put；
- `sys_mmap2` 先按 fd 种类分派：
  - `LX_FD_SHM` → `shm_mmap`（共享）
  - `LX_FD_DEV` + `ops->mmap` → 设备页
  - 普通文件 → 零拷贝映射（见 4.3）
- `MAP_SHARED|MAP_ANONYMOUS`：创建匿名的 `LxShm` 对象并把 fd 隐藏；否则 fork
  后父子各拿一份 COW，语义错误；
- `port_as_copy`（fork）：`LX_VM_SHARED` 的页**不置 COW**，父子的 PTE 指向
  同一物理页，引用 +1；只有 `MAP_PRIVATE` 页才 COW；
- `msync`：对 SHM/FILE 映射遍历页表，把脏页写回对象（若对象是 initramfs 只读则
  忽略）；`MS_SYNC` 对 fb0 触发 `C6FBIOFLUSH`；
- `mremap`：先支持 `MREMAP_MAYMOVE|MREMAP_FIXED`；**MAP_SHARED/FILE 映射只搬
  VMA 和页表，不能复制内容**（否则共享语义丢失）；匿名私有页搬移用新页 + 复制 +
  旧页 put（简单正确优先）；`MREMAP_DONTUNMAP` 不做；
- `madvise`：`MADV_DONTNEED`/`MADV_FREE` 对匿名页释放物理页并把 PTE 清 not-present；
  其余返回 0。V8 的堆收缩依赖它，不实现会吃满内存；
- **大虚拟保留**：接受 `MAP_NORESERVE`（0x4000）、`MAP_STACK`（0x20000）等标志；
  `PROT_NONE` 的大区间只加 VMA、不建页表。`g1map.c` 先测 1 TB
  `MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|PROT_NONE` 保留再释放（V8 sandbox/pointer
  cage 的前置）。

### 4.3 VFS / rootfs

#### 动态节点表（A7）

- `LxNode` 增加 `next`；`g_node` 改为 `LxNode **g_nodes` 指针数组 + 开放寻址
  哈希表（key=路径字符串 djb2）；`g_nodes` 从 `port_kmalloc` 增长；
- `FS_MAX_FILES` 从"硬上限"改为初始容量（1024），按需 ×2，直到内存耗尽；
- `lx_fs_init` 统计 cpio 条目数后一次性建表；每加入一个 node 计算 hash；
- 目录 `getdents` 仍按前缀遍历（Chrome 很少 readdir），保留线性但加节点数上限
  保护。

#### 零拷贝文件映射（A6）

- `mk-initramfs.py` 把每个文件数据按 4 KB 对齐（`align4` 改 `align_page`）；
- `lx_fs_pa_of(path)` 已返回文件数据物理地址；`mmap` 文件只读时：
  `port_as_map(g_as, va, pa + page_off, len, C6OS_PROT_R)`，逐页
  `port_phys_ref(pa)`（initramfs 区在启动时 `port_phys_reserve`，不能真的释放）；
- `MAP_PRIVATE|PROT_WRITE`：映射时 PTE 置只读 + `PTE_COW`；首次写 #PF 复制到
  新页（复用现有 COW 路径）；不再为每个库分配 4 KB 拷贝；
- 只读大文件（Chrome `resources.pak`、`.so` 的代码段）直接共享物理页；
- **普通文件的 `MAP_SHARED|PROT_WRITE` v1 不支持**（返回 `EOPNOTSUPP`）：Chrome
  的可写共享内存走 memfd，不走文件；不要为它做半套写回缓存；

#### rootfs 交付（A8）

- 新增 `x86-chrome-rootfs` 目标：`ALPINE_EXTRA_PKGS="chromium ..."` 生成
  `full.cpio`；文件数预计 10k+，大小 300–400 MB；
- 新增 `os_blkdev` 抽象（`linux-abi-portable.md` §6 已有设计）：x86 ATA 主/从盘；
  rootfs 作为第二块 **raw cpio** 盘（`-drive file=rootfs.img,format=raw,if=ide,index=1`），
  `/data` 为第三块 FAT 盘（`if=ide,index=3`；`-cdrom` 占 `index=2` 的 secondary
  master），启动时内核用 ATA PIO 顺序读入高地址物理区（`0x10000000` 起，避开
  低端内核）；
  启动信息（boot info）必须带 `rootfs_pa/rootfs_len`，PMM 初始化时
  `port_phys_reserve(rootfs_pa, rootfs_len)`，否则分配器会把 rootfs 覆盖掉；
  不做 FAT rootfs——cpio 直接解析，`lx_fs_pa_of` 才能对 Chrome 的 `.so`/资源做
  零拷贝映射；`/data` 仍是独立 FAT 盘，只放用户数据；
- 不再走 `cd0.S` 的 `int 15h` 逐块搬：小 rootfs 保留 ISO 路径，Chrome rootfs
  走磁盘路径；内核读盘用 32 位 PIO，速度由 QEMU 决定（目标 ≥ 20 MB/s）；
- 备选：rootfs 压缩（gzip）在启动时解压，但解压代码和内存开销更大，v1 不做。

#### `/data`

- Chrome 的 `--user-data-dir`/磁盘缓存**不放 `/data`**：SQLite/SingletonLock 需要
  POSIX 锁和符号链接，FAT 不保证；v1 放 `/tmp/chrome`（ramfs），`/data` 只放
  下载文件；持久化 profile 留到 G6 再评估；
- 8 MB → 512 MB（FAT16 16 KB 簇 / 32768 簇）或 1 GB FAT32；**现有
  `tools/mkfat.py` 只造单文件测试卷（`HELLO.TXT`），不是通用格式化器**：
  G0 要么把它扩展成通用 FAT16 格式化器（保留固定测试入口），要么构建时用
  host `mkfs.fat -F 16 -s 32` 并加依赖检查；二选一在 G0 开工前定；
- `/tmp` 保持 ramfs，大小按需。

### 4.4 procfs / sysfs

| 路径 | 用途 | 实现 |
|---|---|---|
| `/proc/meminfo` | Chrome 内存压力、V8 堆 | 真实 `port_phys_total_kb/free_kb` |
| `/proc/cpuinfo` | CPU 特性、库探测 | 从 CPUID 生成 `flags`（sse/sse2/sse3/ssse3/sse4_1/sse4_2/avx…） |
| `/proc/self/fd` | memfd 只读副本 | 目录 + `readlink`；`open` 即 dup |
| `/proc/self/maps` | crash/内存统计 | 从 VMA 链生成；现有 `PROC_BUF_MAX=4096` 不够，procfs 缓冲改动态增长（Chrome 映射可达数百 KB） |
| `/proc/self/stat` | CPU 时间、RSS | 补 utime/stime/starttime/rss 字段 |
| `/proc/self/status` | 线程数、VmRSS | 补 `Threads`/`VmRSS`/`VmSize` |
| `/proc/self/comm` | 线程名 | `prctl` comm |
| `/proc/sys/vm/overcommit_memory` 等 | Chrome 探测 | 只读小文件返回合理值；写返回 0 |
| `/sys/devices/system/cpu/online` | CPU 数 | 只读 "0" |

### 4.5 网络

- `TCP_MAX_SOCKS` 4 → 256，`LOS_SAL_MAX_SOCKETS` 16 → 256；socket 表从静态
  数组改动态（`port_kcalloc`），每 socket 发送/接收缓冲区按 64 KB 起；
- 补 `SO_RCVBUF/SO_SNDBUF/TCP_NODELAY/SO_KEEPALIVE/SO_REUSEADDR` 的
  `setsockopt`（若现有缺失）；
- Chrome 的网络服务会同时打开 HTTP/HTTPS/QUIC(不启用)/DNS 连接；`epoll` 已能
  接入；`getaddrinfo` 走 musl 的 DNS（已有）；
- 不做 IPv6（返回 `EAFNOSUPPORT`，Chrome 回退 IPv4）；`AF_UNIX` 抽象命名空间
  可后补。

### 4.6 时间与信号

- `clock_gettime`：`CLOCK_MONOTONIC`/`CLOCK_MONOTONIC_RAW`/`CLOCK_BOOTTIME`
  用 TSC ns；`CLOCK_REALTIME` 用 RTC；`CLOCK_PROCESS/THREAD_CPUTIME_ID`
  先返回 0（Chrome 不关键）；纳秒精度不再截到 ms；
- `nanosleep/clock_nanosleep` 走等待队列（现有）；
- `sigtimedwait`/`signalfd` 已有；
- Chrome 会安装 `SIGSEGV/SIGBUS/SIGILL` handler（V8 的 wasm trap handler）；
  `sigaltstack` + 信号帧已支持；`--disable-breakpad` 关闭 Crashpad，避免 ptrace；
- `SIGCHLD`/`wait4` 已有，zygote 依赖。

---

## 5. 显示服务与 Chrome 集成

### 5.1 Path A：kiosk（headless + CDP 桥，G3）

#### 进程与协议

`c6chrome-display` 是用户态 C 程序（musl 编译，放 Alpine rootfs `/usr/bin/`）：

```
c6chrome-display --url data:text/html,... --size 800x600 --fb /dev/fb0
  ├─ pipe(3,4) → fork/exec chromium
  │    --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage
  │    --remote-debugging-pipe --user-data-dir=/tmp/chrome
  ├─ CDP: Target.createTarget → Target.attachToTarget → Page.enable
  │      → Page.startScreencast {format:"jpeg", quality:70, maxWidth, maxHeight}
  ├─ 事件循环：poll([cdp_fd, evdev_fd])
  │    Page.screencastFrame → base64 解码 → JPEG 解码（stb_image）→ RGB
  │    → fb0 mmap 拷贝（缩放）→ Page.screencastFrameAck
  │    evdev 事件 → Input.dispatchMouseEvent / dispatchKeyEvent
  │    （keycode→DOM code/key/windowsVirtualKeyCode 映射表至少覆盖字母数字、
  │      方向、回车、退格、Tab、Esc、Ctrl/Shift/Alt）
  └─ 退出：SIGTERM → Browser.close → waitpid
```

- `--remote-debugging-pipe` 协议：fd 3 读、fd 4 写，**NUL 分隔的 JSON**；
  父进程 `dup2` 到 3/4 后必须清 `CLOEXEC`，其余 fd 全部 `CLOEXEC`；
- 只实现需要的 CDP 方法：`Target.createTarget/attachToTarget`、`Page.enable/
  startScreencast/screencastFrameAck`、`Input.dispatchMouseEvent/dispatchKeyEvent`、
  `Browser.close`；
- JSON 解析器是**递归下降**（消息是 `{method, params:{...}}` 的嵌套结构，不能只
  支持扁平对象），字符串转义/整数/布尔/数组都要支持；不引入第三方库；
- 若目标 Chrome 版本 `Page.startScreencast` 不可用，回退到定时
  `Page.captureScreenshot`（性能差但保证 C2 可验收）；
- JPEG 解码 vendor `stb_image.h`（public domain，单头文件）；若 rootfs 已有
  `libjpeg-turbo` 可链接系统库，二选一在 G3 开始前定。

#### 优点

- 不依赖 Wayland/X11/DRM；不需要实现窗口系统；
- Chrome 的软件合成器（Skia）已经在 headless 下渲染完整页面；
- 先把"屏上能看到 Chrome"跑通，给 GUI 设备层/输入层提供真实验收场景。

#### 缺点

- 每帧 JPEG 编码/解码，800×600 下 5–15 fps（QEMU TCG）；240×240 可 15–20 fps；
- 没有原生窗口/多窗口语义（kiosk 足够）；
- 输入坐标映射要自己做（fb 像素 ↔ Chrome viewport）。

### 5.2 Path B：最小 Wayland 合成器（G4/G5）

#### 技术选型

**推荐 A：自包含 Wayland wire protocol，不链接 `libwayland-server`/`pixman`/`xkbcommon`。**
理由：宿主是 glibc，guest 是 musl；Alpine 的 `libwayland-server`/`pixman` 是 musl
动态库，宿主 `gcc -static` 链接不了，guest 里也没有构建工具链。自包含实现只需
libc + `mmap`/socket，`c6wl` 可作为静态二进制放进 rootfs，测试客户端 `wl_demo`
也能用同一套 wire 协议在宿主静态编译。代价是对象注册/消息编解码要自己写，
但本线只支持 Chrome 所需子集（约 2–3k 行 C）。

**备选 B**：在 Alpine rootfs 构建阶段用 `chroot` + `apk add build-base
wayland-dev pixman-dev libxkbcommon-dev` 编译，保留 `libwayland-server` 的
对象生命周期管理。需要构建机 root/工具链，G4 开工前评估；不默认。

- **不用 wlroots**：wlroots 的 DRM/GBM 后端依赖 DRM，headless 后端无输出；
  自研最小合成器反而更小、更可控；
- 后端：`fbdev`（写 `/dev/fb0`）+ `evdev`（读 `/dev/input/event0`）；
- 合成：直接读 client `wl_shm` buffer（`memfd` mmap）做 ARGB/XRGB 行拷贝 +
  简单 alpha 混合；不引入 pixman；
- 键盘：内置 US keymap 文本（`wl_keyboard.keymap` XKB_V1 fd），不依赖
  `libxkbcommon`/`xkeyboard-config`；但 Chrome 自己解析 keymap 需要 rootfs 里有
  `xkeyboard-config` 数据，G0 的 Chrome 依赖闭包要带上；
- 定时器 ~60 Hz 重绘（无 vsync；用 `timerfd`）。

#### 协议范围（只够 Chrome）

| 协议 | 对象 | 说明 |
|---|---|---|
| `wl_display` | — | 自建 AF_UNIX `$XDG_RUNTIME_DIR/wayland-0`；对象 id 分配/消息分发 |
| `wl_compositor` | surface/region | 只支持 `WL_SURFACE_ERROR_INVALID_SIZE` 等基本错误 |
| `wl_shm` | pool/buffer | 只支持 `WL_SHM_FORMAT_XRGB8888/ARGB8888`；`memfd_create` + `mmap` |
| `xdg_shell` | xdg_wm_base/xdg_surface/xdg_toplevel | 单一窗口；configure 固定输出尺寸；无 server-side decoration |
| `wl_seat` | pointer/keyboard | 指针 + 键盘；`wl_keyboard.keymap` 用 memfd 发 xkb keymap |
| `wl_output` | — | geometry/mode/scale=1/transform=normal |
| `wl_callback` | frame | `wl_surface_frame` 的 done 事件 |

- `wl_shm` 缓冲：`memfd_create("wl_shm", MFD_CLOEXEC|MFD_ALLOW_SEALING)` +
  `ftruncate` + `mmap`；合成器直接读 buffer，缩放/合成后写 fb；
- 像素格式：fb 为 32bpp 时直接 `memcpy` stride 行（ARGB 先做 alpha 混合）；
  16bpp 时逐像素转 `r5g6b5`；
- 输入：evdev → `wl_pointer.motion/button/axis`、`wl_keyboard.key/enter/leave`；
  软件光标至少支持 `wl_pointer.set_cursor` 的 32×32 ARGB 图片（没有硬件光标）；
- 窗口：Chrome 的 `xdg_toplevel` 默认全屏（kiosk）；不做最大化/最小化。

#### 与 Chrome 的兼容性

- Chrome 用 `--ozone-platform=wayland --no-sandbox --disable-gpu`；
- 需要 `XDG_RUNTIME_DIR` 存在且可写（rootfs 里设 `/run/user/0`）；
- Chrome 的 Wayland 后端会创建 `wl_shm` buffer、`wl_seat`、`xdg_surface`；
- 若 Chrome 要求 `zwp_linux_dmabuf`/EGL（GPU 路径），`--disable-gpu` 会走
  wl_shm 软件路径，不需要 dmabuf。

### 5.3 DRM/KMS（G7，可选）

- `/dev/dri/card0`：`DRM_IOCTL_VERSION`、`MODE_GETRESOURCES`、
  `MODE_GETCONNECTOR`、`MODE_GETENCODER`、`MODE_CREATE_DUMB`、
  `MODE_MAP_DUMB`、`MODE_ADDFB2`、`MODE_SETCRTC`、`MODE_PAGE_FLIP`；
- 只作为合成器后端（page-flip + vsync），不追求 Chrome 的 `ozone-drm`
  （它需要 GBM/EGL，超出本线）；
- 复用 PMM 引用计数实现 GEM 缓冲区（`LxGem` = 物理页数组 + refs）。

### 5.4 Chrome 启动参数基线

```sh
# C1：headless，最小
chromium --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
  --disable-breakpad --disable-crash-reporter --no-first-run --no-default-browser-check \
  --user-data-dir=/tmp/chrome --disk-cache-dir=/tmp/chrome/cache \
  --js-flags=--max-old-space-size=256 --dump-dom 'data:text/html,<h1>hi</h1>'

# C3：Wayland
chromium --ozone-platform=wayland --no-sandbox --disable-gpu \
  --disable-dev-shm-usage --disable-breakpad --disable-crash-reporter \
  --no-first-run --no-default-browser-check \
  --user-data-dir=/tmp/chrome --disk-cache-dir=/tmp/chrome/cache \
  --js-flags=--max-old-space-size=512 --window-size=800,600
```

- `--disable-dev-shm-usage` 是**临时**兜底：G1 实现 memfd 后应去掉并验证；
  rootfs init 仍要建 `/dev/shm`（ramfs）并设 `XDG_RUNTIME_DIR=/run/user/0`（0700）；
- `--user-data-dir` 先放 `/tmp`（ramfs）：Chrome 的 SQLite/SingletonLock 需要
  POSIX 锁和符号链接，FAT 驱动不保证；持久化到 `/data` 留到 G6 评估；
- `--no-sandbox` 必须：没有 user namespace/seccomp；
- `--disable-breakpad`：没有 ptrace；
- `--single-process` 仅在内存不足时调试用，不作为验收口径（多进程是 Chrome
  的正常形态）。

---

## 6. 里程碑（TDD）

> 每个里程碑的"测试先行"是第一个提交，必须先在当前树跑出该里程碑的 FAIL；
> "实现"提交让测试转 PASS；"验收"是最终 `make check-linuxabi-gN` 的 PASS 串。

### G0 运行底座（rootfs / 内存 / procfs）

**目标**：Chrome 二进制能进 guest 并被 loader 读到；系统资源报告真实。

**测试先行（RED）**

| 测试 | 文件 | 当前预期失败 |
|---|---|---|
| G0-1 sysinfo 真实内存 | `tests/linux/g0sysinfo.c` | `sysinfo.totalram` 只有 24 MB（现状） |
| G0-2 `/proc/meminfo`/`/proc/cpuinfo` | `tests/linux/g0proc.c` | 固定 24576 kB / 2 CPU（现状） |
| G0-3 动态 initramfs 节点 >1024 | `tests/host/t_fs_nodes.c` + `tests/linux/g0nodes.c` | 第 1025 个条目被丢弃 |
| G0-4 rootfs 加载 | `tools/check-linuxabi-g0.sh` | rootfs 里没有 chromium（现状） |

**实现任务**

1. PMM 支持 8 GB 物理地址空间（`PMM_MAX_FRAMES` 动态化；**E820 必须保留 4 GB
   以上条目**，`build_dmap` 扩到 8 GB；位图 256 KB 放到 1–4 MB 保留区，
   `.bss` 不越过 `0xA0000`；`port_phys_total_kb/free_kb` 真实）。注意
   `port_phys_total_kb` 不能拿"最高物理页号"算：PCI hole 会被算进 totalram；
   要累加 E820 usable 区间得到 `usable_kb`，`sysinfo` 用它；
   **DMAP 不能把 1–4 GB 全按 UC**：>1 GB 的 RAM 是普通内存，必须按 E820 把
   usable 区间映射 WB，只有 PCI hole/MMIO 才 UC（否则内核访问 1–3 GB RAM 会
   慢到不可用，且与用户 WB 映射形成内存类型别名）；做法：0–8 GB 先按 2 MB
   默认 UC 建表，再对每个 E820 usable 区间覆盖 WB，并加
   `port_phys_is_ram(pa)` 供分配器/映射断言；`build_dmap` 建表期间还在
   stage1 的 1 GB identity map 上，页表页必须从 **1 GB 以下** 分配
   （`port_phys_alloc_below(1GB)` 或先建临时 DMAP），不能拿到 4 GB 以上的帧；
2. `LX_PROC_MAX_CAP`/fd 表保持，检查 Chrome 进程数量；
3. `LxNode` 动态表 + 哈希（`FS_MAX_FILES` 1024 → 按需）；
4. rootfs 构建目标 `x86-chrome-rootfs`（`fetch-apk.py ... chromium`）+ 第二块
   raw cpio 盘（`os_blkdev` + ATA 从盘）加载；boot info 带 `rootfs_pa/len`，
   `pmm_init` 必须 `port_phys_reserve` 该区间；
5. `/data` 扩到 512 MB（`mkfat.py --size-mb`）；
6. `sysinfo`/`proc_meminfo_` 接 PMM；
7. rootfs init 建 `/dev/shm`（ramfs）、`/run/user/0`（0700）、`/tmp`（ramfs），
   并导出 `XDG_RUNTIME_DIR=/run/user/0`（`c6wl`/Chrome 的硬前置）。

**验收（GREEN）**

```
make check-linuxabi-g0
PASS: linuxabi-g0（rootfs 内 chromium --version = 131.x；sysinfo ≥ 2.9 GB；节点数 > 10k）
```

### G1 Headless Chromium（syscall/内存语义）

**目标**：`--headless=new --dump-dom` 在 guest 里完整跑通。

**测试先行（RED）**

| 测试 | 文件 | 失败点 |
|---|---|---|
| G1-1 futex 按地址 + 超时 | `tests/linux/g1futex.c` | WAKE 唤醒无关线程；超时忽略 |
| G1-2 memfd + MAP_SHARED + fork | `tests/linux/g1shm.c` | `memfd_create` ENOSYS；mmap 私有拷贝 |
| G1-3 mremap/madvise | `tests/linux/g1mremap.c` | ENOSYS / 不释放页 |
| G1-4 文件 mmap 零拷贝 | `tests/host/t_filemap.c` + `tests/linux/g1filemap.c` | 逐页分配+拷页 |
| G1-5 大虚拟保留/共享匿名 | `tests/linux/g1map.c` | 1 TB `PROT_NONE` 保留失败；`MAP_SHARED` + `MAP_ANON` 不共享 |
| G1-6 TCP/SAL 扩容 | `tests/linux/g1sock.c` | 第 5 条 TCP 连接 `EMFILE` |
| G1-7 POSIX 记录锁 | `tests/linux/g1fcntl.c` | `F_SETLK` 返回 `EINVAL`；SQLite 初始化失败 |
| G1-8 Chrome headless | `tools/check-linuxabi-g1.sh` | Chrome 启动即 ENOSYS/崩溃 |

**实现任务**

1. 重写 `lx_futex`（§4.1）+ robust list + `clone3` 兜底 + `sched_setaffinity`；
2. `LxShm` memfd + seals（§4.2）+ `/proc/self/fd`；
3. `mmap` 分派与 `MAP_SHARED`/fork 修正 + `mremap`/`madvise`；
4. 文件 mmap 零拷贝（§4.3）+ cpio 4 KB 对齐；
5. `procfs` 真实 meminfo/cpuinfo + `/proc/self/maps` 最小版（缓冲动态增长，
   不能沿用 4 KB 的 `PROC_BUF_MAX`）；
6. `prctl` comm、`clock_gettime` ns/clock id；
7. `TCP_MAX_SOCKS`/`SAL_MAX_SOCKETS` 扩容（Chrome 启动时就会开 socket）；
8. `fcntl` 记录锁（`F_SETLK/F_GETLK/F_SETLKW`）供 Chrome profile 的 SQLite 使用。

**验收（GREEN）**

```
make check-linuxabi-g1
PASS: linuxabi-g1（futex 16 线程/共享内存 fork/mremap/madvise 全 PASS；headless dump-dom 含 <h1>）
```

### G2 fb0 + evdev（设备层）

**目标**：`/dev/fb0` 画色块、`/dev/input/event0` 读键鼠事件。

**测试先行（RED）**

| 测试 | 文件 | 失败点 |
|---|---|---|
| G2-1 fb ioctl/mmap | `tests/linux/g2fb.c` | open `/dev/fb0` ENOENT |
| G2-2 evdev read/poll | `tests/linux/g2evdev.c` | open `/dev/input/event0` ENOENT |
| G2-3 屏幕像素 | `tools/check-linuxabi-g2.sh`（QEMU screendump + Python） | 无屏幕输出 |
| G2-4 i8042 真实按键 | 同上（QEMU `sendkey a`） | 无事件 |

**实现任务**

1. `OsFileOps` + `LxDev` 注册表（§3.1）；
2. `/dev/fb0`（§3.2）+ `PTE_DEV` + `port_as_map` UC；
3. `/dev/input/event0` + 环形队列 + poll + `C6EVIOCINJECT`；
4. x86 VBE 模式（§3.5）+ `port/x86_64/fb.c`；
5. i8042 IRQ1/IRQ12（PIC 掩码、IDT、keycode 表）；
6. QEMU 测试参数 `-vga std -display none` + monitor `screendump`。

**验收（GREEN）**

```
make check-linuxabi-g2
PASS: linuxabi-g2（framebuffer 800x600x32；色块像素校验通过；sendkey 收到 KEY_A）
```

### G3 kiosk：headless Chrome → fb0（C2）

**目标**：屏上看到 Chromium 页面；按键/点击能操作。

**测试先行（RED）**

| 测试 | 文件 | 失败点 |
|---|---|---|
| G3-1 CDP NUL-JSON/base64 | `tests/host/t_cdp.c` | 无实现 |
| G3-2 screencast 帧解码 | `tests/host/t_jpeg.c`（stb_image 单元） | 无实现 |
| G3-3 端到端 | `tools/check-linuxabi-g3.sh` | `c6chrome-display` 不存在 |

**实现任务**

1. `user/c6chrome-display`：CDP 管道客户端、JSON、base64、JPEG、fb 写；
2. evdev → CDP 输入映射；
3. Chrome 启动参数/生命周期管理（fork/exec、SIGTERM）；
4. 端到端脚本：启动 Chrome→加载 `data:text/html,<h1 id=x>...`→截图→
   `Page.evaluate` 改颜色→再截图。

**验收（GREEN）**

```
make check-linuxabi-g3
PASS: linuxabi-g3（screendump 页面中心为预期色；sendkey 后页面颜色变化）
```

### G4 最小 Wayland 合成器（C3 前置）

**目标**：自研 `c6wl` 能跑 Wayland 演示客户端，`wl_shm` + xdg_shell + 输入完整。

**测试先行（RED）**

| 测试 | 文件 | 失败点 |
|---|---|---|
| G4-1 wl_shm buffer | `tests/linux/wl_demo.c` | 无 compositor socket |
| G4-2 surface commit/帧回调 | 同上 | 无输出 |
| G4-3 合成器像素 | `tools/check-linuxabi-g4.sh`（screendump） | 无实现 |

**实现任务**

1. `user/c6wl`：自包含 Wayland wire protocol（对象 id/消息分发/AF_UNIX socket）+
   `wl_compositor`/`wl_shm`/`wl_output`/`wl_seat`/`xdg_shell`/frame callback（§5.2）；
2. 自写 ARGB/XRGB 行拷贝 + alpha 混合到 fb0（32bpp/16bpp 两条路径），不引入 pixman；
3. evdev 输入 → wl_pointer/wl_keyboard（内置 US keymap，memfd 发送）；
4. `XDG_RUNTIME_DIR`、socket 文件、SIGCHLD 清理、软件光标。

**验收（GREEN）**

```
make check-linuxabi-g4
PASS: linuxabi-g4（wl_demo 全屏纯色 + 移动方块；screendump 像素校验）
```

### G5 Chrome on Wayland（C3/C4）

**目标**：Chrome 作为 Wayland 客户端显示并接受输入；30 分钟稳定。

**测试先行（RED）**

| 测试 | 文件 | 失败点 |
|---|---|---|
| G5-1 Chrome 启动 | `tools/check-linuxabi-g5.sh` | Wayland 连接失败/崩溃 |
| G5-2 输入 | 同上（sendkey/mouse） | 无反应 |
| G5-3 长稳 | `tools/check-linuxabi-g5-long.sh`（30 min） | 未实现 |

**实现任务**

1. Chrome `--ozone-platform=wayland` 适配（窗口尺寸、configure、keymap）；
2. `c6wl` 兼容 Chrome 的 `wl_shm` 格式/`xdg_toplevel` 配置；
3. 内存/线程压力调优（V8 heap、renderer 数量）；
4. 长稳脚本与内存基线。

**验收（GREEN）**

```
make check-linuxabi-g5
PASS: linuxabi-g5（Chrome 页面显示；键盘输入改变 DOM；30 min 无崩溃，RSS 回基线 ±10%）
```

### G6 性能收口

- 启动时间：QEMU TCG 下 `--dump-dom` ≤ 60 s（冷启动，含 Chrome 进程树）；
- 首屏：G3/G5 首帧 ≤ 30 s；
- 文件 mmap 零拷贝命中率 > 90%（避免启动时 150 MB 拷贝）；
- 内存：Chrome 主进程 + 2 renderer 常驻 ≤ 1.5 GB（3 GB 机器）；
- 网络：TCP 256 socket 压测不丢连接。

**测试先行**：`tools/perf-linuxabi-g6.sh` 先输出基线（当前必超标），实现后达标。

### G7 DRM/KMS（可选）

- `/dev/dri/card0` + dumb buffer + page flip；只服务合成器；不承诺 Chrome
  `ozone-platform=drm`；验收 `check-linuxabi-g7`（合成器 page-flip 不撕裂）。

### G8 S3/MaixCam 复用（可选）

- 把 fb0/evdev 的 `OsFileOps` 接到 `port/s3/panel.c`（RGB565、`C6FBIOFLUSH`）；
- 仅裸机 GUI 或 MaixCam rv64 ABI 使用；Chrome 不在 S3 上运行；
- 验收：S3 屏上跑 `t_s3_gui` 等价程序 + fb0 写像素。

---

## 7. TDD 测试设计与执行

> 本章是施工合同：**任何实现代码之前，必须先有本章对应的失败测试**。测试不是
> "事后补"，而是每个里程碑的入口条件；没有 RED 提交的里程碑不开工。

### 7.1 测试金字塔

| 层 | 目录 | 构建/运行 | 覆盖对象 | 命令 |
|---|---|---|---|---|
| **H 宿主单测** | `tests/host/`（新增） | 宿主 `gcc -O2 -g -fsanitize=address,undefined` | 纯逻辑：fb/evdev 结构布局、CDP JSON/base64、JPEG、shm 页计算、seals 状态机 | `make check-host-gN` |
| **K 内核语义** | `tests/linux/g*.c` | 宿主 `gcc -static`，装进 initramfs，QEMU 串口判定 | syscall 语义、VMA/COW、futex、memfd、设备 ioctl | `make check-linuxabi-gN` |
| **S 屏幕端到端** | `tools/check-linuxabi-gN.sh` | QEMU + monitor `screendump` + Python PPM 校验 | fb0 真实像素、Chrome 首帧、输入后变化 | `make check-linuxabi-gN` |
| **L 长稳/性能** | `tools/check-linuxabi-g5-long.sh`、`tools/perf-linuxabi-g6.sh` | QEMU 长时间运行、采样 RSS/帧率 | 30 分钟不崩、内存回基线、启动/首帧阈值 | `make check-linuxabi-g5-long`、`make perf-linuxabi-g6` |

**测试优先顺序固定为 H → K → S → L**：能用宿主单测钉死的逻辑不放进 QEMU；
QEMU 测不了像素的才上 `screendump`；性能/长稳最后做，且必须单独目标、不混入
快速回归。

### 7.2 目录与命名

```
tests/host/
  t_fb_layout.c          # sizeof/offsetof/ioctl 号
  t_evdev_layout.c       # input_event 布局、EVIOC* 号
  t_cdp.c                # NUL 分隔 JSON、id 配对、base64 向量、错误输入
  t_jpeg.c               # 合法/截断 JPEG 解码
  t_shm.c                # memfd 页索引、seals 状态转换
  t_filemap.c            # cpio 4 KB 对齐、文件页映射偏移计算
  t_fs_nodes.c           # 动态节点表哈希/扩容（G0-3）
tests/linux/
  g0sysinfo.c  g0proc.c  g0nodes.c
  g1futex.c    g1shm.c   g1mremap.c  g1filemap.c  g1sock.c  g1map.c  g1fcntl.c
  g2fb.c       g2evdev.c
  wl_demo.c
tools/
  check-linuxabi-g0.sh ... check-linuxabi-g5.sh
  check-linuxabi-g5-long.sh
  perf-linuxabi-g6.sh
  qemu_gui.py            # 起机/串口等待/screendump/sendkey/PPM 解析
  ppm_check.py           # 纯 stdlib P6 解析 + 坐标断言
```

命名与输出约定：

- 用例 ID：`G<里程碑>-<序号>`（如 `G1-1`），测试文件头注释写清"验什么"；
- 每个断言打印 `FAIL: g1-1 <case> (line N, errno=E)`，全部通过打印
  `PASS: g1-1 <case> (<pass>/<total> checks)`；
- 里程碑脚本最后一行必须打印 `PASS: linuxabi-gN（...）`，失败打印
  `FAIL: linuxabi-gN` 并把串口日志尾部 30 行输出到 stderr；
- Makefile 变量沿用现有风格：`G0_SRC`、`G0_ELF`、`G0_IMG`，不要另起一套。

### 7.3 红-绿-重构提交协议（每个里程碑强制）

| 步骤 | 提交内容 | 必须满足 | 提交信息 |
|---|---|---|---|
| **RED** | 只加测试文件、fixture、Makefile 目标、`check-*` 脚本 | 在当前树运行 `make check-...` 得到**预期失败原因**（如 `ENOSYS`、`ENOENT`、`assert 0`），不是编译错误、不是脚本语法错误；`make check-all` 仍绿 | `test(gN): add failing acceptance tests (RED)` |
| **GREEN** | 最小实现，让同一测试转 PASS | `make check-linuxabi-gN` 连续 3 次 PASS；`make check-linuxabi-all` 无回归 | `feat(gN): <能力>（GREEN）` |
| **REFACTOR** | 可选，只改实现/测试辅助代码 | 测试文件断言不变；全绿 | `refactor(gN): ...` |

硬规则：

1. **RED 提交禁止包含生产代码**；GREEN 提交禁止放宽/删除测试断言。测试本身写错，
   单独 `test(gN): fix test` 提交并在提交信息说明原因。
2. **禁止 `|| true`、`set +e`、自动重试、`sleep` 等掩盖失败**；`check-*` 脚本
   一律 `set -euo pipefail`，QEMU 一律带 `timeout`。
3. **失败必须可诊断**：脚本失败时保留串口日志、QEMU 命令行、PPM 截图到
   `build/x86/artifacts/gN-<时间戳>/`，并在 CI 输出中打印路径。
4. **每级不退**：新测试加入 `tools/check-linuxabi-all.sh` 前，先确认它在
   `make check-linuxabi-gN` 里已 GREEN；加入后 `check-linuxabi-all` 必须全绿。
5. **Definition of Done**：该里程碑所有测试文件、脚本、fixture 已提交；RED
   提交在分支历史里可查；GREEN 连续 3 次通过；`docs/todo.md` 状态更新为
   "✅ Gx"；PASS 输出贴进提交信息。

### 7.4 测试夹具与 QEMU 封装

新增 `tools/qemu_gui.py`（stdlib only），所有 GUI 测试共用，避免每个脚本复制
QEMU 参数：

```python
# 伪接口（实现时按此契约）
q = Qemu(vga="std", display="none", m=3072, cpu="max",
         monitor="/tmp/c6qmp", serial_log=...)
q.boot(iso_or_disk, timeout=120)
q.wait_serial("C6READY", timeout=60)
q.sendkey("a"); q.sendkey("ctrl+c")
q.screendump("/tmp/c6screen.ppm")
p = ppm_read("/tmp/c6screen.ppm")          # P6 解析，无第三方库
assert p.pixel(400, 300) == (0x12, 0x34, 0x56)
q.kill()
```

- **串口判定**：测试程序主动打印 `C6READY`，脚本等待后再操作，避免"启动慢导致
  假失败"；所有等待有上限。
- **屏幕判定**：`screendump` 后重试最多 3 次（仅对 QEMU 截图文件尚未落盘这类
  基础设施抖动），像素断言本身不重试。
- **注入**：`C6EVIOCINJECT` 只在 `C6OS_TEST_HOOKS=1` 的内核里编译；真实 i8042
  路径用 QEMU monitor `sendkey` 单独覆盖，二者都要过。
- **Wayland 测试客户端**：`tests/linux/wl_demo.c` 不用 `libwayland-client`（宿主
  是 glibc，guest 是 musl，静态/动态都对不上）；直接按 Wayland wire protocol
  （`wl_display.sync`/`wl_registry.bind`/`wl_compositor`/`wl_shm`/`xdg_shell`
  的消息 id+opcode）手写，宿主 `gcc -static` 可编。服务端 `c6wl` 也走同一套
  自包含 wire protocol（见 §5.2 方案 A），不依赖 Alpine 的 Wayland 库。
- **确定性**：测试禁用 RTC 相关随机、固定 `-smp 1`、固定内存大小；`getrandom`
  测试只断言范围/非全零，不断言具体值。

### 7.5 宿主单测用例清单（H 层）

| 测试 | 断言 |
|---|---|
| `t_fb_layout.c` | `sizeof(struct fb_var_screeninfo)==160`、`sizeof(struct fb_fix_screeninfo)==80`、关键 `offsetof`（`smem_start`、`line_length`、`mmio_start`）、`FBIO*` 常量值 |
| `t_evdev_layout.c` | `sizeof(struct input_event)==24`、`time` 偏移 0、`type/code/value` 偏移 16/18/20、`EVIOCGNAME(256)==0x81004506` |
| `t_cdp.c` | NUL 切帧、`id` 请求/响应配对、JSON 字符串转义、`base64` RFC 4648 向量、畸形帧返回错误不崩溃 |
| `t_jpeg.c` | 16×16 基线 JPEG 解码出正确尺寸/首像素；截断文件返回错误；0 字节返回错误 |
| `t_shm.c` | `ftruncate` 扩/缩页数计算、`MAP_SHARED` 偏移对齐检查、`F_SEAL_SHRINK/GROW/WRITE/SEAL` 状态机每个转换的允许/拒绝 |

### 7.6 内核语义用例清单（K 层）

| 测试 | 用例（每条都必须有正/负向） |
|---|---|
| `g0sysinfo.c` | ① `totalram`/`freeram` ≥2.9 GB 且 `<` 物理上限；② `sysinfo` 结构 112 B 字段偏移；③ E820 高内存条目被计入 |
| `g0proc.c` | ① `/proc/meminfo` 与 `sysinfo` 一致；② `/proc/cpuinfo` flags 含实际 CPUID 特性；③ `/proc/self/maps` >4 KB 不截断 |
| `g0nodes.c` | ① 1025 个文件节点全部可见；② 10k 节点哈希查找正确；③ 重名/长路径错误可预期 |
| `g1futex.c` | ① 两个线程各自 futex，`WAKE` 只唤醒目标；② `WAIT` 超时返回 `ETIMEDOUT`；③ `WAIT_BITSET/WAKE_BITSET` 位选择；④ `CMP_REQUEUE` 搬移等待者；⑤ robust list 持有者退出后 `FUTEX_OWNER_DIED`；⑥ fork 后共享页上的 futex 跨进程命中；⑦ 非对齐 uaddr 返回 `EINVAL` |
| `g1shm.c` | ① `memfd_create` + `ftruncate` + `mmap(MAP_SHARED)` 父子可见；② `MAP_PRIVATE` 写不串；③ `F_SEAL_SHRINK/GROW/WRITE` 后对应操作 `EPERM`；④ `/proc/self/fd/N` open 得到独立 fd；⑤ `ftruncate` 缩小释放页且访问 SIGBUS；⑥ 页引用计数回到基线 |
| `g1mremap.c` | ① 扩大保留数据；② 缩小释放尾部页；③ `MREMAP_MAYMOVE` 搬移后数据不变、旧地址不可访问；④ `MREMAP_FIXED` 覆盖目标；⑤ 非法对齐 `EINVAL` |
| `g1filemap.c` | ① 只读映射 `/bin/busybox` 首/末页字节与 `read` 一致；② `MAP_PRIVATE` 写后文件不变；③ `madvise(DONTNEED)` 后重新读回原内容；④ 1000 次 mmap/munmap 物理页回到基线 |
| `g1sock.c` | ① 同时打开 64 条 TCP 连接（超过旧上限 4）；② `SO_RCVBUF/SNDBUF` 设置生效；③ `TCP_NODELAY` 不报错；④ 关闭后 socket 表回收 |
| `g1map.c` | ① 1 TB `PROT_NONE` 保留/释放；② `MAP_NORESERVE` 不预分配物理页；③ `MAP_FIXED_NOREPLACE` 冲突返回 `EEXIST`；④ `MAP_SHARED` + `MAP_ANONYMOUS` fork 后父子共享同一页 |
| `g1fcntl.c` | ① `F_SETLK` 写锁成功、第二 fd 冲突 `EAGAIN`；② `F_GETLK` 返回冲突 pid/范围；③ `F_SETLKW` 阻塞后被释放唤醒；④ 关闭 fd 自动释放锁 |
| `g2fb.c` | ① `FBIOGET_VSCREENINFO` 返回 800×600×32；② `FBIOGET_FSCREENINFO` 的 `smem_len/line_length` 自洽；③ mmap 写像素再读回；④ 非法 `FBIOPUT_VSCREENINFO` 返回 `EINVAL`；⑤ `C6FBIOFLUSH` 后 S3 面板收到像素（G8） |
| `g2evdev.c` | ① `EVIOCGNAME` 字符串正确；② `C6EVIOCINJECT` 后 `read` 得到 24 字节事件、字段顺序正确；③ 无事件非阻塞 `EAGAIN`；④ `poll(POLLIN)` 有事件才返回；⑤ 队列满丢弃策略可预期 |
| `wl_demo.c` | ① `wl_shm` 创建 buffer；② attach/commit 后合成器收到 damage；③ frame callback 触发；④ 键盘事件送达；⑤ 400×300 窗口在 800×600 fb 上位置正确 |

### 7.7 端到端脚本契约（S 层）

每个 `tools/check-linuxabi-gN.sh` 固定结构：

```sh
#!/bin/sh
set -eu
# 1) 构建所需镜像（失败即 FAIL，不跳过）
make x86-gN-image
# 2) 起 QEMU（唯一允许的超时/日志路径）
timeout 180 python3 tools/qemu_gui.py --case gN --serial build/x86/gN.log ...
# 3) 串口断言
grep -q '^PASS: linuxabi-gN' build/x86/gN.log
# 4) 屏幕断言（仅 UI 测试）
python3 tools/ppm_check.py --image build/x86/gN.ppm --pixel 400,300=#123456
# 5) 汇总
echo 'PASS: linuxabi-gN（...）'
```

- 屏幕坐标、颜色、页面 HTML 全部写在脚本顶部常量区，不在断言里硬编码魔法数；
- 失败时脚本退出码非 0，且打印 `FAIL: linuxabi-gN` + 日志尾部；
- 任何测试不允许依赖宿主网络（Chrome 页面用 `data:` URL 或本地临时 HTTP）。
- **超时预算**：G0/G1 各 300 s（400 MB rootfs + Chrome 冷启动），G2/G3/G4 180 s，
  G5 快速 300 s、长稳 2400 s；超时按 FAIL 处理，不自动重试。

### 7.8 需求 → 测试追溯矩阵

| 需求（§1.2/§1.3） | 测试 | 里程碑 |
|---|---|---|
| A1 futex 语义 | `g1futex.c` G1-1 | G1 |
| A2 memfd + seals | `g1shm.c` G1-2、`t_shm.c` | G1 |
| A3 MAP_SHARED/fork | `g1shm.c` G1-2 | G1 |
| A4 mremap | `g1mremap.c` G1-3 | G1 |
| A5 madvise | `g1mremap.c` G1-3、`g1filemap.c` | G1 |
| A6 文件 mmap 零拷贝 | `g1filemap.c` G1-4 | G1 |
| A7 initramfs 节点容量 | `g0nodes.c` G0-3 | G0 |
| A8 rootfs 容量/加载 | `check-linuxabi-g0.sh` G0-4 | G0 |
| A9 `/proc/self/fd` | `g1shm.c` G1-2 | G1 |
| A10 meminfo/sysinfo | `g0sysinfo.c`、`g0proc.c` | G0 |
| A11 PMM >1 GB | `g0sysinfo.c`（totalram） | G0 |
| A12 TCP/SAL 容量 | `g1sock.c` G1-6 | G1 |
| A13–A19 syscall 杂项 | `g1sock.c` + 逐项小用例（clone3/prctl/clock/statx） | G1 |
| MAP_SHARED + MAP_ANON / 大保留 | `g1map.c` G1-5 | G1 |
| A20 抽象 UNIX socket（观察项） | 暂不强制，G1 若 Chrome 报错再补 | G1 |
| A21 POSIX 记录锁 | `g1fcntl.c` G1-7 | G1 |
| fb0 / FBIO* | `g2fb.c`、`t_fb_layout.c` | G2 |
| evdev / EVIOC* | `g2evdev.c`、`t_evdev_layout.c` | G2 |
| VBE 显示 | `check-linuxabi-g2.sh`（S 层） | G2 |
| C2 屏上 Chrome | `check-linuxabi-g3.sh` | G3 |
| Wayland 协议子集 | `wl_demo.c` | G4 |
| C3/C4 Chrome Wayland | `check-linuxabi-g5.sh`、`-g5-long` | G5 |

原则：**§1.2 的每个 P0 需求至少有一个自动化测试**；P1/P2 在对应里程碑前补测试，
不允许"先实现、后补测"。

### 7.9 质量门槛

- H 层新增代码行覆盖率 ≥ 80%（`gcov`/`lcov`，CI 只对 `tests/host` 统计）；
- 每个新 syscall 至少 1 个正向 + 2 个负向（`EFAULT`/`EINVAL`/`ENOSYS`）用例；
- 每轮 GREEN 后跑一次全量 `make check-linuxabi-all`，任何历史项变红即阻塞合并；
- 内存泄漏门禁：测试前后 `port_phys_free_kb()` 回到基线 ±4 KB（沿用 M13 判据）；
- 测试自身不得引入第三方运行时依赖（Python 只用 stdlib；`stb_image.h` vendor）。

---

## 8. 验收矩阵（全部可复现）

| 目标 | 命令 | PASS 串/判据 |
|---|---|---|
| G0 底座 | `make check-linuxabi-g0` | `PASS: linuxabi-g0（...）` |
| G1 headless | `make check-linuxabi-g1` | `PASS: linuxabi-g1（futex/memfd/mremap/dump-dom）` |
| G2 fb/evdev | `make check-linuxabi-g2` | `PASS: linuxabi-g2（fb 色块 + KEY_A）` |
| G3 kiosk | `make check-linuxabi-g3` | `PASS: linuxabi-g3（页面中心色 + 输入后变色）` |
| G4 合成器 | `make check-linuxabi-g4` | `PASS: linuxabi-g4（wl_demo 像素）` |
| G5 Chrome Wayland | `make check-linuxabi-g5` | `PASS: linuxabi-g5（窗口 + 输入 + 30min）` |
| G6 性能 | `make perf-linuxabi-g6` | 各项阈值在 `tools/perf-linuxabi-g6.sh` 顶部 |
| 零回归 | `make check-linuxabi-all` | 原有 N 项 + G0–G5 全绿 |

QEMU 屏幕校验约定：

- 统一用 `-vga std -display none -monitor unix:/tmp/c6qmp,server,nowait`；
- 脚本发 `screendump /tmp/c6screen.ppm`，Python 读 PPM 检查指定坐标像素；
- 失败时保留 PPM + 串口日志到 `build/x86/artifacts/`。

---

## 9. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| Chrome 二进制实际启动需要远超清单的 syscall | 高 | G1 卡住 | 用 `strace -f` 在宿主 Alpine 容器里录全量 syscall 序列；逐条对照 `linux/sys.c`；未实现的先返回 ENOSYS 并打日志 |
| 243 MB rootfs + 3 GB RAM 在 QEMU TCG 下启动过慢 | 高 | 验收超时 | 先在 host 用 KVM 验证 Chrome 可跑；guest 侧先 `--single-process`；rootfs 零拷贝 mmap；必要时 `-enable-kvm` |
| futex 重写引入回归 | 中 | 既有 pthread 测试崩 | 现有 `check-linuxabi-l7/m13d` 先跑；新 futex 保留旧路径开关 `C6OS_FUTEX_V2` |
| MAP_SHARED 与 COW 互相踩 | 中 | 随机丢数据 | 单测覆盖 fork+共享页；`PTE_DEV/COW` 软件位独立；页引用计数单元测试 |
| VBE 在 QEMU 不同版本/`-M pc` 下模式号不一样 | 中 | G2 黑屏 | 先 `VBE 0x4F00` 取控制器信息，再按支持的 mode list 选 800×600×32；失败回退 640×480×32 |
| i8042 与现有 IRQ0 抢占冲突 | 中 | 键盘丢键/系统卡 | IRQ1/IRQ12 只做事件入队，不在 IRQ 里唤醒复杂路径；`sendkey` 做回归 |
| Chrome 对 musl/Alpine 支持本身不完整 | 中 | C1 失败 | 先 `chromium --headless=new --version`；不行换 Alpine 3.19 或 `chromium-headless-shell`；再不行换 Debian rootfs（glibc） |
| 合成器协议实现不全导致 Chrome 拒绝启动 | 高 | G4/G5 延期 | 先跑 `weston-simple-shm`/`wl_demo`；用 `WAYLAND_DEBUG=1` 对比协议日志 |
| 内存不足（Chrome 1.5 GB+） | 高 | OOM | `-m 3072`；`--js-flags=--max-old-space-size=256`；G6 前不开多标签 |
| 时间成本 | 高 | 周期长 | G0–G3 先交付 C2（可见 Chrome）；G4/G5 作为第二阶段；G7/G8 可砍 |
| QEMU `-m 3072` 的内存有一部分在 4 GB 以上的 E820 条目；若 PMM 只做到 4 GB，`sysinfo` 只有约 2.5 GB | 高 | G0 验收 `totalram ≥ 2.9 GB` 直接失败 | G0 就把 `build_dmap` 扩到 8 GB、PMM 收集完整 E820；`g0sysinfo.c` 先打印 E820 分布 |
| 1–4 GB 的 RAM 被现有 DMAP 按 UC 映射 | 高 | 高内存访问奇慢/内存类型别名 | G0 按 E820 重写 DMAP：usable WB、PCI hole/MMIO UC；`port_phys_is_ram` 断言 |
| `fetch-apk.py` 对 Chromium 的虚包/`provides` 解析不全 | 中 | rootfs 缺库，Chrome 起不来 | G0 用 `lddtree`/`readelf -d` 对 Chromium 做依赖闭包审计；缺包手工补 `APKINDEX` 映射 |
| `tools/mkfat.py` 不是通用 FAT16 格式化器，只造单文件测试卷 | 中 | 无法生成 512 MB `/data` | G0 扩展为通用格式化器或改用 host `mkfs.fat -F 16 -s 32`；加构建依赖检查 |
| Chrome profile 放 FAT：SQLite 需要 fcntl 锁/符号链接，FAT 驱动不一定支持 | 中 | Chrome 启动时 "Failed to create SingletonLock" 或数据库损坏 | v1 把 `--user-data-dir=/tmp/chrome`（ramfs），`/data` 只放下载；G6 再评估 FAT 锁 |
| Chrome/Alpine 若使用 AVX/AVX2，而内核只保存 SSE 上下文 | 低 | 用户态随机崩溃/数据错 | 用 `-cpu qemu64`/不暴露 AVX；或实现 XSAVE/AVX 上下文（另立任务） |

---

## 10. 开放问题（开工前定）

1. **Chrome 还是 Chromium**：本文按 Alpine `chromium` 设计；若必须官方
   Chrome（glibc），需要另做 Debian rootfs + glibc NSS，工作量 +2–3 周。
2. **rootfs 交付**：第二块 raw 盘（推荐）vs 放大 initramfs（简单但 BIOS 慢）。
3. **kiosk 优先还是 Wayland 优先**：建议 G3 先交付 C2，G4/G5 再补原生路线。
4. **JPEG 库**：vendor `stb_image.h` vs 链接 Alpine `libjpeg-turbo`。
5. **QEMU 内存**：2 GB 还是 3 GB；取决于 Chrome 实测 RSS。
6. **是否做 DRM**：G7 只在需要 vsync/page-flip 时做。

---

## 11. 明确不做

- 硬件 GPU、EGL/GLES、WebGL、Vulkan；
- Chrome sandbox / namespace / seccomp / user namespace；
- X11/Xorg（VT 子系统缺失，成本高）；
- 音频/视频硬解、摄像头、USB；
- S3 上跑 Chrome（架构不支持）；
- 完整 Wayland 协议（只实现 Chrome 所需子集）；
- 多用户/多 seat/多输出。

---

## 附录 A：代码证据索引（施工前先读）

| 主题 | 文件/行 |
|---|---|
| `/dev` 白名单 | `linux/fs.c:955-973 dev_node_` |
| ioctl 分派（只有 tty/pty/socket） | `linux/sys.c:1121-1198` |
| `sys_mmap2` 文件映射逐页拷贝 | `linux/sys.c:281-366` |
| `msync` 空操作 | `linux/sys.c:663-670` |
| futex 广播唤醒 | `linux/proc.c:4167-4200` |
| VMA/COW | `port/x86_64/mmu.c:1046-1120`、`linux/vma.c` |
| PMM 1 GB 上限 | `port/x86_64/mmu.c:43-120` |
| initramfs 1024 节点 | `linux/fs.c:19-33`、`linux/fs.c:238-301` |
| rootfs 8 MB 目的地 | `port/x86_64/cd0.S`、`tools/mkiso.py:ROOTFS_DST` |
| 内核镜像 <0xA0000 | `port/x86_64/stage2.ld:23-29` |
| x86 DMAP 0–8 GB（RAM WB / MMIO UC） | `port/x86_64/mmu.c:450-484` |
| x86 无 VBE | `port/x86_64/boot16.S`、`port/x86_64/cd0.S` |
| S3 面板/RGB565 | `port/s3/panel.c:114-115,1077-1140` |
| `OsGfx` | `rt/gfx.c`、`include/c6os/gfx.h` |
| SAL/TCP 上限 | `include/los/sal.h:28`、`kernel/net/tcp.c6:20` |
| 设备对象设计 | `docs/linux-abi-portable.md` §6 |
| 里程碑表 | `docs/linux-abi-complete.md` §4 |

## 附录 B：Chrome 启动/调试命令速查

```sh
# 构建带 Chrome 的 rootfs（G0 目标）
make x86-chrome-rootfs
# 起机（3 GB、VBE、第二块 rootfs 盘）
qemu-system-x86_64 -M pc -m 3072 -cpu max -vga std \
  -cdrom build/x86/iso/chrome.iso \
  -drive format=raw,file=build/x86/alpine/chrome-rootfs.img,if=ide,index=1 \
  -drive format=raw,file=build/x86/alpine/chrome-data.img,if=ide,index=3 \
  -serial stdio -monitor unix:/tmp/c6qmp,server,nowait -no-reboot

# guest 内
chromium --version
chromium --headless=new --no-sandbox --disable-gpu \
  --disable-dev-shm-usage --disable-breakpad \
  --user-data-dir=/tmp/chrome --dump-dom 'data:text/html,<h1>hi</h1>'

# Wayland
export XDG_RUNTIME_DIR=/run/user/0
c6wl &
chromium --ozone-platform=wayland --no-sandbox --disable-gpu \
  --user-data-dir=/tmp/chrome --window-size=800,600
```

## 附录 C：设计自评审记录（R1）

| 编号 | 发现 | 处理 |
|---|---|---|
| R1-1 | `F_SEAL_SEAL`/`F_SEAL_FUTURE_WRITE` 值写反 | 已改为 `SEAL=0x1`、`FUTURE_WRITE=0x10` |
| R1-2 | `-m 3072` 的内存有 4 GB 以上 E820 条目，原设计 PMM 只到 4 GB，`sysinfo ≥3 GB` 不可能 | G0 改为 PMM/DMAP 支持 0–8 GB；验收改为 `≥2.9 GB` |
| R1-3 | 现有 DMAP 把 1–4 GB 全按 UC，>1 GB 的 RAM 会被当 MMIO | G0 改为按 E820 映射：usable WB、PCI hole/MMIO UC |
| R1-4 | `port_as_copy` 无法从 PTE 区分 MAP_SHARED，原设计只写"跳过 `LX_VM_SHARED`"但 port 层看不到 VMA | 新增 `PTE_SHARED`（bit 11）；`PTE_DEV`（bit 10）只用于 MMIO |
| R1-5 | 原设计把 futex key 一律用物理地址，私有页 COW 后 PA 变化会丢唤醒 | 私有映射用 `(as_id<<48) + uaddr`，共享映射才用 PA |
| R1-6 | CDP 消息是嵌套 `{method,params:{...}}`，原设计"扁平 JSON"不够 | 改为递归下降 JSON 解析器 |
| R1-7 | Chrome profile 放 FAT 会踩 SQLite 锁/符号链接 | v1 改 `/tmp/chrome`（ramfs），`/data` 只放下载 |
| R1-8 | `fcntl` 只有 fd 标志，SQLite 需要的记录锁缺失 | 新增 A21 + `g1fcntl.c`（G1-7） |
| R1-9 | 1 TB `PROT_NONE` 保留（V8 sandbox）未覆盖 | 新增 `g1map.c`（G1-5）与 `PTE_SHARED`/`MAP_NORESERVE` 设计 |
| R1-10 | TDD 只在总则和里程碑表里散落，缺独立测试设计 | 新增 §7（测试金字塔、目录、用例清单、RED/GREEN 协议、追溯矩阵） |

> 本表是设计自评审，不是外部评审签署；施工前还需一次代码 owner 评审。
