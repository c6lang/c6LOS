#ifndef C6OS_TRAP_H
#define C6OS_TRAP_H

#include "c6os/types.h"

/* Xtensa 异常帧（M0 只用于 panic 打印；字段顺序与 port/s3/traps.S 一致）。 */
typedef struct OsTrapFrame {
    u32 pc;
    u32 ps;
    u32 exccause;
    u32 excvaddr;
    u32 a[16];
} OsTrapFrame;

#endif
