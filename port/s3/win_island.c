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
/* ROM：0x40005c58，填 g_phyFuns 函数表（见 esp32s3.rom.ld） */
/* ROM 0x40005c58：返回 PHY 的函数表指针（调用者负责存进 g_phyFuns）；
 * 注意 ROM 是 windowed，返回值在 a10，本文件也是 windowed 编译，直接收就对。 */
u32 phy_get_romfuncs(void);
/* **正确的入口**：libphy.a 自己导出的 phy_get_romfunc_addr()（.text, 偏移 0x88）。
 * IDF 的 include/phy.h 写明："Initialize function pointer table in PHY library…
 * must be called before register_chipv7_phy"。ROM 里那个 phy_get_romfuncs()
 * 是另一回事（它填 ROM 内部的表，跟我们的 g_phyFuns 无关）。 */
void phy_get_romfunc_addr(void);
/* IDF 用它让 PHY 初始化时**不要抢走内部 USB PHY**（否则 USB-CDC 立刻掉枚举，
 * 串口和 esptool 一起失联，什么都看不到）。必须在 register_chipv7_phy 之前调用。 */
void phy_bbpll_en_usb(u32 enable);

void s3_win_log(const char *s)
{
    ets_printf("%s", s);
}

void s3_win_trace(const char *s);

/* ---- 最小 PHY 初始化（M1） ------------------------------------------
 * register_chipv7_phy(init_data, cal_data, cal_mode) 是 libphy 的初始化主入口
 * （IDF 的 esp_phy_enable() 最终也是调它）。cal_mode: 0=部分校准 1=不校准 2=完整校准。
 * 校准数据先放内存（我们还没有 NVS 落盘）。
 */
typedef struct { u8 v[1904]; } phy_cal_data_t;      /* esp_phy_calibration_data_t */

#ifdef S3_WITH_LIBPHY
extern const u8 s3_phy_init_data[128];              /* port/s3/phy_init_data.c */
int register_chipv7_phy(const void *init_data, void *cal_data, int cal_mode);

static phy_cal_data_t g_phy_cal;

/* 校准模式：0 = PHY_RF_CAL_PARTIAL（上电后的常规路径，轻）
 *           2 = PHY_RF_CAL_FULL（首次/换环境用，耗时长、电流大） */
#ifndef S3_PHY_CAL_MODE
#define S3_PHY_CAL_MODE 0
#endif

/* IDF 在 register_chipv7_phy() 之前必须先给 modem 开时钟（见 esp_phy/src/phy_init.c
 * 的 esp_phy_enable：esp_phy_common_clock_enable() + phy_module_enable()）。
 * S3 上就是往 SYSCON_WIFI_CLK_EN(0x60026014) 塞两组位：
 *   WIFI_BT_COMMON = 0x78078F   （WiFi/BLE 共用时钟）
 *   PHY_EN         = 0x400000   （PHY 校准模块时钟）
 * 没有这些时钟，库里的轮询会永远等下去 —— 实测就是"卡死"。 */
static void s3_win_phy_clock_enable(void)
{
    volatile u32 *clk = (volatile u32 *)0x60026014u;

    *clk = *clk | 0x78078Fu;
    *clk = *clk | 0x400000u;
}

/* PHY 初始化会动内部 USB PHY（IDF 里由 phy_bbpll_en_usb 之类处理），
 * 结果是我们的 USB-Serial/JTAG 掉枚举 —— 串口/esptool 全没了（实测）。
 * 调用返回后把 USB PHY 重新抢回来。 */
static void s3_win_usb_reclaim(void)
{
    volatile u32 *rtc_usb = (volatile u32 *)0x60008120u;   /* RTC_CNTL_USB_CONF */

    *rtc_usb = (*rtc_usb & ~(1u << 19)) | (1u << 20);      /* USJ 抢回 PHY */
    *(volatile u32 *)0x600C001Cu |= (1u << 10);            /* PERIP_CLK_EN1: USB device */
    *(volatile u32 *)0x600C0024u &= ~(1u << 10);           /* PERIP_RST_EN1: 解除复位 */
    *(volatile u32 *)0x60038018u &= ~(1u << 16);           /* USB_JTAG_CONF0: 清 disconnect */
    *(volatile u32 *)0x60038018u |= (1u << 14);            /*               置 pullup */
}

/* 分步诊断：逐步执行 PHY 初始化的前置动作，每步都返回，方便用串口/信标
 * 定位"到底哪一步开始卡"。step 含义见 tests/t_s3_phy.c6。
 * 返回：0 成功；否则是失败的步骤号。
 * 另外把 g_phyFuns 的内容也暴露出去（step 2 用它确认 ROM 真的填了表）。 */
extern volatile u32 g_phyFuns;
int register_chipv7_phy_init_param(const void *init_data, void *cal_data, int mode);

u32 s3_win_phy_funcs_word(void)
{
    return g_phyFuns;
}

/* 调 no-op 返回，专门拿 phy_get_romfuncs() 的返回值（有些实现会返回表指针） */
u32 s3_win_phy_romfuncs_ret(void)
{
    return phy_get_romfuncs();
}

/* 在 ROM 数据区里找 PHY 函数表：
 * 特征 —— 某个 4 字节位置存着指向 DRAM 的指针 V，而 *(V + 0x204) 是一个
 * ROM 代码地址（0x4000_0000..0x400F_FFFF）。register_chipv7_phy 正是
 * 用 l32i a5, [表+0x204]; callx8 a5 起手，所以这个特征很强。 */
u32 s3_win_find_phytab(void)
{
    /* ROM 数据区：esp32s3.rom.ld 里有 phy_param_rom = 0x3fcef81c，
     * 说明 0x3FCEF000 往上这块是 ROM 自己的数据结构区（映射好的）。
     * 0x3FC88000..0x3FCA0000 反而没映射，去读会 LoadStoreError。 */
    u32 a = 0x3FCEF000u;

    while (a < 0x3FD00000u) {
        u32 v = *(volatile u32 *)a;

        /* 只在**确定映射的 DRAM** 里解引用，否则会 LoadStoreError（踩过） */
        if (v >= 0x3FC88000u && v < 0x3FCF0000u) {
            u32 e = *(volatile u32 *)(v + 0x204u);

            if (e >= 0x40000000u && e < 0x40100000u) {
                return v;
            }
        }
        a = a + 4u;
    }
    return 0u;
}

/* 在 [start,end) 里 4 字节步进扫描，把值落在 0x3FC00000..0x3FD00000（ROM 数据区）
 * 的**地址**收集出来（这些就是"某处存着指向 DRAM 的指针"，很可能就是函数表）。 */
u32 s3_win_scan_dram(u32 start, u32 end, u32 *out, u32 max)
{
    u32 a = start;
    u32 n = 0;

    while (a < end && n < max) {
        u32 v = *(volatile u32 *)a;

        if (v >= 0x3FC00000u && v < 0x3FD00000u) {
            out[n] = a;
            n = n + 1u;
        }
        a = a + 4u;
    }
    return n;
}

/* IDF 的 esp_wifi_bt_power_domain_on() 等价物 —— **必须在 PHY 初始化之前**：
 *   1) 解除 WiFi 掉电：RTC_CNTL_DIG_PWC.WIFI_FORCE_PD(bit17) = 0
 *   2) 打开 WiFi/BT 共用时钟 + PHY 校准时钟（SYSCON_WIFI_CLK_EN）
 *   3) 给 modem 打一次复位脉冲：SYSCON_WIFI_RST_EN 的 MODEM_RESET_FIELD_WHEN_PU
 *      = bits 0,1,2,3,4,9,11,13 = 0x2A1F
 *   4) 解除隔离：RTC_CNTL_DIG_ISO.WIFI_FORCE_ISO(bit28) = 0
 * 没有这些，PHY 里的寄存器读回来全是垃圾 —— 实测表现就是程序跑到奇怪的地方
 * （IllegalInstruction，PC 落在指令中间，而且随校准模式漂移）。
 */
static void s3_win_phy_clock_enable(void);

static void s3_win_phy_power_on(void)
{
    volatile u32 *pwc = (volatile u32 *)0x60008090u;   /* RTC_CNTL_DIG_PWC */
    volatile u32 *iso = (volatile u32 *)0x60008094u;   /* RTC_CNTL_DIG_ISO */
    volatile u32 *rst = (volatile u32 *)0x60026018u;   /* SYSCON_WIFI_RST_EN */
    volatile u32 k = 0;

    *pwc = *pwc & ~(1u << 17);              /* 解除 WiFi 掉电 */
    while (k < 400u) { k = k + 1u; }        /* ~10us @40MHz */
    s3_win_phy_clock_enable();              /* 打开 modem 时钟 */
    *rst = *rst | 0x2A1Fu;                  /* modem 复位：assert */
    *rst = *rst & ~0x2A1Fu;                 /* modem 复位：release */
    *iso = *iso & ~(1u << 28);              /* 解除隔离 */
    k = 0;
    while (k < 400u) { k = k + 1u; }
}

int s3_win_phy_step(int step)
{
    s3_win_phy_power_on();
    if (step <= 1) {
        return 0;
    }
    {
        /* 把 ROM 返回的函数表指针存进 g_phyFuns —— 这正是 register_chipv7_phy
         * 一上来就要用的那张表。S3 的 ROM 不导出 g_phyFuns 符号（S2 才导出），
         * 所以必须自己接上。 */
        u32 tab = 0;

        (void)tab;
        /* ① 让 PHY 知道 USB 也在用内部 PHY（IDF 的 CONFIG_ESP_PHY_ENABLE_USB 等价物） */
        phy_bbpll_en_usb(1u);
        /* ② 初始化 PHY 库自己的函数指针表（g_phyFuns）——必须在 register_chipv7_phy 之前 */
        phy_get_romfunc_addr();
    }
    if (step <= 2) {
        return 0;
    }
    if (g_phyFuns == 0u) {
        return 0x100 + 2;          /* 表没填上 */
    }
    if (step <= 3) {
        return 0;
    }
    (void)register_chipv7_phy_init_param(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);
    if (step <= 4) {
        return 0;
    }
    return register_chipv7_phy(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);
}

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
    s3_win_phy_clock_enable();
    /* **关键**：register_chipv7_phy() 一上来就用全局函数表 g_phyFuns
     * （反汇编：l32i a5, g_phyFuns; l32i a5, a5, 0x204; callx8 a5）。
     * 这个表由 ROM 的 phy_get_romfuncs()（0x40005c58）填；IDF 在
     * esp_phy_enable() 里会先调它。我们之前没调 -> 表是空的 -> 解引用空指针，
     * 表现就是"卡死在里面"。
     * ROM 是 windowed，所以必须从孤岛里调。 */
    phy_get_romfuncs();
    {
        int rc = register_chipv7_phy(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);

        s3_win_usb_reclaim();      /* 把 USB PHY 抢回来，否则串口/esptool 都失联 */
        return rc;
    }
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

u32 s3_win_phy_step(u32 step)
{
    (void)step;
    return 0xFFFFFFFFu;   /* 没链 libphy */
}

u32 s3_win_phy_funcs_word(void)
{
    return 0u;
}

u32 s3_win_phy_romfuncs_ret(void)
{
    return 0u;
}

u32 s3_win_find_phytab(void)
{
    return 0u;
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

/* 通过 ROM 的 Cache_Enable_ICache() 使能 I-cache（IDF 自己也是这么做的，
 * 见 hal/esp32s3/include/hal/cache_ll.h）。ROM 是 windowed ABI，所以本函数
 * 必须在这个 -mabi=windowed 的孤岛里实现，由 win_bridge 的 trampoline 调进来。 */
u32 s3_win_cache_enable(void)
{
    void (*fn)(u32) = (void (*)(u32))0x40001878u;   /* ROM: Cache_Enable_ICache */

    fn(0u);
    return 0u;
}

/* 清 I-cache：ROM 的 Cache_Invalidate_ICache_All(0x400016d4)。
 * 之前从 call0 直接调会"一调就死"，其实是 ROM 的 windowed ABI 问题；
 * 放在这个 windowed 孤岛里调才是对的。 */
u32 s3_win_cache_invalidate(void)
{
    void (*fn)(void) = (void (*)(void))0x400016d4u;

    fn();
    return 0u;
}

/* 读 ROM 的 Cache_Get_IROM_MMU_End / Cache_Get_DROM_MMU_End（判断 IROM 在 512 项表里的切分点） */
u32 s3_win_irom_mmu_end(void)
{
    return ((u32 (*)(void))0x4000195cu)();
}

u32 s3_win_drom_mmu_end(void)
{
    return ((u32 (*)(void))0x40001968u)();
}

/* ROM Cache_Invalidate_Addr(addr, size) @0x400016b0 —— 按地址失效（esp-hal 就是这么做的） */
u32 s3_win_cache_invalidate_addr(u32 addr, u32 size)
{
    void (*fn)(u32, u32) = (void (*)(u32, u32))0x400016b0u;

    fn(addr, size);
    return 0u;
}

/* ROM Cache_Suspend_ICache @0x4000189c —— 返回挂起前的状态 */
u32 s3_win_cache_suspend_icache(void)
{
    return ((u32 (*)(void))0x4000189cu)();
}

/* ROM Cache_Resume_ICache @0x400018a8 */
u32 s3_win_cache_resume_icache(u32 state)
{
    ((void (*)(u32))0x400018a8u)(state);
    return 0u;
}

/* ROM rom_config_instruction_cache_mode(size, ways, line) @0x40001a1c
 * IDF 就是靠它配置 I-cache 的容量/相联度/行大小（而不只是改 CTRL 那一位）。 */
u32 s3_win_config_icache_mode(u32 size, u32 ways, u32 line)
{
    ((void (*)(u32, u32, u32))0x40001a1cu)(size, ways, line);
    return 0u;
}

/* esp_wifi_init_internal 是 windowed ABI（入口是 entry a1,32），必须从这个
 * -mabi=windowed 的孤岛里调用；从 call0 直接 callx 会在 entry 指令上
 * IllegalInstruction。 */
extern int esp_wifi_init_internal(const void *cfg);

u32 s3_win_wifi_init(void *cfg)
{
    return (u32)esp_wifi_init_internal(cfg);
}

/* wifi_glue.c 现在是 windowed 编译，call0 侧必须经孤岛调它 */
extern void s3_wifi_osi_init(void);
extern u32  s3_wifi_osi_funcs_ptr(void);

extern void *s3_wifi_cfg_ptr(void);
extern void *s3_wifi_calls_ptr(void);

extern u32 s3_wifi_bringup(void);
u32 s3_win_bringup(void) { return s3_wifi_bringup(); }

u32 s3_win_cfg_ptr(void)   { return (u32)s3_wifi_cfg_ptr(); }
u32 s3_win_calls_ptr(void) { return (u32)s3_wifi_calls_ptr(); }

u32 s3_win_osi_init(void)
{
    s3_wifi_osi_init();
    return s3_wifi_osi_funcs_ptr();
}

/* 通用 windowed 调用：以窗口化 ABI 调用任意函数地址，传一个参数 */
u32 s3_win_call1(u32 fn, u32 arg)
{
    return ((u32 (*)(u32))fn)(arg);
}

/* 放在 .flash.text 末尾的 call0 函数：用于验证"高偏移处的取指"是否正常。
 * 注意它不用 windowed ABI（就是普通 C 函数），所以没有 entry/retw。 */
__attribute__((section(".flashend.test"), noinline, used))
u32 s3_flash_end_test(u32 x)
{
    return x + 9u;
}
