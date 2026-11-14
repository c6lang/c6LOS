# ESP32-S3 应用镜像格式（实测确认版）

官方文档：`esp-idf/docs/en/api-reference/system/app_image_format.rst`
段偏移公式：seg0 = sizeof(header) + sizeof(seg_header)；
segN = segN-1 + len(N-1) + sizeof(seg_header)。

## 24 字节镜像头

```
偏移  原厂 xiaozhi.bin            含义
0x00  e9                          magic
0x01  07                          segment_count
0x02  02                          SPI 模式 (0=QIO 1=QOUT 2=DIO 3=DOUT)
0x03  4f                          freq|size nibble
0x04  40 9a 37 40                 entry_addr (LE) = 0x40379a40
0x08  ee                          wp_pin 禁用
0x09  00 00 00                    spi_pin_drv
0x0C  09 00                       chip_id = 9 (ESP32-S3)
0x0E  00
0x0F  00
0x10  00 63 00 00                 min/max chip rev
0x14  00 00 00 01                 0x17 = hash_appended
0x18  -- 段表从这里开始
```

## 段头（8 字节）

```
uint32 load_addr   // 段自身的地址
uint32 data_len
```

- **RAM 段**（IRAM/DRAM）：bootloader 把数据拷到 load_addr。
- **flash 段**（DROM 0x3C00_0000 / IROM 0x4200_0000）：bootloader **用 MMU 映射**，
  不拷贝。段数据在**镜像文件**里的偏移 `X` 决定它在 flash 上的物理地址：
  `paddr = 镜像烧录地址 + (X - 0x18)`，然后 `vaddr` 与 `paddr` 按 MMU 页（64 KB）对齐后写表。

## 我们踩出来的四个必要条件（少一个 bootloader 就不认）

1. **`esp_app_desc_t` 必须落在镜像文件偏移 0x20**（魔数 `32 54 cd ab`）——
   `esptool elf2image` 会把 DRAM 段排最前，导致 app_desc 落不到 0x20，只能用
   `tools/mkimg.py` 自己排段。
2. **段表必须连续**：段与段之间不能有空洞。flash 段需要的"页对齐空洞"要**并入前一段的
   data_len**（DROM 是虚拟映射区，加长没有副作用）。
3. **每段的 data_len 要含补齐**：原厂各段长度全是 4 的倍数；我们的 `.vector` 是 0x47A，
   不补 2 字节会让解析器错位 4 字节。
4. **镜像总长度必须是 16 的倍数，且最后 1 字节是校验和**：
   `checksum = 0xEF; 对各段数据（含补齐）逐字节 XOR`，然后补齐到 `(len+1) % 16 == 0`，
   最后放这个校验和字节。做到后 `esptool --chip esp32s3 image-info` 会报
   `Checksum: 0xXX (valid)`。

`tools/mkimg.py` 就是按这四条实现的。

## 本项目的实际布局

```
flash 0x0      原厂 bootloader（我们不重写，用它来建 MMU/cache）
flash 0x8000   我们自己的分区表（factory @ 0x20000, 4MB）
flash 0x20000  我们的 app 镜像（mkimg 生成）
                ├─ 段0 DROM(.flash.rodata) vaddr 0x3C000020，数据在文件偏移 0x20
                ├─ 段1 IROM(.flash.text)   vaddr 0x42030000，数据 → flash 0x30000
                └─ 段2.. RAM 段（.eh_frame/.vector/.text/.data）
```

**注意**：走这条路之后，`tools/flashmon.sh`（把镜像写到 0x0）会覆盖 bootloader 和
分区表，之后再走标准镜像路线时必须重刷这两样。
