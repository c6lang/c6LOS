#ifndef C6OS_VFS_H
#define C6OS_VFS_H

#include "c6os/types.h"
#include "los/los_fs.h"

/* 内核内部的 VFS（LiteOS-M 兼容面在 include/los/los_fs.h）。
 *
 * 上限一律编译期常量：M9 的定位是"把语义做对"，容量留给块设备文件系统
 * （M9-3）。后端目前只有 romfs（只读）；ramfs 见 docs/todo.md M9-1。 */

#define OS_MAX_MOUNTS  8
#define OS_MAX_FILES   32
#define OS_FS_TYPE_MAX 8

/* errno（内部统一用 -errno 返回，LOS_Fs* 出口原样透出） */
#define OS_E_NOENT  2
#define OS_E_BADF   9
#define OS_E_NOMEM  12
#define OS_E_EXIST  17
#define OS_E_ISDIR  21
#define OS_E_INVAL  22
#define OS_E_ROFS   30

/* 只读镜像型文件系统的描述。buf 必须在挂载期间一直有效（romfs 不拷贝）。 */
typedef struct {
    void *buf;
    u32   len;
} OsFsImage;

typedef struct {
    u32 used;
    char path[LOS_FS_PATH_MAX];   /* 挂载点："/"、"/tmp"…… */
    char type[OS_FS_TYPE_MAX];    /* "romfs" / "ramfs" */
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

extern OsMount g_os_mounts[OS_MAX_MOUNTS];
extern OsFile  g_os_files[OS_MAX_FILES];

#endif
