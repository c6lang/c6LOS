# ESP32-S3 真板操作手册（c6os）

适用板卡：ESP32-S3（QFN56 v0.2，双核 Xtensa LX7，8MB PSRAM，16MB flash，
40MHz 晶振），USB-Serial/JTAG 直连（`303a:1001` → `/dev/ttyACM0`）。

## 1. 工具链

```sh
export C6=<c6 仓库根>
TOOL=~/.c6os/toolchain/xtensa-esp-elf/bin
$TOOL/xtensa-esp32s3-elf-gcc --version     # gcc 16.x
esptool version                            # 5.x
```

## 2. 构建

```sh
cd /home/winger/rtos-c6
make C6_ROOT=$C6 BUILD=build/s3irq S3_MAIN=tests/t_s3_irq.c6 s3
# 产物：build/s3irq/c6os.bin（esptool elf2image --ram-only-header）
```

`S3_MAIN` 必须是带 `void os_main(void)` 的 c6 文件；`S3_EXTRA_C6` 追加内核
源文件（默认 task/tick/timer/timeout/heap；IPC 用例追加 sem/mutex/event/queue/membox）。

## 3. 烧写（每轮流程）

板子按 **BOOT → RESET → 松开 BOOT** 进下载模式，然后：

```sh
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 \
  --before usb-reset --after no-reset write-flash 0x0 build/s3irq/c6os.bin

# 复位并让 USB 重新枚举（不要用普通 RESET，USB 会掉线）
sudo -n esptool --chip esp32s3 --port /dev/ttyACM0 \
  --before default-reset --after hard-reset read-mac
```

- `--before usb-reset` 在本板比 `no-reset` 可靠；`--after hard-reset` 后
  `/dev/ttyACM0` 会以新设备号重新出现，等 1~2 秒再抓串口。
- 镜像烧在 **0x0**，会覆盖原厂 bootloader；没有完整 flash 备份时不要指望
  “回原厂”，需重新下载官方固件包整片烧写。

## 4. 串口

115200 8N1，`winger` 不在 `dialout`，命令加 `sudo -n`。
最小抓取脚本（DTR/RTS + 非阻塞读）见仓库历史提交；要点：

```python
termios.tcsetattr(fd, TCSANOW, 115200 8N1 raw)
fcntl.ioctl(fd, 0x5416, struct.pack('i', 0x006))   # DTR|RTS
```

启动通常打印两次 `c6os s3 boot`（USB 重新枚举/复位），随后才是测试 banner。

## 5. 已接外设/中断

- 控制台：USB-Serial/JTAG `EP1_REG` 轮询写；`port_init` 重新抢回 PHY、
  使能时钟/PAD，等待枚举。
- 定时器：TIMG0 看门狗在时钟使能后关闭；`CCOMPARE0 = CCOUNT + 400000`
  （100Hz），level-1 中断走 IRAM 向量表 `+0x300`。
- VECBASE：先清 `PMS_CORE_0_VECBASE_OVERRIDE`（0x600C11C0 低 2 位），
  并清 `PS.UM`（ROM 入口是 user mode），否则 level-1 走 0x340 用户向量。
- 陷阱：`EXCCAUSE != 4` 走 `s3_trap` 打印 cause/pc/ps 后停机。
- 输入：`port_getc()` 读 `EP1_REG`，用 `EP1_CONF.SERIAL_OUT_EP_DATA_AVAIL`
  门控；目前主机写入能置位，但 shell 读取仍不稳定（见 docs/todo.md）。

## 6. 常用验证

```sh
make ... S3_MAIN=tests/t_s3_irq.c6 s3       # PASS: s3 irq tick
make ... S3_MAIN=tests/t_s3_preempt.c6 s3   # PASS: s3 preempt
make ... S3_MAIN=tests/t_s3_jitter.c6 s3    # PASS: s3 jitter
make ... S3_MAIN=tests/t_s3_ipc.c6 s3       # ALL PASS: ipc
```

## 7. 故障排查

| 现象 | 处理 |
| --- | --- |
| 串口 0 字节 | 确认已 `hard-reset` 并等枚举；DTR/RTS 置位；检查是否烧到 0x0 |
| `SHA-256 comparison failed ... boot anyway` | `--ram-only-header` 的正常提示，可忽略 |
| 复位后掉 USB | 用 `--before usb-reset` / 拔插 USB；本板普通 RESET 不恢复 |
| 启动约 300ms 被复位 | TIMG0 时钟未使能就关看门狗；确认先置 `SYSTEM_PERIP_CLK_EN0` bit13 |
| 中断不触发 | 查 VECBASE override、PS.UM、INTENABLE bit6、CCOMPARE0 重装 |

原厂 16MB flash 备份已丢失；恢复原厂需官方固件包整片烧写（见 docs/todo.md）。
