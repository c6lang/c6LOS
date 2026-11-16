#ifndef C6OS_ERRNO_H
#define C6OS_ERRNO_H

/* 内核内部的错误码：函数一律返回 0 或 -errno（POSIX 风格），
 * LiteOS 兼容面在出口再翻译（见 docs/vfs-net-design.md §2.2）。
 * 抽成独立头是因为 VFS 和块设备层都要用，不该让块设备去 include vfs.h。 */

#define OS_E_NOENT  2
#define OS_E_BADF   9
#define OS_E_NOMEM  12
#define OS_E_EXIST  17
#define OS_E_ISDIR  21
#define OS_E_INVAL  22
#define OS_E_ROFS   30
#define OS_E_NOSPC  28
#define OS_E_NOTDIR 20
#define OS_E_NOTEMPTY 39
#define OS_E_OPNOTSUPP 95

#endif
