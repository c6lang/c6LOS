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

/* 叶子函数：不需要栈帧，编译器应该只生成 addi/retw（没有 entry）。
 * 用它区分"是不是 entry 指令在 IROM 里出问题"。 */
__attribute__((section(".flash.text"), noinline))
u32 s3_win_flash_leaf(u32 a)
{
    return a + 7u;
}

u32 s3_asm_flash_42(void);      /* 手写在 flash 段里的 movi+retw（无 entry） */

u32 s3_win_flash42_chain(void)
{
    return s3_asm_flash_42();
}

u32 s3_win_flash_leaf_chain(u32 a)
{
    return s3_win_flash_leaf(a);
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

/* ---- 最小 PHY 初始化（M1） ------------------------------------------
 * register_chipv7_phy(init_data, cal_data, cal_mode) 是 libphy 的初始化主入口
 * （IDF 的 esp_phy_enable() 最终也是调它）。cal_mode: 0=部分校准 1=不校准 2=完整校准。
 * 校准数据先放内存（我们还没有 NVS 落盘）。
 */
typedef struct { u8 v[1904]; } phy_cal_data_t;      /* esp_phy_calibration_data_t */

extern const u8 s3_phy_init_data[128];              /* port/s3/phy_init_data.c */
int register_chipv7_phy(const void *init_data, void *cal_data, int cal_mode);

static phy_cal_data_t g_phy_cal;

int s3_win_phy_enable(void)
{
    return register_chipv7_phy(s3_phy_init_data, &g_phy_cal, 2 /* PHY_RF_CAL_FULL */);
}

u32 s3_win_phy_cal_word(u32 i)
{
    return ((const u32 *)(const void *)g_phy_cal.v)[i & 0x1FFu];
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
