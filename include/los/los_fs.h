#ifndef _LOS_FS_H
#define _LOS_FS_H

#include "los/los_typedef.h"

/* OpenHarmony LiteOS-M 的 VFS 兼容面（`LOS_Fs*` 一族）。
 *
 * 返回约定：>= 0 成功（read/write 返回字节数），< 0 为 -errno（POSIX 风格）。
 * 只偏离 LiteOS 的一处，理由见 docs/vfs-net-design.md §2.2：内核内部和
 * Linux ABI 两侧都要 errno 语义，只在出口翻译一次最不容易错。
 *
 * 当前后端：romfs（只读，包 newc cpio）。ramfs/FAT 见 docs/todo.md M9-1/M9-3。 */

#define LOS_FS_PATH_MAX 64
#define LOS_FS_NAME_MAX 32

/* open 标志 */
#define LOS_O_RDONLY 0x0000
#define LOS_O_WRONLY 0x0001
#define LOS_O_RDWR   0x0002
#define LOS_O_CREAT  0x0040
#define LOS_O_EXCL   0x0080
#define LOS_O_TRUNC  0x0200
#define LOS_O_APPEND 0x0400

/* lseek whence */
#define LOS_SEEK_SET 0
#define LOS_SEEK_CUR 1
#define LOS_SEEK_END 2

/* stat/dirent 的类型 */
#define LOS_FS_TYPE_FILE 0u
#define LOS_FS_TYPE_DIR  1u

typedef struct {
    UINT32 size;
    UINT32 type;      /* LOS_FS_TYPE_FILE / LOS_FS_TYPE_DIR */
    UINT32 mode;      /* 预留：权限位；M9 只区分只读/可写 */
    UINT32 mtime;     /* 预留：romfs/ramfs 都返回 0 */
} LosFsStat;

/* statfs 的结果：df 读的就是 bsize/blocks/bfree/bavail。 */
typedef struct {
    UINT32 bsize;     /* 基本块大小（字节） */
    UINT32 blocks;    /* 总块数 */
    UINT32 bfree;     /* 空闲块数 */
    UINT32 bavail;    /* 非特权用户可用块数 */
    UINT32 files;     /* 总 inode 数（0 = 不适用） */
    UINT32 ffree;     /* 空闲 inode 数 */
} LosFsStatFs;

typedef struct {
    CHAR name[LOS_FS_NAME_MAX];
    UINT32 type;
    UINT32 size;
} LosFsDirent;

/* dev: 块设备名（ramfs 传空串）；path: 挂载点（"/" 或 "/tmp"）；
 * fsType: "romfs" / "ramfs"；data: 后端私有（romfs 传 OsFsImage*）。 */
INT32 LOS_FsMount(const CHAR *dev, const CHAR *path, const CHAR *fsType,
                  VOID *data);
INT32 LOS_FsUmount(const CHAR *path);

INT32 LOS_FsOpen(const CHAR *path, UINT32 flags);
INT32 LOS_FsClose(INT32 fd);
INT32 LOS_FsRead(INT32 fd, VOID *buf, UINT32 len);
INT32 LOS_FsWrite(INT32 fd, const VOID *buf, UINT32 len);
INT32 LOS_FsLseek(INT32 fd, INT32 off, UINT32 whence);

INT32 LOS_FsStat(const CHAR *path, LosFsStat *st);
INT32 LOS_FsStatFs(const CHAR *path, LosFsStatFs *st);
INT32 LOS_FsReaddir(const CHAR *path, UINT32 idx, LosFsDirent *de);

INT32 LOS_FsUnlink(const CHAR *path);
INT32 LOS_FsMkdir(const CHAR *path);
INT32 LOS_FsRmdir(const CHAR *path);
INT32 LOS_FsRename(const CHAR *from, const CHAR *to);
INT32 LOS_FsTruncate(const CHAR *path, UINT32 size);
INT32 LOS_FsSync(INT32 fd);

#endif
