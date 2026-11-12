#ifndef C6OS_S3_MEMMAP_H
#define C6OS_S3_MEMMAP_H

/* ESP32-S3 内存边界（IDF soc/esp32s3/include/soc/soc.h）。 */
#define S3_IRAM_LOW   0x40370000u
#define S3_IRAM_HIGH  0x403E0000u
#define S3_DRAM_LOW   0x3FC88000u
#define S3_DRAM_HIGH  0x3FD00000u

/* 内核栈顶：留 4KB 给 ROM/异常。 */
#define S3_STACK_TOP  0x3FCFF000u

#endif
