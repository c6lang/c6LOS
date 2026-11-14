/* c6OS 最底层的寄存器读写（各 arch 都实现一份）。
 * 有了它，测试/驱动就不必依赖 panel.c 里的 s3_reg32/s3_reg_wr。 */
#ifndef C6OS_REG_H
#define C6OS_REG_H

#include "c6os/types.h"

u32  os_reg_rd(u32 addr);
void os_reg_wr(u32 addr, u32 val);

#endif /* C6OS_REG_H */
