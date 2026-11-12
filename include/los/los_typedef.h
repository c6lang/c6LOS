#ifndef _LOS_TYPEDEF_H
#define _LOS_TYPEDEF_H

/* LiteOS-M 兼容层的最小类型/常量集（与 c6 内核的 u8/u16/u32 分开）。 */
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef signed int INT32;
typedef void VOID;
typedef char CHAR;
typedef int BOOL;

#define LOS_OK 0u
#define LOS_WAIT_FOREVER 0xFFFFFFFFu

#endif
