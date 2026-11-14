/* windowed ABI 孤岛：这里放 IDF 的闭源库和它们的 C 胶水（它们只能是 windowed）。
 * 本文件用 -mabi=windowed 单独编译，其余代码仍是 call0，两者之间只通过
 * port/s3/win_bridge.S 里的手写 trampoline 来往。
 */
#include "c6os/types.h"

/* 闭源库（windowed）专用的 mem*：库里对 memcpy/memset 的引用被 objcopy
 * 改名成 win_*，这样就不会和主世界（call0）那份混用。 */
void *win_memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    while (n--) { *d++ = *s++; }
    return dst;
}

void *win_memset(void *dst, int c, u32 n)
{
    u8 *d = (u8 *)dst;

    while (n--) { *d++ = (u8)c; }
    return dst;
}

void *win_memmove(void *dst, const void *src, u32 n) { return win_memcpy(dst, src, n); }

int win_memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;

    while (n--) {
        if (*x != *y) { return (int)*x - (int)*y; }
        x++;
        y++;
    }
    return 0;
}

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

/* 12 个参数 → 编译器会用 call12；递归 8 层必然压到 _win_ovf12/_win_unf12。 */
u32 s3_win_deep12(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f,
                  u32 g, u32 h, u32 i, u32 j, u32 k, u32 l)
{
    if (a == 0u) {
        return b + c + d + e + f + g + h + i + j + k + l;
    }
    return s3_win_deep12(a - 1u, b, c, d, e, f, g, h, i, j, k, l) + 1u;
}

/* 单参数入口，方便 call0 侧的桥调用 */
u32 s3_win_deep12_entry(u32 n)
{
    return s3_win_deep12(n, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u);
}

u32 s3_win_deep(u32 n)
{
    if (n == 0u) {
        return 1u;
    }
    return s3_win_deep(n - 1u) + 1u;
}

/* ---- ROM 控制台转发 ---------------------------------------------------
 * ROM 里的 ets_printf 按 ROM 自己的配置输出（USB-Serial/JTAG 或 UART0），
 * 不依赖我们 port_init 里的 USB 初始化 —— 串口"坏掉"时这是最可靠的通道。
 * 注意 ROM 是 windowed ABI，必须从孤岛（windowed）里调，call0 直接调会跑飞。
 */
int ets_printf(const char *fmt, ...);

void s3_win_log(const char *s)
{
    ets_printf("%s", s);
}

/* ---- 最小 PHY 初始化（M1） ------------------------------------------
 * register_chipv7_phy(init_data, cal_data, cal_mode) 是 libphy 的初始化主入口
 * （IDF 的 esp_phy_enable() 最终也是调它）。cal_mode: 0=部分校准 1=不校准 2=完整校准。
 * 校准数据先放内存（我们还没有 NVS 落盘）。
 */
#ifdef S3_WITH_LIBPHY
typedef struct { u8 v[1904]; } phy_cal_data_t;      /* esp_phy_calibration_data_t */

extern const u8 s3_phy_init_data[128];              /* port/s3/phy_init_data.c */
int register_chipv7_phy(const void *init_data, void *cal_data, int cal_mode);

static phy_cal_data_t g_phy_cal;

/* 校准模式：0 = PHY_RF_CAL_PARTIAL（上电后的常规路径，轻）
 *           2 = PHY_RF_CAL_FULL（首次/换环境用，耗时长、电流大） */
#ifndef S3_PHY_CAL_MODE
#define S3_PHY_CAL_MODE 0
#endif

int s3_win_phy_enable(void)
{
    /* IDF 在调 register_chipv7_phy 之前会把 efuse 里的 MAC 填进 cal_data.mac
     * （见 esp_phy/src/phy_init.c）。这里先写死成这块板自己的 MAC
     * cc:8d:a2:ec:99:c4；以后改成从 efuse 读。 */
    g_phy_cal.v[4] = 0xccu;
    g_phy_cal.v[5] = 0x8du;
    g_phy_cal.v[6] = 0xa2u;
    g_phy_cal.v[7] = 0xecu;
    g_phy_cal.v[8] = 0x99u;
    g_phy_cal.v[9] = 0xc4u;
    return register_chipv7_phy(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);
}

u32 s3_win_phy_cal_word(u32 i)
{
    return ((const u32 *)(const void *)g_phy_cal.v)[i & 0x1FFu];
}
#else
int s3_win_phy_enable(void)
{
    return -1;      /* 没链 libphy */
}

u32 s3_win_phy_cal_word(u32 i)
{
    (void)i;
    return 0u;
}
#endif

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
