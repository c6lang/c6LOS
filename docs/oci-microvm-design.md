# OCI 镜像 → 微虚机 + runtime shim（变现主线）

> 状态：**设计稿，O1 起施工**。本文是 M11/M12 那条"镜像即固件"线路的产品化延伸：
> 把"能跑一个镜像"变成"客户 CI 里 `docker run` 一行就能跑"。
> 关联：[m11-docker-iso-design.md](m11-docker-iso-design.md)、[linux-abi-complete.md](linux-abi-complete.md)、
> [linux-abi.md](linux-abi.md)、[architecture.md](architecture.md)、[todo.md](todo.md)。

> 施工口径（用户给定，本文所有取舍都服从它）：
> **T4（namespace/cgroup/overlayfs/seccomp）明确不做**；容器 = 一台微虚机，
> 隔离边界由 VMM 提供。本主线**不碰** `linux/` 的语义施工，只碰两件事：
> "怎么把 OCI 镜像变成一台能起的机器"、"宿主侧怎么驱动这台机器并接回容器生态"。

---

## 0. 一句话与商业靶心

一句话：**镜像还是 OCI 镜像，`docker run` 还是 `docker run`，只是跑它的内核不是 Linux，是 c6os。**

### 0.1 三条候选路，为什么选第三条

| 路 | 内容 | 商业价值 | 成本 | 结论 |
|---|---|---|---|---|
| A. 核内做 T4 | namespace/cgroup/overlayfs/seccomp + netlink/veth，让 `dockerd` 真的跑起来 | 低：正面对撞免费的 Linux + gVisor + Kata + Firecracker | 极高：等于把 Linux 的 5 个子系统重写一遍，每步都要追新 | **不做**（[linux-abi-complete.md](linux-abi-complete.md) §8 已定）|
| B. 只做 unikernel 演示 | 镜像 → ISO → qemu 起机（M11 已达成）| 中：能 demo，但没有客户接口 | 低（已基本完成）| **作为地基，不作为产品形态** |
| C. 本文：镜像 → 微虚机 + runtime shim | 容器照旧、镜像照旧，换掉"跑它的是什么"；宿主侧提供 `docker run --runtime=c6r` / containerd shim v2 | 高：客户零改动接入，隔离更强、启动更快、内核可裁剪可测量 | 中：bzImage 壳 + virtio 块/网 + shim，全部是本仓库已有能力的外延 | **主线** |

C 的关键判断：**客户不会为了换内核改自己的 CI。** 谁能提供"`docker run` 一行不变"，
谁就拿到了替换内核的入口。这也是为什么 shim 比内核本身更值钱 —— 它是商业接口。

### 0.2 客户画像（按付费意愿排序）

1. **边缘/工控/网络设备厂商**：要 OCI 交付链（云端 CI 出镜像 → 设备上跑），
   但设备上不想背一个通用 Linux（尺寸、启动时间、认证、许可、可测量性）。
2. **单用途 appliance + 可 attestation**：一个镜像 = 一台机器，镜像摘要就是运行时的全部内容，
   启动可测量、可把摘要绑进 attestation。
3. **自主可控内核需求**：内核（c6）与工具链可交付、可按客户裁剪到 MCU。
4. **教学/研究**：LiteOS 兼容面 + Linux ABI 双面的教学平台（现金流小、成本也小）。

不付钱的方向（写清楚以免走偏）：把 C6OS 做成"能 `docker run nginx` 的通用 Linux 替代"。

---

## 1. 与已有工作的关系（不是从零开始）

| 已有资产 | 在本主线里承担什么 | 还缺什么 |
|---|---|---|
| M11-0..M11-3：El Torito 引导、romfs、FAT16 写、`mkdockeriso.py` | 镜像打包链的 ~80% | 只支持 BIOS 引导；没有 runtime 接口 |
| M12：Alpine 3.20 minirootfs 跑通（动态 musl + 真 busybox）| 证明真实发行版镜像能跑 | 同上 |
| M13–M20（[linux-abi-complete.md](linux-abi-complete.md)）| 决定"镜像里的程序能跑多远" | 与本主线**正交** |
| SAL/ARP/IPv4/TCP（`check-tcp`）+ e1000 | guest 侧网络 | 微虚机里没有 e1000，要 virtio-net；用户态 socket 面在 M18 |
| `boot16`/`cd0`/`stage1`/`stage2` | BIOS 引导路径（开发与"完整设备模型"调试）| 微虚机没有 BIOS |

两条施工线的关系（避免并行打架）：

- **T 线**（linux-abi-complete）：只改 `linux/` 与 `kernel/` 的语义，验收面是"Alpine 程序行为正确"。
- **O 线**（本文）：只改 `port/x86_64/` + 引导 + 宿主侧 `tools/`，验收面是"`docker run` 一行出结果"。
- 唯一会师点：**镜像里的程序能跑多远由 T 线决定**。O 线先把"Alpine 装得下的镜像"做成一等公民，
  T 线按客户要跑的镜像逐包推进（见 linux-abi-complete §0 的 T2/T3 台阶）。

---

## 2. 客户看得见的产品形态

```sh
# 1. 客户 CI 什么都不改
docker build -t app:1 .

# 2. 只换 runtime（O2）
docker run --runtime=c6r --rm app:1

# 3. K8s 里只改 RuntimeClass（O6）
#    RuntimeClass: c6  →  containerd-shim-c6-v2

# 4. 不装 docker 也能直接跑一个 OCI bundle
c6r run --bundle ./oci-bundle --backend qemu-microvm

# 5. 镜像准入检查（出报告，而不是让 guest 崩）
c6r check-image app:1
```

不做的事（写死）：不做 docker daemon、不做 registry 客户端、不把镜像转成自有格式。
**镜像是 OCI 镜像**，拉取/校验/content store/GC 全部复用宿主已有的 docker 或 containerd ——
这是本方案能快速对接生态的根本原因：我们不实现镜像分发，只实现"跑"。

---

## 3. 总体架构

```
                创建期（宿主，Linux）                                运行期（微虚机）
OCI bundle (config.json + rootfs) ─┐
                                   ├─► c6r（shim）
docker/containerd content store ───┘    ├─ 摘要缓存：key = sha256(rootfs + manifest)
                                        ├─ rootfs → 内核镜像容器（cpio，或只读 FAT 卷）
                                        ├─ 写 boot manifest /etc/c6os.json
                                        └─ 选后端：qemu-microvm / cloud-hypervisor / firecracker
                                              │ -kernel c6os-bz.bin -append "console=... root=..."
                                              │ block: cpio 或 rootfs.img
                                              │ serial → 容器 stdout/stderr
                                              ▼
                            ┌──────────────────────────────────────────────┐
                            │ bzImage 壳（port/x86_64/bzkern.S）            │
                            │   boot_params：e820 / cmdline / mem → 0x7000   │
                            │   → stage1（页表/长模式）→ stage2 → os_main    │
                            │ c6os：romfs(FAT 只读卷) 挂 /，FAT 挂 /data     │
                            │   容器模式：pid1 不吞信号 + 退出即关机           │
                            │   ENTRYPOINT → ring3 → 应用                    │
                            │   退出：串口 "\nC6EXIT <code>\n" + 关机          │
                            └──────────────────────────────────────────────┘
                                              │
                                     shim 解析 → 容器退出码 / 日志流
```

### 3.1 一次 `docker run` 的时间预算

| 阶段 | 目标 | 备注 |
|---|---|---|
| shim create：解析 bundle、查缓存 | ≤ 15 ms | 缓存命中只做 stat + 摘要比对 |
| 缓存未命中：rootfs → cpio / FAT 卷 | 3.5 MB ≈ 40 ms（Python 首版），目标 ≤ 10 ms | O3 按 §5.1 判据决定是否换 Go |
| VMM 启动（qemu `-M microvm`，无 BIOS）| 30–60 ms | Firecracker 同类约 10 ms（O5 实测）|
| guest：壳 → 内核 init → romfs 挂载 → execve | ≤ 25 ms | O1 起用串口时间戳量，写进 `[boot] t=...` 行 |
| **端到端到应用第一条输出** | **≤ 120 ms（qemu）/ ≤ 60 ms（fc 类）** | `tools/perf-oci.sh` 固定阈值，只防大退化 |

---

## 4. 关键接口决策

### 4.1 引导契约：伪造一个 Linux 内核（bzImage 壳）

这是整条主线唯一"非做不可、且没有替代品"的技术决策。

**事实**：qemu 的 `-M microvm` 没有 BIOS（不能从 MBR/El Torito 起）；Firecracker、
Cloud Hypervisor 也都只认"Linux 内核镜像"。现有的 BIOS 路径**进不了微虚机**，
所以必须支持 Linux 引导协议（x86 boot protocol）。

做法（O1）：在现有 `flat.bin` 前面加一层最小合法 bzImage 壳 ——
引导段是壳，真正载荷还是我们的 `stage1`：

| 偏移 | 字段 | 值 / 含义 |
|---|---|---|
| `0x1F1` | `setup_sects` | 壳占的 512 字节扇区数 |
| `0x1FE` | `boot_flag` | `0xAA55` |
| `0x202` | header magic | `"HdrS"` |
| `0x206` | `header.version` | `0x020F`（协议 2.15：保证 32 位入口 + e820 在 `boot_params` 里）|
| `0x211` | `loadflags` | bit0 = 1（`LOADED_HIGH`）|
| `0x214` | `code32_start` | `0x00100000` |
| `0x200` | 入口 | `jmp startup_32`（32 位入口）|

被跳进来时的状态（协议 2.15）：32 位保护模式、平坦段、中断关、`%esi` = `boot_params` 物理地址。
壳只做四件事：

1. 从 `boot_params->e820_table`（个数在 `e820_entries`）重建现有 `0x5000` 那张 E820 表，
   替掉 BIOS 路径里的 `int 15h` 收集；
2. 把 `boot_params->hdr.cmd_line_ptr` 指向的 cmdline 拷进 bootinfo 块（`0x7000`）；
3. 打一行心跳：`c6os bzkern: e820=N mem=...K cmdline=...`
   —— 三个后端的差异全靠这行定位（这是 M11 里最值钱的经验：都表现为"没有输出"）；
4. 跳现有 `stage1`（建页表 → 长模式 → `0x11000` → `os_main`）。

**一份 manifest、两条引导路径**：cmdline 只放 4 个开关
（`console=`、`root=`、`init_mode=container`、`poweroff=serial`），
真正的 bundle 描述放在镜像里的 `/etc/c6os.json`（不受 2048 字节 cmdline 限制）。
BIOS 路径（ISO/磁盘，开发与"完整设备模型"调试用）读同一个文件 —— **路径两条、语义一份**。

不做：multiboot（qemu 支持，另外两个后端不支持）、PVH ELF（qemu/CH 支持，Firecracker 不支持）、
kexec、EFI（`efi.c` 现状不参与微虚机路径）。

风险前置：各后端 loader 对协议版本、`setup_sects`、`init_size` 的处理有差别。
O1 的验收面收窄到 **qemu `-M microvm` 起得来 + `file` 认得格式**，三后端矩阵留 O5。

### 4.2 镜像容器格式：两种，按大小选

| rootfs 大小 | 形式 | 理由 |
|---|---|---|
| ≤ 32 MB | cpio + 512 字节扇区头（`tools/mk-initramfs.py` 现有格式）→ 内存里 romfs | 最快，零块设备依赖 |
| > 32 MB | 只读 FAT 卷（`mkfat.py` 生成，`fatfs.c6` 只读路径已实现）→ virtio-blk | 内存装不下，且 3.5 MB Alpine 只是下限不是上限 |

- cpio 的 512 字节前缀格式**保持不变**（内核侧消费逻辑不动）。
- 大镜像的只读卷用 FAT16（FAT32 读属 M11-4 待补）；不做 squashfs/ext4/overlayfs ——
  多一个内核文件系统就是多一条不可测的路径，收益只是省宿主磁盘。

### 4.3 可写语义：构建期重映射，而不是运行期 overlay

容器里"程序要写 `/var/lib/x`"，在没有 overlay 的前提下有三种做法，选第三种：

| 做法 | 代价 | 结论 |
|---|---|---|
| 运行期 overlayfs | 内核里重写一个 Linux 子系统（VFS 大改 + xattr + whiteout）| 不做 |
| tmpfs 顶掉（重启即丢）| 语义假，客户会踩坑 | 只在显式 `--tmpfs` 时用 |
| **构建期重映射**：打包时把可写路径换成指向 `/data` 的符号链接 | 0 内核改动（符号链接解析 M12 已有）| **采用** |

接口：`c6r create --writable /var/lib/app --data-size 64M`。
打包期：`/var/lib/app` → 符号链接 `/data/var/lib/app`（`/data` 是 FAT 数据盘，
宿主可直接 `7z l` / `mount -o loop` 校验）。运行期挂上持久盘就得到"跨重启保留"，
而且这个语义**客户能懂**（就是一块盘），不假装是 union fs。

### 4.4 I/O：串口是唯一必须的通道

| 用途 | 通道 | 说明 |
|---|---|---|
| 容器 stdout/stderr | 串口 → shim 的 pipe | 字节直通，`docker logs` 立刻可用 |
| 容器退出码 | 串口控制行 `\nC6EXIT <code>\n` | 零设备依赖、三后端通用；内核在 pid1 退出时打 |
| 优雅停止（SIGTERM）| O4：第二串口（qemu `isa-serial`）/ vsock（fc）| O2/O3 阶段 `stop` = 硬停（明确写进已知限制）|
| stdin（`-i`）| 串口 RX → 应用 stdin | M12 的行规程已在，`-t` 需要的 raw 模式也已够用 |
| 持久盘 | virtio-blk（微虚机）/ ATA（qemu pc）| VFS 侧同一套 `LOS_Block*`，差别只在驱动 |

退出码走串口而不是 `isa-debug-exit` / vsock，是刻意的：**协议只依赖我们自己的串口驱动**，
不引入任何后端特有设备，一次实现三个后端通吃；等 O4 有了 ctl 通道再考虑搬家。

### 4.5 设备模型：virtio 是硬需求，e1000/ATA 是开发期的事

| 设备 | 现状 | 微虚机需要 |
|---|---|---|
| 控制台 | 16550 串口（`port/x86_64/console.c`）| 够用（qemu `-M microvm` 有 isa-serial；fc/CH 的 serial 也是 16550 兼容）|
| 块 | ATA PIO（`ata.c`）| **virtio-blk（mmio）** —— 新增 |
| 网 | e1000（`e1000.c`）| **virtio-net（mmio）** —— 新增 |
| 时钟 | PIT + TSC 校准 | 不假设 TSC 频率：一律"运行时校准 + 上限保护" |
| 电源 | 无（靠 `-no-reboot` 收尾）| 无 ACPI → `poweroff=serial`：guest 打完 `C6EXIT` 由 shim 杀 VMM |

virtio-mmio 探测：不依赖 ACPI/设备树（微虚机常常没有），**扫两段窗口找 `0x74726976`（"virt"）魔数**
（qemu microvm 在 `0xfeb00000` 起，Firecracker 在 `0xd0000000` 起），
额外允许 `virtio_mmio=0x...` cmdline 覆盖。扫不到就当"没有该设备"继续启动 ——
**内核不能因为缺网卡/缺盘起不来**。

### 4.6 网络与端口映射：不做 netns，做"一台机器一根网线"

- guest 侧：单网卡 + cmdline `ip=10.0.2.15/24 gw=10.0.2.2`（沿用 Linux 的 `ip=` 语法，shim 填、
  内核解析）。这条**不依赖** M18 的 socket 面：内核侧 SAL/TCP 已有 `check-tcp` 验收。
- 宿主侧端口映射：qemu 用 slirp `hostfwd=tcp::8080-:80`；firecracker/CH 用宿主 `iptables DNAT`
  （shim 生成规则，随容器生命周期回收）。
- 明确不做：netns / veth / bridge / 多网卡 / 容器间东西向网络。容器互通 = 走宿主网络栈，
  和"两台 VM 互相访问"一个道理。

---

## 5. shim 的实现口径

### 5.1 语言与分阶段（用测量决定，不拍脑袋）

- **O2 用 Python**（`tools/c6r.py`）：复用 `mk-initramfs.py` / `mkfat.py` 的逻辑，
  一两百行即可端到端，最先见到 `docker run` 一行出结果。
- **O3 测出来再定**：判据写死 —— **缓存命中的 `c6r create` 必须 ≤ 20 ms**。
  达不到就把热路径（cpio 打包 + 摘要 + 缓存）换成 Go 单二进制（单文件分发，客户不必装 Python）；
  达到就不换。**不为了语言本身重写。**

### 5.2 支持度矩阵（OCI runtime spec 子集）

| 命令 | 阶段 | 说明 |
|---|---|---|
| `create` / `start` / `state` / `delete` | O2 ✅ | 与 runc 兼容的 bundle 布局与状态目录 |
| `kill <SIGKILL>` | O2 ✅ | 杀 VMM 进程 |
| `kill <SIGTERM/其他>` | O4 | 需要 ctl 通道；此前降级为 SIGKILL 并打警告 |
| `exec` | **永不支持** | 微虚机里没有"再进一个进程"的通道；`docker exec` 明确报错。要进容器 = 镜像里带 ssh/自建 agent，走网络 |
| `list` / `ps` | O2 ✅ | 读自己的状态目录 |
| stdout / stderr 流 | O2 ✅ | 串口直通 |
| 退出码 | O2 ✅ | `C6EXIT` 行 |
| `--rm` | O2 ✅ | delete 语义 |
| 资源限制（`--memory` / `--cpus`）| O3 部分 | 只映射到 VMM 的 `-m` / `-smp`（超配在 VMM 层被拒），不做 cgroup |
| 卷（`-v host:ct`）| O3 | 见 §4.3：`-v` = 一块 FAT 数据盘；`docker volume` 子命令不实现 |
| stats（CPU/内存）| O6 粗粒度 | guest 定期往串口打一行 `C6STAT`，不是 cgroup 精确值 |

### 5.3 缓存与摘要

`~/.c6os/cache/<sha256>/`，key = sha256(rootfs 树（路径 + 权限 + 内容）+ manifest)。
命中即跳过打包 —— 这是"CI 里反复 run 也秒开"的关键。`c6r gc` 可清，
默认上限可配（不设编译期人为小上限，但要有水位线防磁盘被吃光）。

---

## 6. 里程碑与验收

每个里程碑独立可回归：`make check-oci-*` 绿、文档同步、上一级不退。

| 里程碑 | 内容 | 验收（qemu 真跑） | 打印 |
|---|---|---|---|
| **O1** bzImage 壳 | `port/x86_64/bzkern.S`（setup header + 32 位入口）+ `boot_params` 解析（e820/cmdline/mem→`0x7000`）+ 心跳行；`make x86-bz` | `make check-oci-boot`：`qemu-system-x86_64 -M microvm -m 256M -kernel build/x86/bz/c6os-bz.bin -serial stdio`（**无 `-cdrom`、无 BIOS**）起机，打印 `c6os bzkern: e820=` 与 `c6os microvm alive` | `PASS: oci O1（bzImage 壳：无 BIOS 起机）` |
| **O2** shim v0（qemu 后端）| `tools/c6r.py`：bundle → cpio → `qemu -M microvm -kernel` → 串口流 → `C6EXIT` 解析；容器模式 pid1（不吞信号 + 退出即关机）；`/etc/c6os.json` manifest | `make check-oci-runtime`：**不依赖 docker daemon** —— 造一个 bundle（`config.json` + 静态 ELF）→ `c6r run` → 断言 stdout 内容 + 退出码（含 `exit 7` 用例 + `exit 0` 用例）；另留 `make run-oci-docker` 在有 docker 的宿主上手工验 `docker run --runtime=c6r` | `PASS: oci O2（OCI bundle → 微虚机 → stdout/退出码）` |
| **O3** 设备面与持久化 | virtio-blk（mmio）+ >32 MB 只读 FAT 卷；virtio-net + `ip=` 解析 + slirp `hostfwd`；`--writable` 构建期重映射；摘要缓存 | `make check-oci-dev`：① >32 MB rootfs 从只读卷起机；② guest `ping` 网关 + 内核 TCP 连宿主 slirp 拿 200；③ `--writable /var/lib/x` 写一次、重启后还在（宿主 `7z l` 旁证）| `PASS: oci O3（virtio 块/网 + 可写重映射）` |
| **O4** 生命周期 | ctl 通道（第二串口 / vsock）+ SIGTERM 优雅退出 + 超时强杀；`-i`/`-t`；`C6EXIT` 之外的控制行（`C6READY`、`C6LOGLEVEL`）| `make check-oci-lifecycle`：`stop` 在 3 s 内退出，且应用里装的 signal handler 打了一行；无 handler 时 5 s 强杀 | `PASS: oci O4（停/杀/信号语义）` |
| **O5** 后端矩阵与性能 | `--backend qemu-microvm\|cloud-hypervisor\|firecracker`；`tools/perf-oci.sh`；热路径是否换 Go（§5.1 判据）| `make check-oci-backends`：同一 `c6os-bz.bin` 在三个后端各起一次同一个 Alpine bundle；`make perf-oci` 打端到端 P50/P90 并对照 §3.1 预算 | `PASS: oci O5（三后端 + 启动预算）` |
| **O6** 生态接口 | `containerd-shim-c6-v2`（ttrpc）：create/start/delete/kill/exec(明确拒绝)/stats/shim 生命周期；K8s `RuntimeClass: c6` | `make check-oci-containerd`：有 containerd 时手工验；无 containerd 时跑 shim v2 协议自测（mock ttrpc 客户端）| `PASS: oci O6（containerd shim v2）` |

依赖关系：

```
O1 ──► O2 ──► O3 ──► O4
        │              │
        ├──► O5        └──► O6
```

O3 的网络验收口径**只写"内核 TCP + ping"**，不写 `wget`：
镜像里的 `wget` 要等 T 线的 M18（用户态 socket 面），两条线不互相卡。

---

## 7. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| bzImage 壳被某个后端 loader 拒绝（协议版本 / `setup_sects` / `init_size` 细节）| 中 | O1 卡住 | O1 只以 qemu `-M microvm` 为验收面；三后端差异留 O5；壳保持"最小合法 bzImage"，不加花样字段 |
| 没有 BIOS 后内存洞信息不全 | 中 | 页表/直接映射踩空 | `boot_params->e820_table` 是权威；启动时整表打印（现有 `mmu: e820=` 那行就是这个用途）|
| virtio-mmio 扫描窗口猜错 | 低 | 找不到块设备/网卡 | 扫两段已知窗口 + `virtio_mmio=` cmdline 覆盖；扫不到必须能继续启动（降级而非崩溃）|
| 微虚机里 PIT/TSC 行为差异 | 中 | 启动异常、sleep 漂移 | 时钟一律运行时校准 + 上限保护，不依赖开机前 TSC 频率 |
| 打包 rootfs 的 Python 开销 | 高 | create 延迟超预算 | 摘要缓存 + §5.1 判据换 Go |
| 客户镜像依赖 `/proc`、`/dev/*`、网络 | 高 | "跑不了"的售前/支持成本 | `c6r check-image` 静态扫 ELF 的 INTERP、`/proc` 字样、动态库依赖，**出报告而不是让 guest 崩** |
| 「不支持 `docker exec`」被当缺陷 | 高 | 售前摩擦 | 写进产品口径（§5.2），CLI 里给明确报错文案与替代方案（镜像内 agent / ssh）|
| 与 Linux 的语义差距被当成 bug 报进来 | 高 | 支持成本失控 | 每个 issue 先分类：T 线（语义，进 linux-abi-complete 清单）还是 O 线（引导/工具，进本文清单），不混 |
| 一个容器一台 VM 的资源开销被拿来和 runc 比 | 中 | 定位被带偏 | 口径统一：对比对象是"容器级 VM"（Kata/Firecracker），不是 runc |

---

## 8. 明确不做

- namespace（pid/mnt/net/uts/ipc/user）、cgroup、overlayfs、seccomp-bpf、capabilities —— 隔离交 VMM；
- 运行期 overlay / 分层缓存 / whiteout（构建期扁平化，M11 已定）；
- docker daemon / registry 客户端 / 镜像格式转换 / 内容寻址存储的自研实现；
- `exec` 进运行中的容器；
- 多容器共享一个内核（一台微虚机 = 一个容器，多容器 = 多个 VMM）；
- **S3 上跑 Linux 镜像**：无 MMU，`C6OS_LINUX_ABI=1` 在 Makefile 解析期就 `$(error)`（能力位
  `MMU|USER|IRQ_USER` 全 0）。S3 侧同一打包链、不同目标：cpio + manifest + xtensa 原生代码，
  没有 ELF、没有容器语义；
- 完整 ACPI/PCI 枚举、VGA、输入设备、GPU、音频；
- i386/32 位 ABI。

---

## 9. 施工记录

（每完成一个里程碑补一节：做了什么、踩到什么坑、验收输出。）

### O1 之前（本文建立时的现状盘点）

- 现状实测：`-M microvm` 路径完全不存在（仓库里没有 virtio/vsock/bzImage 相关代码），
  引导只有 BIOS 两条（`boot16` 磁盘 / `cd0` El Torito），块设备只有 ATA PIO，网卡只有 e1000。
- 因此 O1（bzImage 壳）是唯一的前置闸门：它不通，后面全是空谈。
- 镜像打包链已在（`mk-initramfs.py` / `mkfat.py` / `mkiso.py` / `mkdockeriso.py`），
  O2 的工作量主要在 shim 与"容器模式 pid1"这两处，不在打包。
