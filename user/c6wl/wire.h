/* c6wl 与 wl_demo 共用的 Wayland wire 常量（自包含，不链接 libwayland）。 */
#ifndef C6WL_WIRE_H
#define C6WL_WIRE_H
#include <stdint.h>

#define WL_MAX_MSG 4096u

/* 全局注册的 name（c6wl 固定分配；客户端 bind 时按 interface 匹配）。 */
#define C6WL_NAME_COMPOSITOR 1u
#define C6WL_NAME_SHM        2u
#define C6WL_NAME_SEAT       3u
#define C6WL_NAME_OUTPUT     4u
#define C6WL_NAME_XDG_WM     5u
#define C6WL_NAME_SUBCOMP    6u
#define C6WL_NAME_DATA_MGR   7u

/* wl_shm format */
#define WL_SHM_FORMAT_ARGB8888 0u
#define WL_SHM_FORMAT_XRGB8888 1u

/* 对象类型 */
enum {
    C6WL_T_DISPLAY = 1,
    C6WL_T_REGISTRY,
    C6WL_T_COMPOSITOR,
    C6WL_T_SHM,
    C6WL_T_SHM_POOL,
    C6WL_T_BUFFER,
    C6WL_T_SEAT,
    C6WL_T_POINTER,
    C6WL_T_KEYBOARD,
    C6WL_T_OUTPUT,
    C6WL_T_XDG_WM,
    C6WL_T_XDG_SURFACE,
    C6WL_T_XDG_TOPLEVEL,
    C6WL_T_SURFACE,
    C6WL_T_CALLBACK,
    C6WL_T_SUBCOMPOSITOR,
    C6WL_T_DATA_MGR,
    C6WL_T_DUMMY
};

static inline uint32_t wl_word(uint32_t size, uint16_t opcode)
{
    return ((size & 0xFFFFu) << 16) | opcode;
}

#endif
