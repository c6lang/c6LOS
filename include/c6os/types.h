#ifndef C6OS_TYPES_H
#define C6OS_TYPES_H

/* 纯 C 类型，port / compat / rt / c6 共用。
 * 规则：跨 C ABI 只允许 void* / u32 / u64 / 函数指针（见 docs/architecture.md §3）。 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed   int       i32;
typedef signed   long long i64;

#define C6OS_NAME_MAX 16

#endif
