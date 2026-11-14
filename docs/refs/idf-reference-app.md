# 可用的 IDF 参照 app（二分 IROM 问题的基准）

## 为什么需要它

我们自己的固件跳进 flash 时取指/数据读都返回 0，而**原厂固件和任何 IDF app 都能正常从
flash 执行**。为了定位差异，需要一个"最小、干净、一定能跑"的 IDF app 作为对照物。

## 环境（已装好，工具链在数据盘）

```sh
export IDF_TOOLS_PATH=/media/winger/inst_26/esp/.espressif   # 6.2G，放数据盘
. /media/winger/inst_26/esp/esp-idf/export.sh
```

- IDF 源码：`/media/winger/inst_26/esp/esp-idf`（v6.0.1）
- 工具链/虚拟环境：`/media/winger/inst_26/esp/.espressif`
  （原来是 `~/.espressif`，因为 `/home` 满而迁到数据盘；`tools/dbg.sh` 已跟着改）
- 踩过的坑：IDF 5.3 的 venv 不适用于 6.0.1；`xtensa-esp-elf esp-15.2.0` 曾因安装中断
  而缺 `cc1`，删掉该目录重装即可

## 最小 app

`/tmp/idfref/`：`app_main` 里循环 `printf("IDFREF alive n=%d\n")`。
**实测真板输出正常**：

```
IDFREF alive n=0
IDFREF alive n=1
...
```

⇒ 这块板子的 IROM 取指完全正常，问题在我们自己的镜像/启动路径。

## 寄存器对照（IDF 参照 app 运行中 vs 我们的 app）

同一批地址（EXTMEM 0x600C4000+0x100、SPI_MEM 0x60002000+0x80、
PMS 0x60026028+0x30、MMU 0x600C5000+0x80 共 140 个）逐字 diff，只剩 9 处不同：

```
0x600C402C  idf=0x3c010000 ours=0x3c800000   DCACHE_SYNC_ADDR（噪声）
0x600C40C8  idf=0x00000000 ours=0x00000001   访问计数器（噪声）
0x600C40CC  idf=0x00000000 ours=0x00000001
0x600C40D4  idf=0x00000030 ours=0x00000d55   计数器（噪声）
0x600C40D8  idf=0x0000007e ours=0x000034e7
0x60002008  idf=0x00608000 ours=0x016ca008   ★ SPI_MEM_CTRL 不同
0x60002014  idf=0x00030103 ours=0x80000000   ★ SPI_MEM_CLOCK 完全不同
0x600C5000  idf=0x00004000 ours=0x00000002   ★ MMU[0]
0x600C500C  idf=0x00004000 ours=0x00000003   ★ MMU[3]
```

两个 **★** 结论：

1. **MMU 表项**：IDF app 里未使用的表项是 `0x4000`（bit14=valid + 页 0）；
   我们被 bootloader 映射的表项是 `2`/`3`（**没有 bit14**）。
2. **SPI 控制器**：`SPI_MEM_CLOCK` 在 IDF app 里是 `0x00030103`，我们的是 `0x80000000`
   —— **时钟配置完全不同**，这正是 cache 从 flash 读不回数据的直接嫌疑。

## 下一步

把这两个寄存器按 IDF 的值写回去（尤其 `SPI_MEM_CLOCK`），确认写入是否生效
（回读 + 信标），然后再做数据读/取指测试。
