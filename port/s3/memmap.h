#ifndef C6OS_S3_MEMMAP_H
#define C6OS_S3_MEMMAP_H

/* ESP32-S3 内存边界（IDF soc/esp32s3/include/soc/soc.h）。
 * 注意：低 IRAM/DRAM 被 ROM 自用；原厂二级引导器链接在
 * IRAM 0x403C8700 / DRAM 0x3FCE2820，我们取同区域并留余量。 */
#define S3_IRAM_LOW   0x403C9000u
#define S3_IRAM_HIGH  0x403E0000u
#define S3_DRAM_LOW   0x3FCE3000u
#define S3_DRAM_HIGH  0x3FD00000u

/* 内核栈顶：留 4KB 给 ROM/异常。 */
#define S3_STACK_TOP  0x3FCFF000u

#endif
