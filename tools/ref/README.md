# 复现材料

本目录是 `docs/support-report.md` 里提到的复现材料。

## 1. 分区表

- `partitions.csv` —— 分区表源文件
- `partition-table-4MB.bin` —— 已生成好的二进制（烧到 0x8000）

```
nvs        data/nvs   0x9000   0x6000
phy_init   data/phy   0xF000   0x1000
factory    app        0x20000  0x400000   ← 我们的 app 镜像烧在这里
```

重新生成（需要 IDF 的 `gen_esp32part.py`）：

```sh
python3 $IDF_PATH/components/partition_table/gen_esp32part.py partitions.csv partition-table-4MB.bin
```

## 2. 最小 IDF 参照工程（同一块板子上 IROM 正常）

`idfref/` 可以直接构建：

```sh
export IDF_TOOLS_PATH=/media/winger/inst_26/esp/.espressif
. $IDF_PATH/export.sh
cd idfref && idf.py set-target esp32s3 && idf.py build
```

它当前版本会打印 `SPI_MEM` 前 16 个字和 MMU 前 8 项（用于和我们的固件做对照），
把这个 `app_main` 换成空循环就是一个正常的最小 app。

## 3. 我们的固件（IROM 访问失败）

```sh
# 构建
make C6_ROOT=/home/winger/c6 BUILD=build/iromN2 S3_MAIN=tests/t_s3_irom18.c6 s3

# 生成 IDF 格式镜像（关键：app_desc 必须落在偏移 0x20）
python3 tools/mkimg.py build/iromN2/c6os.elf /tmp/app.bin

# 烧录（三样都要）
esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x0     <原厂 bootloader.bin>
esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x8000  tools/ref/partition-table-4MB.bin
esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0x20000 /tmp/app.bin
```

相关用例：`tests/t_s3_irom8/10/16/17/18.c6`（数据读、寄存器对比、cache 顺序实验）。
