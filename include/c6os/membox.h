#ifndef C6OS_MEMBOX_H
#define C6OS_MEMBOX_H

#include "c6os/types.h"

/* 定长内存块池：O(1) 分配/释放，块头复用为 free-list next。 */
typedef struct OsMembox {
    u8 *base;
    u32 blk_size;
    u32 blk_count;
    u32 free_count;
    void *free_list;
    u32 magic;
} OsMembox;

int os_membox_init(OsMembox *mb, void *storage, u32 blk_size, u32 blk_count);
void *os_membox_alloc(OsMembox *mb);
int os_membox_free(OsMembox *mb, void *p);
u32 os_membox_free_count(OsMembox *mb);

#endif
