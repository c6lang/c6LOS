/* c6LOS 的 lwIP 平台适配头（arch/cc.h）
 *
 * lwIP 的 `lwip/arch.h` 会 `#include "arch/cc.h"` 来找这些平台相关的东西：
 * 定宽整数类型、结构体打包方式、诊断/断言钩子、随机数。
 * IDF 那份（components/lwip/port/esp32xx/include/arch/cc.h）依赖 sdkconfig.h、
 * esp_log.h 和 FreeRTOS 的 arch/sys_arch.h，我们这里一个都不能用，所以自己写。
 *
 * 实测（docs/wifi-plan.md §207）：换上这份 cc.h 之后，lwIP 2.x 的核心
 * （ip4/tcp/udp/pbuf/mem/memp/netif/etharp/dhcp/icmp/dns/init/sys）用
 * `xtensa-esp32s3-elf-gcc -mabi=call0 -Os -ffreestanding` 全部编译通过，
 * 一个警告都不需要特殊处理 —— 说明 lwIP 这一层是可移植的、不需要 FreeRTOS。
 *
 * 钩子的实现放在 port/s3/lwip/c6_lwip_port.c。
 */
#ifndef C6_LWIP_ARCH_CC_H
#define C6_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdlib.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* lwIP 要求的基础类型（arch.h 不再自己定义这些） */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;
typedef int       sys_prot_t;

/* printf 格式（lwIP 的调试宏会拼 "u"/"d"/"x"，不能带长度修饰） */
#define U8_F  "u"
#define S8_F  "d"
#define X8_F  "x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

/* 结构体打包：Xtensa 的 GCC 支持 __attribute__((packed))，
 * 所以协议头（etharp/ip4/tcp）可以按位摆放，不需要逐字段 memcpy。 */
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT   __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* 诊断 / 断言 / 随机数：走 c6_lwip_port.c 里的实现（不引 stdio） */
#define LWIP_PLATFORM_DIAG(x)   do { c6lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { c6lwip_assert(x); } while (0)
#define LWIP_RAND()             ((u32_t)c6lwip_rand())

void         c6lwip_diag(const char *fmt, ...);
void         c6lwip_assert(const char *msg);
unsigned int c6lwip_rand(void);

#endif /* C6_LWIP_ARCH_CC_H */
