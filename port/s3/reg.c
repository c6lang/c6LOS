/* ESP32-S3 的寄存器读写（简单封装，保证编译器不把访问重排掉）。 */
#include "c6os/types.h"
#include "c6os/reg.h"

u32 os_reg_rd(u32 addr)
{
    return *(volatile u32 *)addr;
}

void os_reg_wr(u32 addr, u32 val)
{
    *(volatile u32 *)addr = val;
}
