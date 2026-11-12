#ifndef C6OS_HEAP_H
#define C6OS_HEAP_H

#include "c6os/types.h"

/* 简单 best-fit 堆：块头 16B，双向线性扫描，释放时与相邻空闲块合并。
 * 单核、关中断使用；M5 再接 per-CPU 锁与统计。 */
typedef struct OsHeap {
    u8 *base;
    u32 size;
    u32 used;
    u32 peak;
    u32 magic;
} OsHeap;

void os_heap_init(OsHeap *h, void *mem, u32 size);
void *os_heap_alloc(OsHeap *h, u32 n);
int os_heap_free(OsHeap *h, void *p);
u32 os_heap_used(OsHeap *h);
u32 os_heap_peak(OsHeap *h);
u32 os_heap_max_free(OsHeap *h);

void os_sys_heap_init(void *mem, u32 size);
u32 os_sys_heap_used(void);
u32 os_sys_heap_peak(void);
u32 os_sys_heap_max_free(void);

#endif
