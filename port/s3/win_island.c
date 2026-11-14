/* windowed ABI 孤岛：这里放 IDF 的闭源库和它们的 C 胶水（它们只能是 windowed）。
 * 本文件用 -mabi=windowed 单独编译，其余代码仍是 call0，两者之间只通过
 * port/s3/win_bridge.S 里的手写 trampoline 来往。
 */
#include "c6os/types.h"

u32 s3_win_add(u32 a, u32 b)
{
    return a + b + 1000u;
}

/* 深递归：用来逼出 Xtensa 的寄存器窗口溢出/欠载（4 个窗口，5 层以上必然溢出）。
 * 如果我们的窗口溢出/欠载向量写错了，这里就会跑飞。 */
/* flash 段里的已知图案：用来客观测量 IROM 映射有没有偏移（只读不执行） */
__attribute__((section(".flash.rodata"), used))
const u8 g_flash_pat[8] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u};

u32 s3_win_flash_add(u32 a, u32 b);   /* 前向声明（下面取它的地址） */

u32 s3_win_flash_addr(void)
{
    return (u32)(const void *)s3_win_flash_add;
}

u32 s3_win_flash_pat(void)
{
    return (u32)(const void *)g_flash_pat;
}

/* 放在 flash 段（IROM）里的 windowed 函数：验证"从 flash 执行 windowed 代码"本身
 * 是否可行（和 libphy 无关）。 */
__attribute__((section(".flash.text"), noinline))
u32 s3_win_flash_add(u32 a, u32 b)
{
    return a + b + 7u;
}

/* IRAM 里的包装：由编译器负责跨到 flash（-mlongcalls 会生成 l32r+callx8） */
u32 s3_win_flash_chain(u32 a, u32 b)
{
    return s3_win_flash_add(a, b);
}

u32 s3_win_deep(u32 n)
{
    if (n == 0u) {
        return 1u;
    }
    return s3_win_deep(n - 1u) + 1u;
}

/* libphy.a 里的入口（闭源，windowed ABI，只能从孤岛里调）。
 * 需要链接 libphy.a 的构建才定义 S3_WITH_LIBPHY。 */
#ifdef S3_WITH_LIBPHY
u32 phy_get_rf_cal_version(void);

u32 s3_win_phy_cal(void)
{
    return phy_get_rf_cal_version();
}
#else
u32 s3_win_phy_cal(void)
{
    return 0xDEADu;      /* 没链 libphy：用来验证桥本身 */
}
#endif
