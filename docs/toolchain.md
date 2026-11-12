# 工具链与构建（ESP32-S3 + 参考机）

> 状态：设计定稿 · 相关：[architecture.md](architecture.md) · [porting-esp32s3.md](porting-esp32s3.md)

---

## 1. 两条构建路径

```
                    ┌─ 参考机 x86-64 / rv32 ────────────────────────────┐
kernel/*.c6 ──c6──> │ LLVM IR（--emit-llvm）→ clang/ld.lld → qemu     │
                    └───────────────────────────────────────────────────┘
                    ┌─ 目标机 ESP32-S3 ────────────────────────────────┐
kernel/*.c6 ──c6──> │ C99（-c -o dir/）→ xtensa-esp32s3-elf-gcc → esptool │
                    └───────────────────────────────────────────────────┘
```

同一份 `.c6` 源码，两个后端。**目标机不用 LLVM IR**，原因见 §2。

---

## 2. 为什么 S3 不走 c6→LLVM IR（实测证据）

本机 clang 19.1.4（`/usr/lib/llvm-19`）虽然 `--print-targets` 列出 `xtensa - Xtensa 32`，
但该后端不可用于 ESP32-S3：

| 测试 | 结果 |
|---|---|
| `clang --target=xtensa-esp32s3-elf -mcpu=esp32s3 -c x.c` | `error: unsupported option '-mcpu=' for target 'xtensa-esp32s3-elf'` |
| `clang --target=xtensa-esp32s3-elf -mabi=call0 -c x.c` | `error: unsupported option '-mabi='` |
| `llc -march=xtensa -mcpu=help` | 只有 `generic`，特性只有 `density` |
| `ld.lld --version` / `--help \| grep xtensa` | LLD 19 无 Xtensa 后端（无可链接目标） |
| `clang --target=xtensa-unknown-elf` | `error: unknown target triple` |

即：LLVM 19 的 Xtensa 只是**骨架后端**（无 ESP32-S3 CPU 模型、无 call0/windowed ABI 选择、
无寄存器窗口溢出向量），且没有链接器。`c6→LLVM IR→S3 二进制` 在此工具链上不成立。

可选替代与取舍：

| 方案 | 结论 |
|---|---|
| **c6 C99 后端 → Espressif xtensa-esp-elf-gcc** | **采用**。C99 后端与 native 共用前端和同一套测试语料；ABI、寄存器窗口、FPU、PSRAM 全交给厂商编译器 |
| Espressif `esp-clang`（LLVM fork，含 Xtensa+lld）吃我们的 `.ll` | 不采用：我们的 IR 按 LP64 建模，Xtensa 是 ILP32 + windowed ABI，跨 ABI 的 IR 需要重写类型层；收益低、风险高 |
| 自己写 Xtensa 后端/链接器 | 不采用：超出内核重构范围 |

> 因此 C0 编译器补丁（typed declare、`--emit-llvm-tu`）只影响**参考机**路径；
> S3 路径不依赖 C0，可在 C0 合入前先上板。

---

## 3. 目标矩阵

| 目标 | triple / 编译器 | 后端 | 标志 | 验证 |
|---|---|---|---|---|
| host | `x86_64-linux-gnu` | LLVM IR | `--emit-llvm` + `clang -fuse-ld=lld` + `-ltcc1` | 逻辑单测、虚拟时钟 |
| x86-64 裸机 | `x86_64-unknown-none` | LLVM IR | `-mno-sse -mno-red-zone -nostdlib` + `boot.S` | qemu-system-x86_64，真抢占 |
| rv32 | `riscv32-unknown-elf` | LLVM IR | `-march=rv32imac_zicsr_zifencei -mabi=ilp32` | qemu-riscv32（ILP32 回归） |
| **S3** | `xtensa-esp32s3-elf-gcc` | **C99** | `-mabi=call0 -Os -ffreestanding -fno-builtin -nostdlib -mlongcalls -mtext-section-literals` | 真板（USB-Serial/JTAG） |

> rv32 用 `-mabi=ilp32`（软浮点）而不是 `ilp32f`：内核核心不需要 FPU 帧，
> qemu-riscv32 回归更接近纯整数调度路径（S3 的 FPU 帧另算）。

---

## 4. S3 构建流水线

```sh
C6=${C6_ROOT:-../superc}; C6CC=$C6/tcc/c6

# 1) 每个 TU 出 C99（-c 表示每 TU 一个 .c）
mkdir -p build/s3/c99
$C6CC -B $C6/tcc -c -o build/s3/c99/ kernel/*.c6 compat/*.c6 app/*.c6

# 2) 每个 TU 编成对象
for c in build/s3/c99/*.c; do
  xtensa-esp32s3-elf-gcc -mabi=call0 -Os -ffreestanding -fno-builtin -fno-common \
    -mlongcalls -mtext-section-literals -Wall -Wextra \
    -Iinclude -c "$c" -o "build/s3/obj/$(basename "${c%.c}").o"
done

# 3) 汇编端口
xtensa-esp32s3-elf-gcc -mabi=call0 -c port/s3/*.S -o ...

# 4) 链接（脚本给出 IRAM/DRAM 段与入口）
xtensa-esp-elf-ld -T port/s3/link.ld -nostdlib -o build/s3/c6os.elf build/s3/obj/*.o

# 5) 生成 esp_image 并烧写（0x0 是我们的二级引导器）
esptool --chip esp32s3 elf2image --flash-mode dio --flash-size 16MB \
  --flash-freq 80m -o build/s3/c6os.bin build/s3/c6os.elf
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 build/s3/c6os.bin
```

要点：

- `c6 -c -o dir/` 每个 `.c6` 产出一个 `.c`，文件名与输入同名；头文件不参与。
- 生成的 C 只要求 `<string.h>` 的 `memcpy/memmove/memset`；目标侧由 `rt/c6rt.c`
  提供，`-ffreestanding` 下不拉 glibc。
- `-mabi=call0`：无寄存器窗口溢出异常，上下文切换只存 a0–a15；已实测：
  `-mabi=call0` 生成 `call0`/`ret.n`，默认 windowed 生成 `call8`/`retw.n`。
- **FPU 必须进陷阱帧**：该 GCC 没有 `-msoft-float`/`-mno-fpu`/`-mfloat-abi` 开关，
  `float` 直接生成 `add.s f0` 等指令；陷阱帧保存 `f0–f15` + `FCR/FSR`。
  内核核心自身禁止浮点（lint），浮点只出现在应用任务。
- 不链接 `libtcc1`（LGPL + LP64 签名）；目标侧运行库是 `rt/c6rt.c`。
- 链接脚本的 IRAM/DRAM 边界以 IDF `soc/esp32s3/include/soc/soc.h` 的
  `SOC_IRAM_LOW/HIGH`、`SOC_DRAM_LOW/HIGH` 为准，写死在 `port/s3/memmap.h` 并在
  `make s3-info` 里打印核对。

### 镜像与烧写

- `elf2image` 生成 `0xE9` 头 + 段表 + 校验/哈希；ROM 从 `0x0` 读取并搬到段头地址。
- 不需要分区表：我们不加载 app、不解析分区；整个内核就是二级引导器。
- 烧写前：`make backup-flash`（16MB 全片读回，存 `build/backup/flash-16MB.bin`）。
- 进入下载模式：USB-Serial/JTAG 下 `esptool --before default-reset` 可用（实测能连）；
  必要时按住 BOOT 再复位。
- 回滚：`esptool write-flash 0x0 build/backup/flash-16MB.bin`。

---

## 5. 工具链安装

```sh
make install-xtensa
```

`tools/install-xtensa.sh`：

- 版本：`xtensa-esp-elf 16.1.0_20260609`（Espressif crosstool-NG）
- 资产：`xtensa-esp-elf-16.1.0_20260609-x86_64-linux-gnu.tar.xz`（84.9 MB）
- 下载：`https://github.com/espressif/crosstool-NG/releases/download/esp-16.1.0_20260609/…`
- 安装：`~/.c6os/toolchain/xtensa-esp-elf`；`$PATH` 加 `…/bin`
- 校验：`sha256sum`（首次下载后写进 `tools/toolchain.sha256`）
- 只支持 x86_64-linux；其他平台给出显式错误。

安装后自检（`make toolchain-info`）：

```sh
xtensa-esp32s3-elf-gcc --version       # 实际打印 xtensa-esp-elf-gcc 16.1.0
xtensa-esp32s3-elf-gcc -mabi=call0 -c -x c /dev/null -o /dev/null
xtensa-esp32s3-elf-ld --version
esptool version
```

> 该工具链是统一包（`xtensa-esp-elf-*` 二进制 + `xtensa-esp32s3-elf-*` 前缀包装），
> 用 `xtensa-esp32s3-elf-gcc` 选核；**不要**传 `-mcpu=esp32s3`（不被接受）。

---

## 6. 参考机（LLVM IR）路径

```sh
$C6CC -B $C6/tcc --emit-llvm -o build/host/x.ll kernel/x.c6
clang -fuse-ld=lld -o build/host/test build/host/*.ll -L $C6/tcc -ltcc1
```

- host 必须链 `libtcc1`（`tcc_th()`、`err` 状态、切片/字符串运行时）。
- rv32 必须加一个 `.S` 端口提供 `_start`/`ecall`/`mret`，并用
  `-Wl,-Ttext=0x10000 -Wl,-e,_start`；已验证可跑（打印 banner，rc=0）。
- x86-64 裸机沿用 `examples/boot/` 的 512B 引导器与 `boot.ld`，payload 放 `0x400000`。
- **LP64×ILP32 规则**（architecture.md §3）在两条路径上同样强制。

### C0 编译器工作包（只服务参考机）

| 编号 | 内容 | 验收 |
|---|---|---|
| C0-1 | 跨 TU `declare` 携带类型（`declare i64 @f(ptr, i64)`），不再默认 `i64()` | 两个 TU 的函数指针/返回值类型一致；`tests/abi/` 通过 |
| C0-2 | `--emit-llvm-tu`：每个输入 TU 一个 `.ll`，`-c -o dir/` 语义 | 3 个 TU 分别出 IR，链接后跑通 |
| C0-3 | 外部全局带 size/align（现在是 `external global i8`） | 跨 TU 读写 `int`/指针不越界 |
| C0-4 | 不支持的构造给**显式错误**（inline asm、可变参数定义等） | 负向用例报错而非生成坏 IR |

回退：C0 未合入时，参考机测试用「合并所有 TU 成一个 `.c6` 再出 IR」的脚本过渡；
S3 的 C99 路径与此无关，不受影响。

---

## 7. 目标侧运行库 `rt/c6rt.c`

| 符号 | 作用 |
|---|---|
| `memcpy/memmove/memset/memcmp` | 编译器生成代码所需 |
| `__c6_err_*` | 每任务错误槽（TCB `keys[]`） |
| `__c6_th_*` | 线程/任务绑定（S3 用 `port_cpu_id` + 当前 TCB） |
| `__c6_slice_*` | 切片越界检查（可关） |
| `__c6_udiv/__c6_umod/__c6_sdiv/__c6_smod` | 软除法（Xtensa 无硬件除） |
| `__c6_f2i/__c6_i2f` | 软浮点转换（若 gcc 运行库未提供） |
| `pthread_create/join/detach/self/equal/exit/key_create/key_delete/getspecific/setspecific` | 8 入口 + 2 个 key 入口，供 `tcc_th()` |

门禁：`xtensa-esp-elf-nm -u build/s3/obj/*.o | grep -v '^$'` 必须落在白名单内
（`tools/lint-syms.sh`），否则说明某处偷偷用了 libc/编译器运行库。

---

## 8. lint 脚本

| 脚本 | 检查 |
|---|---|
| `lint-ilp32.sh` | kernel/app 里禁止 `long`/`size_t`/`long double`/按值结构体跨 port |
| `lint-c6.sh` | `.c6` 内禁止 `#include <…>`、`main(`、`asm(`、`@T`、`async`、闭包、f-string |
| `lint-port.sh` | `port/` 不得 `#include` 内核内部头；只允许 `include/c6os/port.h` |
| `lint-syms.sh` | 未定义符号白名单（nm -u） |
| `lint-size.sh` | 编译期 `sizeof`/`offsetof` 断言 + `xtensa-esp-elf-size` 预算门禁 |

---

## 9. 排错

| 现象 | 原因/处理 |
|---|---|
| `error: unsupported option '-mcpu='` | host clang 或统一驱动；S3 必须用 `xtensa-esp32s3-elf-gcc`（前缀选核，不传 `-mcpu`） |
| `undefined reference to __c6_*` | 忘了编 `rt/c6rt.c` 或没进链接列表 |
| `region IRAM overflowed` | 内核超预算；先看 `size`，把只读表放 DRAM/或减功能 |
| esptool 连不上 | 板子不在下载模式；`--before default-reset`，或按住 BOOT 复位 |
| 上电无输出 | 控制台选错（USB-Serial/JTAG vs UART0）；先确认 `port_console` 选择 |
| 一开中断就复位 | 看门狗未关 / 陷阱向量地址错 / 栈未对齐 16B |
| 链接后 entry 不对 | `elf2image` 用 ELF 的入口；确认 `_start` 在 IRAM 且 `-e _start` |
