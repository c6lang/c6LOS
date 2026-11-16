#ifndef _LOS_BLOCK_H
#define _LOS_BLOCK_H

#include "los/los_typedef.h"

/* LiteOS-M 风格的块设备层（M9-2 落地，见 docs/vfs-net-design.md §2.4）。
 *
 * buf 是**内核虚拟地址**（x86-64 恒等映射 / S3 内部 RAM 指针），不是一个
 * 物理地址 —— 这样驱动侧不需要知道调用者是谁，也符合 c6 的跨边界规则。
 */

#define LOS_MAX_DEVICES  8
#define LOS_DEV_NAME_MAX 8

typedef INT32 (*LosDevRead)(UINT32 dev, UINT32 blk, UINT32 cnt, VOID *buf);
typedef INT32 (*LosDevWrite)(UINT32 dev, UINT32 blk, UINT32 cnt,
                             const VOID *buf);

/* 返回 >= 0 是设备号；< 0 是 -errno。 */
INT32 LOS_DeviceRegister(const CHAR *name, UINT32 blkSize, UINT32 blkCount,
                         LosDevRead rd, LosDevWrite wr);
INT32 LOS_DeviceUnregister(const CHAR *name);
INT32 LOS_DeviceGet(const CHAR *name);
INT32 LOS_DeviceCount(VOID);

INT32 LOS_BlockRead(UINT32 dev, UINT32 blk, UINT32 cnt, VOID *buf);
INT32 LOS_BlockWrite(UINT32 dev, UINT32 blk, UINT32 cnt, const VOID *buf);
UINT32 LOS_BlockSize(UINT32 dev);
UINT32 LOS_BlockCount(UINT32 dev);

#endif
