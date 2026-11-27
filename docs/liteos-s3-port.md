# LiteOS-M on ESP32-S3（同板对比基线）进展

目标：在同一块 ESP32-S3（Xtensa LX7 双核 240MHz）上运行 c6os 和 LiteOS-M，用同一 CCOUNT/工作负载 harness 比较功能、切换、IPC、tick/实时性，要求 c6os 对齐或超越。

## 1. 上游支持现状（已核实）

- OpenHarmony `kernel_liteos_m` 的 Xtensa 端口只有 **lx6（ESP32）**；官方明确回复 **不支持 LX7/ESP32-S3**，需要参考 lx6 自行移植。
- 参考板配置在 `device/qemu/esp32/liteos_m`（GN + xtensa-esp32-elf-gcc，`-mlongcalls -mtext-section-literals`）。
- 因此“同板 LiteOS-M”不是现成可跑基线，**必须先做 LX7/S3 端口**；上游没有可下载的 S3 镜像。

## 2. 已完成的可行性构建（本地 /tmp，未入库）

- 拉取 `kernel_liteos_m`（GitHub 镜像）与 `device_qemu/esp32` 配置。
- 用 `xtensa-esp32s3-elf-gcc -mabi=windowed -mlongcalls -mtext-section-literals -Os` 成功编译：
  - `kernel/src/*.c`、`kernel/src/mm/*.c`
  - `arch/xtensa/lx6/gcc/*.c` + `*.S`（windowed ABI 在 S3 上可编译）
  - `arch/xtensa/common/los_common_interrupt.c`
  - `utils/los_debug.c`、`los_error.c`、`los_hook.c`
  - securec 的 `memcpy_s.c`/`memset_s.c`/`strncpy_s.c`
- 说明“源码可编译”这一关已过；尚未完成 S3 启动、向量、时钟/中断和链接。
- 已进一步完成 **最小可构建基线**：`bench/liteos_m_s3/` 提供 S3 target_config、windowed start.S、LiteOS 向量/链接脚本、USB 控制台和 demo 任务；`make` 生成 `build/liteos_s3.elf`（text 23,956B / data 35,152B / bss 17,336B），`make image` 生成 `build/liteos_s3.bin`（当前 27,200B，esptool 校验通过，entry `_start`、vector 位于 0x403C9000；heap 放到 BSS，避免 ROM 加载段与 0x3fceb170 冲突）。
- c6os 侧已生成同口径 S3 镜像：`build/s3perf`（`tests/t_perf.c6`，CCOUNT 微基准）和 `build/s3rt`（`tests/t_rt.c6`，唤醒/切换/tick 抖动），与 `bench/liteos_m_s3` 的 `bench.c` 使用同一组操作和 CCOUNT 口径。
- 因 t_rt 的 BSS 比默认镜像多几十字节，S3 链接期栈保护从 8KB 调整为 7.75KB（`_bss_end + 0x1F00 <= 0x3FCEF000`）；仍保留约 7.7KB 主栈余量。
- 新增 `bench/check_s3_images.py` 静态预检：校验 c6os 默认/perf/rt 与 LiteOS-M bench 四个 S3 镜像的入口、`_vecbase/_init_start`(0x403C9000)、向量段 VMA、BSS 与栈顶边界；当前四项全部 `OK`，输出 `PASS: s3 static preflight`。
- **真板已跑通**：ESP32-S3 USB-Serial/JTAG 输出 `LiteOS-M S3 boot`、`kernel init rc=00000000`、`bench start rc=00000000`，并输出 `yield: 592`、`sem_post+pend: 143`、`queue_put+get: 434`、`heap_alloc+free: 622` cycles/op（CCOUNT 40MHz）。c6os 默认镜像也已在同板输出 `c6os s3 boot` / `c6os: alive`。
- 当前问题：c6os S3 `t_perf` 镜像只输出 `c6os s3 boot`，未输出 `after init`/bench；同板 c6os perf 数字尚未采集，正在排查其启动/输出路径。

## 3. 仍需移植的部分

1. **target_config.h（S3）**：`OS_SYS_CLOCK=40000000`（CCOUNT）、tick 100Hz、`OS_TICK_INT_NUM=6`、任务/IPC 上限、heap 区间。
2. **启动/链接**：S3 IRAM/DRAM 布局、`.vector`、`_bss`、栈、ROM 二级引导约定；LiteOS lx6 的 `los_exc.S`/`los_dispatch.S` 需确认 LX7 向量/窗口溢出/PS 行为。
3. **控制台/板级**：USB-Serial/JTAG 或 UART0 的 `printf`/`putc`，复用 c6os `port/s3/console.c` 的寄存器初始化经验。
4. **中断/定时器**：S3 的 `CCOMPARE0`/`INTENABLE`/level-1 向量与 lx6 的差异；tick 中断号、`HalHwiInit`、`OsSetVector`。
5. **linker/board main**：`LOS_KernelInit`、`LOS_Start`、idle 任务、heap 初始化。
6. **securec**：至少 `memcpy_s/memset_s/strncpy_s`，后续按链接未定义符号补齐。

## 4. 同板对比验收（待基线可跑后执行）

| 指标 | 方法 | 目标 |
|---|---|---|
| 功能 | 同一组任务/IPC/内存/定时器用例（LOS API 子集） | c6os 全部通过，语义一致 |
| 任务切换 | 两任务 ping-pong，CCOUNT 取 N 次 min/avg/max | c6os ≤ LiteOS-M |
| sem/queue 唤醒 | 阻塞→post→恢复，CCOUNT | c6os ≤ LiteOS-M |
| tick 抖动 | 100/1000 个 tick 的 CCOUNT 间隔 | c6os max 偏差 ≤ LiteOS-M |
| 内存 | alloc/free 32/128/1024B，CCOUNT | c6os ≤ LiteOS-M |
| 代码/内存 | `size` text/data/bss | 记录差异，不强制 |

注意：LiteOS-M 的 lx6 端口使用 windowed ABI，c6os 的 S3 端口使用 call0；两者不能在同一镜像混链，必须各自独立镜像、同一块板分别烧写。

## 5. 下一步

1. 写 S3 `target_config.h` 与最小 linker/start，先让 LiteOS-M 在 S3 跑起 `LOS_Start` + idle + 一个打印任务。
2. 再补 timer/HWI，跑通 `LOS_TaskDelay` 与 sem。
3. 最后接入与 c6os 相同的 benchmark（`tests/t_perf.c6`/`t_rt.c6` 的等价 C 版本），收集同板数据。
4. 硬件不在线时，以上只能在构建/静态层面推进；上板是最终验收的必要条件。
