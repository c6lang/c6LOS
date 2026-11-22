#ifndef C6OS_VFS_H
#define C6OS_VFS_H

#include "c6os/types.h"
#include "los/los_fs.h"
#include "los/los_block.h"
#include "c6os/errno.h"

/* 内核内部的 VFS（LiteOS-M 兼容面在 include/los/los_fs.h）。
 *
 * 上限一律编译期常量：M9 的定位是"把语义做对"，容量留给块设备文件系统
 * （M9-3）。后端目前只有 romfs（只读）；ramfs 见 docs/todo.md M9-1。 */

#define OS_MAX_MOUNTS  8
#define OS_MAX_FILES   32
#define OS_FS_TYPE_MAX 8


/* 只读镜像型文件系统的描述。buf 必须在挂载期间一直有效（romfs 不拷贝）。 */
typedef struct {
    void *buf;
    u32   len;
} OsFsImage;

typedef struct {
    u32 used;
    char path[LOS_FS_PATH_MAX];   /* 挂载点："/"、"/tmp"…… */
    char type[OS_FS_TYPE_MAX];    /* "romfs" / "ramfs" / "fatfs" */
    char dev[LOS_DEV_NAME_MAX];   /* 块设备名（fatfs 用；内存后端留空） */
    void *priv;                   /* 后端私有数据 */
} OsMount;

typedef struct {
    u32 used;
    u32 refs;                     /* dup 用（M9-4） */
    u32 mnt;
    u32 flags;
    u32 pos;
    u32 size;                     /* open 时记下来，供 SEEK_END */
    void *fh;                     /* 后端句柄 */
} OsFile;

int  os_str_eq(const char *a, const char *b);
u32  os_str_len(const char *s);
void os_str_cpy(char *dst, const char *src, u32 max);
int  os_str_prefix(const char *s, const char *p);   /* s 是否以 p 开头 */

/* 路径合法性：以 '/' 开头、长度 < LOS_FS_PATH_MAX、不含 ".." 段 */
int os_vfs_path_check(const char *path);
/* 最长前缀匹配；rel = 挂载点之后的部分（不带前导 '/'，根为 ""） */
int os_vfs_lookup(const char *path, u32 *mnt, char *rel, u32 relsz);

int os_vfs_mount(const char *dev, const char *path, const char *type, void *data);
int os_vfs_umount(const char *path);

int os_vfs_file_alloc(u32 mnt, u32 flags, void *fh);
OsFile *os_vfs_file_get(int fd);
void os_vfs_file_free(int fd);

/* romfs.c6：newc cpio 只读后端 */
int romfs_mount(OsMount *m, void *data);
int romfs_umount(OsMount *m);
int romfs_open(OsMount *m, const char *rel, u32 flags, void **fh);
int romfs_close(OsMount *m, void *fh);
int romfs_read(OsMount *m, void *fh, u32 pos, void *buf, u32 len);
int romfs_stat(OsMount *m, const char *rel, LosFsStat *st);
int romfs_readdir(OsMount *m, const char *rel, u32 idx, LosFsDirent *de);

/* fatfs 的挂载参数：分区起始扇区（0 = 从卷首） */
typedef struct {
    u32 lba;
} OsFsPart;

/* fatfs.c6：FAT16/32 只读后端（M9-3） */
int fatfs_mount(OsMount *m, void *data);
int fatfs_umount(OsMount *m);
int fatfs_open(OsMount *m, const char *rel, u32 flags, void **fh);
int fatfs_close(OsMount *m, void *fh);
int fatfs_read(OsMount *m, void *fh, u32 pos, void *buf, u32 len);
int fatfs_write(OsMount *m, void *fh, u32 pos, const void *buf, u32 len);
int fatfs_stat(OsMount *m, const char *rel, LosFsStat *st);
int fatfs_readdir(OsMount *m, const char *rel, u32 idx, LosFsDirent *de);
int fatfs_unlink(OsMount *m, const char *rel);
int fatfs_mkdir(OsMount *m, const char *rel);
int fatfs_rmdir(OsMount *m, const char *rel);
int fatfs_rename(OsMount *m, const char *from, const char *to);
int fatfs_truncate(OsMount *m, const char *rel, u32 size);
int fatfs_statfs(OsMount *m, LosFsStatFs *st);

/* ramfs.c6：可读写内存后端（M9-1） */
int ramfs_mount(OsMount *m, void *data);
int ramfs_umount(OsMount *m);
int ramfs_open(OsMount *m, const char *rel, u32 flags, void **fh);
int ramfs_close(OsMount *m, void *fh);
int ramfs_read(OsMount *m, void *fh, u32 pos, void *buf, u32 len);
int ramfs_write(OsMount *m, void *fh, u32 pos, const void *buf, u32 len);
int ramfs_stat(OsMount *m, const char *rel, LosFsStat *st);
int ramfs_readdir(OsMount *m, const char *rel, u32 idx, LosFsDirent *de);
int ramfs_unlink(OsMount *m, const char *rel);
int ramfs_mkdir(OsMount *m, const char *rel);
int ramfs_rmdir(OsMount *m, const char *rel);
int ramfs_rename(OsMount *m, const char *from, const char *to);
int ramfs_truncate(OsMount *m, const char *rel, u32 size);
int ramfs_statfs(OsMount *m, LosFsStatFs *st);

extern OsMount g_os_mounts[OS_MAX_MOUNTS];
extern OsFile  g_os_files[OS_MAX_FILES];

#endif
