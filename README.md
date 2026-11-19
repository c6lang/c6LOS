# c6LOS —— 用 c6 重构的 LiteOS-M 语义内核（ESP32-S3）

`c6LOS` 是一个独立的实时操作系统仓库：内核用 **c6**（带扩展的 TinyCC 方言）编写，
目标机是 **ESP32-S3**（Xtensa LX7 双核 240MHz、8MB PSRAM、16MB flash、1.85″ SPI 屏）。

- **内核对标**：LiteOS-M（任务/优先级抢占/时间片、信号量、互斥量、事件、队列、内存池、
  软定时器、中断、tick），单地址空间、确定性、静态内存优先。
- **双 API**：`os_*` c6 原生 API（`T!error` / `defer` / 泛型 / 切片 / interface）+
  `LOS_*` LiteOS-M 兼容层（C ABI，可直接编 LiteOS 示例）。
- **构建**：目标机 `c6 → C99 后端 → xtensa-esp-elf-gcc → esptool 镜像`；
  参考机 `c6 → LLVM IR → clang/ld.lld`（x86-64 裸机、qemu-riscv32）。

> 为什么目标机不走 LLVM IR：本机 LLVM 19 的 Xtensa 后端只有 `generic` CPU，不接受
> `-mcpu=esp32s3` / `-mabi`，且 LLD 没有 Xtensa 后端 —— `c6→LLVM IR→S3` 无法链接。
> 详见 [docs/toolchain.md](docs/toolchain.md) §2。

## 状态

**实现阶段**：M0–M5 核心与 M6 Hwi/ISR 安全、M8-5 上板手册已完成；
M7 屏/仪表盘进行中。施工前先读：

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 分层、ABI 规则、对象模型、调度与 IPC 语义、启动链、内存预算 |
| [docs/toolchain.md](docs/toolchain.md) | 目标矩阵、C99 路径、LLVM IR 路径、编译器补丁、运行库、lint |
| [docs/api.md](docs/api.md) | 原生 `os_*` 与兼容 `LOS_*` 的签名、返回值、错误码映射 |
| [docs/porting-esp32s3.md](docs/porting-esp32s3.md) | S3 启动/镜像/控制台/时钟/中断/双核/屏、上板阶梯 |
| [docs/testing.md](docs/testing.md) | 四层验证矩阵、确定性测试、验收门禁 |
| [docs/todo.md](docs/todo.md) | 任务板（M0–M8、依赖、验收、状态） |
| [docs/board-s3.md](docs/board-s3.md) | 真板烧写/串口/中断/故障排查（实测流程） |
| [docs/vfs-net-design.md](docs/vfs-net-design.md) | M9/M10 设计与施工记录：VFS/romfs/ramfs/FAT、SAL/ARP/IPv4/UDP/TCP/e1000 |
| [docs/m11-docker-iso-design.md](docs/m11-docker-iso-design.md) | M11：docker 镜像 → 可引导 ISO → 虚拟机运行（可读写数据盘、LiteOS 接口优先） |
| [docs/oci-microvm-design.md](docs/oci-microvm-design.md) | OCI 镜像 → 微虚机 + runtime shim（变现主线：bzImage 壳、virtio、`docker run --runtime=c6r`、containerd shim v2） |

## 已知硬件（实测）

```
Chip:      ESP32-S3 (QFN56) revision v0.2
Cores:     Dual Core Xtensa LX7 + LP core, 240 MHz
PSRAM:     Embedded 8 MB (AP_3v3)
Flash:     16 MB, quad, 3.3 V (ef 4018)
Crystal:   40 MHz
USB:       303a:1001 USB JTAG/serial debug unit → /dev/ttyACM0
MAC:       cc:8d:a2:ec:99:c4
Screen:    1.85″（SPI；面板 IC/引脚待核对）
```

> 上板前必须先把原厂 16MB flash 完整备份（`esptool read-flash`），见 porting 文档。

## 构建（施工后成立）

```sh
make toolchain-info     # C6_ROOT / 编译器提交 / xtensa-esp-elf-gcc / esptool 检测
make check-host         # host 单测（38 项，虚拟时钟/异步抢占）
make check-all          # check-host + x86-64 裸机 + qemu-riscv32 构建
make s3                 # ESP32-S3 镜像（build/s3/c6os.bin）
make flash              # esptool 烧写（要求已有原厂备份）
make backup-flash       # 烧写前备份 16MB flash（原厂备份已丢失，需重下官方固件）
```

工具链依赖：`C6_ROOT` 指向 c6 源码树（默认 `../superc`）、`xtensa-esp-elf-gcc` 16.x、
`clang` ≥ 19 + `ld.lld`（参考机）、`qemu-system-x86_64`、`qemu-riscv32`、`esptool` ≥ 5。

## 目录

```
include/c6os/   C ABI 与配置（port / compat / rt 共用）
include/los/    LiteOS-M 兼容头
kernel/         c6 内核：task/sched/tick/swtmr/sem/mux/event/queue/membox/mem/hwi
compat/         LOS_* 薄壳（C 符号，无 module 前缀）
rt/             目标侧自研运行库（mem*、软除法、err 状态、pthread 8 入口）
port/           port.h + sim / x86_64 / rv32 / s3 四个移植层
boards/         板级 profile（xiaozhi-s3-spi / …）
app/            shell / demo / screen
tests/          test 块用例（四层共用）
tools/          c6cc.sh / mkrt.sh / espimg.py / lint-*.sh / install-xtensa.sh
```

## 非目标

LiteOS-A 的进程/MMU/POSIX、文件系统、动态加载、网络中间件；内核内的 `@T` GC、
`async/co`、闭包、`f".."`。

## 许可证

本仓库：Apache-2.0。目标侧运行库 `rt/` 为自研，不静态链接 LGPL 的 `libtcc1`；
`libtcc1` 只出现在 host 单测中（作为工具链运行时）。
