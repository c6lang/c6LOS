/* windowed ABI 孤岛：这里放 IDF 的闭源库和它们的 C 胶水（它们只能是 windowed）。
 * 本文件用 -mabi=windowed 单独编译，其余代码仍是 call0，两者之间只通过
 * port/s3/win_bridge.S 里的手写 trampoline 来往。
 */
#include "c6os/types.h"
#include "c6os/flash.h"
#include "wifi_creds.h"

/* os_wdt_feed/os_wdt_start 是 call0 的 C 函数，windowed 岛里要经 w2c 壳调（§228） */
extern void s3_w2c_wdt_feed(void);
extern unsigned int s3_w2c_sp_probe(void);
extern unsigned int s3_w2c_task_delay(unsigned int ticks);
extern void s3_w2c_wdt_start(unsigned int secs);

/* 内核原语（call0 侧，经 port/s3/w2c.S 的壳调用） */
extern u32 s3_w2c_current_task(void);
extern u32 s3_w2c_tick_get(void);
extern u32 s3_w2c_task_probe(u32 id, u32 what);

/* 桥进出计数（win_bridge.S 自增，供 RTC 诊断） */
u32 g_wb_in = 0;
u32 g_wb_out = 0;

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
 * ROM 里的 s3_ets_printf 按 ROM 自己的配置输出（USB-Serial/JTAG 或 UART0），
 * 不依赖我们 port_init 里的 USB 初始化 —— 串口"坏掉"时这是最可靠的通道。
 * 注意 ROM 是 windowed ABI，必须从孤岛（windowed）里调，call0 直接调会跑飞。
 */
int s3_ets_printf(const char *fmt, ...);
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
    s3_ets_printf("%s", s);
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
    /* §288：BBPLL 上电。IDF 的二级 bootloader 在 rtc_clk_init() 里会调
     * clk_ll_bbpll_enable()，把 RTC_CNTL_OPTIONS0 的三个 FORCE_PD 清掉：
     *   BB_I2C_FORCE_PD = BIT(6)、BBPLL_I2C_FORCE_PD = BIT(8)、BBPLL_FORCE_PD = BIT(10)
     * 我们镜像没有二级 bootloader，这个一直没人做 —— 而 WiFi MAC/BB 的时钟就是
     * 从 BBPLL 分出来的（正是 §285 里恒 0 的那个计数器）。 */
    *(volatile u32 *)0x60008000u &= ~((1u << 6) | (1u << 8) | (1u << 10));
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
    if (step <= 4) {
        return 0;
    }
    return register_chipv7_phy(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);
}

/* 给 OSI 的 `_phy_enable` 槽位（slot 53）用：IDF 的 esp_adapter.c 里就是
 * `._phy_enable = esp_phy_enable_wrapper`（→ esp_phy_enable(PHY_MODEM_WIFI)），
 * 也就是说**PHY 使能是 WiFi 库自己在合适的时机回调我们的**，而不是我们提前手动做。
 * 详见 docs/wifi-plan.md §228。 */
int s3_win_phy_enable(void);     /* 定义在下面 */

int s3_win_phy_enable_hook(void)
{
    s3_ets_printf("[phy] OSI _phy_enable 回调进入\n");
    return s3_win_phy_enable();
}

/* 正常路径：**必须**和 bisect_run() 的子步骤顺序完全一致。
 *
 * 这里曾经和诊断路径不一致（少了 power_on、少了 phy_bbpll_en_usb、函数表用的是
 * 另一个入口、而且那个入口并不会写 g_phyFuns），结果就是 §202 末尾那个
 * "W0 phy start 之后整片从 USB 上消失" —— 现在把原因找到了（§205）：
 *
 * 反汇编 libphy 的 `i2c_bbpll_set()`（我们镜像里 0x42056658）可以看到：
 *     ...
 *     a3 = phy_param;  s8i .a10, a3, 209 / 210     ; 存 I2C 读回值
 *     a3 = phy_param + 0x200
 *     a11 = phy_param[0x2A6]                        ; ← 就是 phy_bbpll_en_usb() 写的那个 byte
 *     bnez a11, 跳过
 *     i2c write(reg 102, sub 5, val 7)              ; ← 这一段是"把内部 USB PHY 关掉"
 *
 * 也就是 IDF Kconfig `ESP_PHY_ENABLE_USB` 那句话的机器码版本：
 * **不显式 phy_bbpll_en_usb(1)，register_chipv7_phy() 就会顺手关掉 USB PHY**，
 * USB-Serial-JTAG 连 `303a:` 都不再出现。我们原来的 s3_win_phy_enable 恰好没调它。
 */
int s3_win_phy_enable(void)
{
    int rc;

    /* IDF 在调 register_chipv7_phy 之前会把 efuse 里的 MAC 填进 cal_data.mac
     * （见 esp_phy/src/phy_init.c）。这里先写死成这块板自己的 MAC
     * cc:8d:a2:ec:99:c4；以后改成从 efuse 读。 */
    g_phy_cal.v[4] = 0xccu;
    g_phy_cal.v[5] = 0x8du;
    g_phy_cal.v[6] = 0xa2u;
    g_phy_cal.v[7] = 0xecu;
    g_phy_cal.v[8] = 0x99u;
    g_phy_cal.v[9] = 0xc4u;

    /* ① 解除 WiFi 掉电 + 开 modem/PHY 时钟 + 复位脉冲 + 解隔离
     *    （= IDF 的 wifi_bt_common_module_enable + phy_module_enable） */
    s3_win_phy_power_on();

    /* ② 告诉 PHY：USB PHY 要留着（否则 i2c_bbpll_set 会把 USB 关掉，见上面） */
    phy_bbpll_en_usb(1u);

    /* ③ 填 g_phyFuns：register_chipv7_phy() 一上来就 `l32i a5, g_phyFuns;
     *    l32i a5, a5, 0x204; callx8 a5`，表是空的就直接跳 0 号地址。
     *    主入口是 libphy 的 phy_get_romfunc_addr()；ROM 的 phy_get_romfuncs()
     *    作为双保险再补一次（两者填的不是同一张表，都调不会有害）。 */
    phy_get_romfunc_addr();
    if (g_phyFuns == 0u) {
        phy_get_romfuncs();
    }
    s3_ets_printf("[phy] g_phyFuns=0x%08x\n", (unsigned)g_phyFuns);

    /* §292 实验：参照工程运行时 SYSCON_WIFI_RST_EN(0x60026018) = 0x3e08，
     * 我们一直是 0。按参照值写一次，看 MAC 块（0x6001c000 段）会不会活过来。 */
    s3_ets_printf("[phy] MAC 0x6001c08c=%08x OPTIONS0=%08x\n",
               (unsigned)*(volatile u32 *)0x6001c08cu,
               (unsigned)*(volatile u32 *)0x60008000u);
    s3_ets_printf("[phy] calling register_chipv7_phy(mode=%d)\n", (int)S3_PHY_CAL_MODE);
    /* ④ 主入口（IDF 的 esp_phy_load_cal_and_init 里就直接是这一句；
     *    register_chipv7_phy_init_param() 只在 esp32c2 的 ROM ld 里出现过，
     *    IDF 的 S3 流程从不调它，我们也不调 —— 少一个"自己发明的调用"。 */
    rc = (int)register_chipv7_phy(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);
    s3_ets_printf("[phy] register_chipv7_phy rc=%d\n", rc);

    /* ⑥ 把 USB PHY 抢回来（即使 ② 生效也补一次，代价极小） */
    s3_win_usb_reclaim();
    return rc;
}

u32 s3_win_phy_cal_word(u32 i)
{
    return ((const u32 *)(const void *)g_phy_cal.v)[i & 0x1FFu];
}

/* ---- §204 跨复位自动二分 -------------------------------------------------
 *
 * 现象：一进 PHY 初始化，整块板子从 USB 上彻底消失（lsusb 里连 303a 都没了），
 * 只能人工断电。一次实验 = 一次盲刷 + 一次拔插电源，效率极低。
 *
 * 办法：把 PHY 前置动作切成 N 个**累积**子步骤，每轮复位只跑 prefix[0..k]，
 * 跑完**故意不喂狗** → RWDT(8s) 复位整片 → 下一轮 k+1。RTC 慢存跨复位不清，
 * 所以用两个 bitmask 记：
 *   BISECT_HUNG：进了这一步、但活到复位都没把标记清掉（= 挂死在这一步），
 *                并且这个 bit 同时当"以后跳过它"的标记（否则会永远卡同一处）；
 *   BISECT_OK  ：这一步活着跑完了（返回了）。
 * 最后一轮把两个 mask 打到串口，一次上电就得到完整故障图。
 *
 * 局限（写在这里免得以后误读）：如果某一步只是**把 USB 掐了**而 CPU 还活着，
 * 它会被记成 OK —— 那种情况只能靠主机端串口日志辨认：
 * 最后一条 `[bisect] step N enter` 后面没有 `ok`，就是它。
 */
#define BISECT_N     6u
#define BISECT_CUR   0x50000A20u
#define BISECT_HUNG  0x50000A24u
#define BISECT_OK    0x50000A28u
#define BISECT_DONE  0x50000A2Cu
#define BISECT_MAGIC 0x600DD0E0u
/* 上电后 RTC 慢存是**随机值**（只有掉电才清）。没有这个校验字，BISECT_CUR
 * 可能一上来就是 0x7fffffff，于是第一轮直接打印"done"把整个二分跳过去。 */
#define BISECT_INIT  0x50000A30u
#define BISECT_INIT_MAGIC 0x600DD0E1u

static u32 bisect_get(u32 addr) { return *(volatile u32 *)addr; }
static void bisect_set(u32 addr, u32 v) { *(volatile u32 *)addr = v; }
static void bisect_or(u32 addr, u32 bits) { *(volatile u32 *)addr = *(volatile u32 *)addr | bits; }
static void bisect_andn(u32 addr, u32 bits) { *(volatile u32 *)addr = *(volatile u32 *)addr & ~bits; }

/* 执行"第 k 个子步骤"本身（k>=1）。返回非 0 表示这一步自己报告失败。
 * 顺序和 IDF esp_phy_enable() 一致：common clock → phy_module_enable
 * → phy_bbpll_en_usb → (cal data) → register_chipv7_phy。 */
static u32 bisect_run(u32 k)
{
    if (k == 1u) {
        s3_win_phy_clock_enable();                       /* WIFI_CLK_EN */
        return 0u;
    }
    if (k == 2u) {
        s3_win_phy_power_on();                           /* PWC/ISO/RST pulse */
        return 0u;
    }
    if (k == 3u) {
        phy_bbpll_en_usb(1u);                            /* 别把 USB PHY 关掉 */
        return 0u;
    }
    if (k == 4u) {
        phy_get_romfunc_addr();                          /* 填 g_phyFuns */
        return (g_phyFuns == 0u) ? 1u : 0u;
    }
    if (k == 5u) {
        return (u32)(int)register_chipv7_phy(s3_phy_init_data, &g_phy_cal, S3_PHY_CAL_MODE);
    }
    return 0u;
}

/* 返回 1 = 二分已经全部完成（正常流程可以继续）；0 = 本轮试到某一步为止，
 * 调用者应该停下来（不喂狗），让 RWDT 复位后继续下一轮。 */
u32 s3_win_phy_bisect(void)
{
    u32 k;
    u32 s = 1u;

    if (bisect_get(BISECT_INIT) != BISECT_INIT_MAGIC) {
        /* 刚上电（或上一次是人手动断电）：账本清零 */
        bisect_set(BISECT_CUR, 0u);
        bisect_set(BISECT_HUNG, 0u);
        bisect_set(BISECT_OK, 0u);
        bisect_set(BISECT_DONE, 0u);
        bisect_set(BISECT_INIT, BISECT_INIT_MAGIC);
    }
    if (bisect_get(BISECT_DONE) == BISECT_MAGIC) {
        return 1u;
    }
    k = bisect_get(BISECT_CUR);
    if (k >= BISECT_N) {
        s3_ets_printf("[bisect] done hung=0x%08x ok=0x%08x\n",
                   (unsigned)bisect_get(BISECT_HUNG), (unsigned)bisect_get(BISECT_OK));
        bisect_set(BISECT_DONE, BISECT_MAGIC);
        return 1u;
    }
    s3_ets_printf("[bisect] round k=%u\n", (unsigned)k);
    while (s <= k) {
        if ((bisect_get(BISECT_HUNG) & (1u << s)) != 0u) {
            s3_ets_printf("[bisect]   step %u SKIP(以前挂死过)\n", (unsigned)s);
            s = s + 1u;
            continue;
        }
        s3_w2c_wdt_feed();                         /* 每步都从满 8s 开始计时 */
        bisect_or(BISECT_HUNG, 1u << s);       /* 先进"挂死候选"，跑完再清 */
        s3_ets_printf("[bisect]   step %u enter\n", (unsigned)s);
        {
            u32 rc = bisect_run(s);

            s3_ets_printf("[bisect]   step %u ok rc=%u\n", (unsigned)s, (unsigned)rc);
        }
        bisect_andn(BISECT_HUNG, 1u << s);
        bisect_or(BISECT_OK, 1u << s);
        s = s + 1u;
    }
    bisect_set(BISECT_CUR, k + 1u);
    return 0u;
}
#else
int s3_win_phy_enable(void)
{
    return -1;      /* 没链 libphy */
}

u32 s3_win_phy_bisect(void)
{
    return 1u;      /* 没链 libphy：直接当二分完成 */
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

/* esp_wifi_start()：闭源库里的公开 API，同样是 windowed ABI。 */
extern int esp_wifi_start(void);
u32 s3_win_wifi_start(void)
{
    return (u32)(int)esp_wifi_start();
}

/* ---- 对照实验：整个 WiFi 主流程都在 windowed 岛里跑 ---------------------
 * 任务入口本身也是 windowed（配合 port_task_ctx_create 的 ABI 标志），
 * 这样任务 1 的执行流里完全没有 call0<->windowed 混用，
 * 只剩内核原语（经 port/s3/w2c.S 的正规壳）。 */
/* 阶段信标：每进一个危险调用之前先写 RTC 慢存 0x50000A10。
 * 这些调用（libphy 校准、esp_wifi_init_internal）会**忙等死转**，整块板子从
 * USB 上消失；RTC 慢存跨 CPU 复位不清，所以复位后读这个字就知道上一轮死在哪。
 * 同时每步之后 feed 一次 RWDT（见 os_wdt_start）：超过 8s 没 feed 就整片复位，
 * 板子自己回来，不用人工断电。 */
#define WSTAGE(v) do { *(volatile unsigned int *)0x50000A10u = (v); } while (0)

__attribute__((section(".flash.text")))
u32 s3_win_wifi_main(void)
{
    u32 rc = 0;

    /* 报一下自己的任务 id：诊断输出里 taskN 的身份要靠这个对上号 */
    s3_ets_printf("[wifimain] id=%u tick=%u\n", (unsigned)s3_w2c_current_task(),
               (unsigned)s3_w2c_tick_get());
    /* §421 诊断：钉住库的 wifi init 状态机是从哪一步开始变成 2 的。 */
    {
        extern unsigned char g_wifi_osi_funcs_md5[];
        extern int s3_ets_printf(const char *fmt, ...);
        volatile u32 *wst = (volatile u32 *)(void *)(g_wifi_osi_funcs_md5 + 4);
        s3_ets_printf("[wst] entry=%u\n", (unsigned)*wst);
    }
#ifdef S3_PHY_BISECT
    /* §204：跨复位自动二分。没跑完就停在这儿**不喂狗**，让 RWDT(8s) 复位，
     * 下一轮继续；跑完了（或没开这个开关）才走正常流程。 */
    if (s3_win_phy_bisect() == 0u) {
        for (;;) { }
    }
    s3_ets_printf("[wifimain] bisect passed, normal flow\n");
#endif
    WSTAGE(0x1001u); s3_w2c_wdt_feed();
    s3_win_log("W0 phy start\n");
    WSTAGE(0x1002u); s3_w2c_wdt_feed();
    (void)s3_win_phy_cal();
    { extern unsigned char g_wifi_osi_funcs_md5[]; volatile u32 *w = (volatile u32 *)(void *)(g_wifi_osi_funcs_md5 + 4);
      s3_ets_printf("[wst] after phy_cal=%u\n", (unsigned)*w); }
    WSTAGE(0x1003u); s3_w2c_wdt_feed();
#ifndef S3_PHY_VIA_OSI
    (void)s3_win_phy_enable();
#else
    s3_ets_printf("[phy] 跳过手动初始化：交给 OSI _phy_enable 回调\n");
#endif
    { extern unsigned char g_wifi_osi_funcs_md5[]; volatile u32 *w = (volatile u32 *)(void *)(g_wifi_osi_funcs_md5 + 4);
      s3_ets_printf("[wst] after phy_enable=%u\n", (unsigned)*w); }
    WSTAGE(0x1004u); s3_w2c_wdt_feed();
    s3_win_log("W1 bringup\n");
    WSTAGE(0x1005u); s3_w2c_wdt_feed();
    rc = s3_wifi_bringup();
    WSTAGE(0x1006u); s3_w2c_wdt_feed();
    s3_ets_printf("[wifi] bringup rc=%u\n", (unsigned)rc);
    s3_win_log("W2 bringup returned\n");
#ifdef S3_WITH_LWIP
    /* §210/§215：bringup 成功就把 IP 栈接上（lwIP + DHCP + RX 回调），
     * 然后关联 AP、等 DHCP 拿到地址，最后取 https://qq.com。
     * 这一段是"关联之后"的路径，只有 -DS3_WITH_LWIP 的镜像才有。 */
    if (rc == 0u) {
        extern int  c6_lwip_start(const unsigned char *mac);
        extern void c6_lwip_poll(void);
        extern u32  s3_w2c_task_delay(u32 ticks);
        static const unsigned char lwip_mac[6] = { 0xccu, 0x8du, 0xa2u, 0xecu, 0x99u, 0xc4u };
        int irc = c6_lwip_start(lwip_mac);
        u32 n = 0u;

        s3_ets_printf("[lwip] start rc=%d\n", irc);
        {
            extern void s3_wifi_dump_osi(const char *tag);

            s3_wifi_dump_osi("init 之后");
        }
        WSTAGE(0x1010u);
        /* 关联 + 等 IP（阶段信标：0x1021 关联中、0x1022 关联成功、0x1023 拿到 IP） */
        {
            /* §279：闭源库的 `sta_is_wpa3_enabled()` 会解引用 `g_cnxMgr+0x1b4`
             * （IDF 里由 wpa_supplicant 调 `esp_wifi_register_wpa_cb_internal()`
             * 填的 WPA 回调表）。我们没有 supplicant → NULL → cause=0x1c。
             * 这里注册一个全零表：`*(cb+0x48)==0` ⇒ 它返回"非 WPA3"，正是我们要的。
             * **故意放在这个 TU（windowed 岛）里**：wifi_glue.c 的 .text/.bss
             * 一点不动，避免 §271 那种"改几十字节就换分支"的坑。 */
            extern int esp_wifi_register_wpa_cb_internal(void *cb);
            /* 每个槽都填"返回 0"的窗口化桩（否则库里 callx8 到 NULL，§279），
             * 只有 +0x48（word 18）留 0 —— 那一格是被当布尔量读的。 */
            extern int s3_wpa_cb_stub(void);
            static u32 s_wpa_cb_zero[24];
            {
                u32 k = 0u;

                for (k = 0u; k < 24u; k++) { s_wpa_cb_zero[k] = (u32)(void *)s3_wpa_cb_stub; }
                s_wpa_cb_zero[18] = 0u;
            }
            s3_ets_printf("[wifi] register_wpa_cb=%d\n",
                       esp_wifi_register_wpa_cb_internal((void *)s_wpa_cb_zero));
            extern u32 s3_wifi_associate(const char *ssid, const char *pass);
            extern int s3_wifi_wait_connected(int ms);
            extern int c6_lwip_wait_ip(int ms);
            extern void c6_lwip_print_ip(void);
            const char *ssid = (const char *)S3_WIFI_SSID;
            const char *pass = (const char *)S3_WIFI_PASS;

            extern void s3_wifi_dump_osi(const char *tag);
            extern void s3_wifi_dump_events(const char *tag);

            WSTAGE(0x1021u);
            (void)s3_wifi_associate(ssid, pass);
            s3_wifi_dump_osi("start+scan 之后");
            s3_wifi_dump_events("start+scan 之后");
            if (s3_wifi_wait_connected(15000) != 0) {
                extern int c6_lwip_start_dhcp(void);

                WSTAGE(0x1022u);
                (void)c6_lwip_start_dhcp();
                s3_wifi_dump_osi("关联成功");
                s3_wifi_dump_events("关联成功");
                if (c6_lwip_wait_ip(20000) != 0) {
                    WSTAGE(0x1023u);
                    c6_lwip_print_ip();
                } else {
                    s3_ets_printf("[lwip] DHCP 超时\n");
                }
            }
        }
        while (n < 2000u) {          /* 先跑一段，让 DHCP/DNS 有机会动起来 */
            c6_lwip_poll();
            s3_w2c_task_delay(1u);
            n = n + 1u;
        }
        WSTAGE(0x1011u);
        s3_ets_printf("[lwip] polled %u\n", (unsigned)n);
#ifdef S3_WITH_HTTPS
        {
            extern int c6_https_demo(void);
            int hrc = c6_https_demo();

            s3_ets_printf("[https] demo rc=%d\n", hrc);
            WSTAGE(0x1020u);
        }
#endif
    }
#endif
    return rc;
}

/* 最小 windowed 任务（§239）：只打印 + 让出 CPU，用来单独验证
 * "新建 windowed 任务的第一次进入"这条路。 */
__attribute__((section(".flash.text")))
void s3_win_hello_task(void *arg)
{
    u32 n = 0u;

    s3_ets_printf("[w%u] hello, windowed task (sp=%08x)\n",
               (unsigned)(u32)arg, (unsigned)s3_w2c_sp_probe());
    for (;;) {
        s3_w2c_task_delay(20u);
        n = n + 1u;
        if ((n & 0x3u) == 0u) {
            s3_ets_printf("[w%u] tick %u\n", (unsigned)(u32)arg, (unsigned)n);
        }
    }
}

void s3_win_wifi_main_task(void *arg)
{
    u32 rc;

    (void)arg;
    rc = s3_win_wifi_main();
    /* 结果写进 RTC（窗口ed 里不能直接调 call0 的 s3_beacon） */
    *(volatile unsigned int *)0x50001200u = rc;
    for (;;) { }
}

/* ---- windowed 调用链跨任务切换的压力实验 --------------------------------
 * 目的：单独验证"windowed 代码在让出 CPU 再回来之后，寄存器窗口还对不对"，
 * 完全不涉及 WiFi 库。如果这里就崩，说明问题在我们的上下文切换，
 * 而不是闭源库。
 *
 * 递归里故意写 `+ 1u`，防止编译器把尾递归优化成循环（那样就不占窗口了）。
 */
extern u32 s3_w2c_task_delay(u32 ticks);

/* 最内层先跑一次深链：s3_win_deep12_entry(8) 用 call12 递归 8 层，
 * s3_win_deep(16) 递归 16 层 —— 4 个窗口装不下，必然触发
 * _win_ovf4/8/12 与 _win_unf4/8/12。返回值是固定算术值，
 * 所以计数器的整除关系能直接证明整条链算对了。 */
__attribute__((section(".flash.text")))
u32 s3_win_stress(u32 depth, u32 rounds)
{
    if (depth == 0u) {
        u32 v = s3_win_deep12_entry(8u);   /* = 8 + (1+2+...+11) = 74 */
        u32 w = s3_win_deep(16u);          /* = 17 */

        s3_w2c_task_delay(1u);             /* 真正切到别的任务 */
        return rounds + v + w;             /* 每轮 +91 */
    }
    return s3_win_stress(depth - 1u, rounds) + 1u;
}

__attribute__((section(".flash.text")))
void s3_win_stress_task(void *arg)
{
    extern int s3_wifi_test_recv(u32 rounds);
    volatile u32 *d = (volatile u32 *)0x50000A40u;

    (void)arg;
    {
        /* §383：**必须显式打开祖先窗口 spill**。
         * `g_s3_win_spill_en` 只在 `s3_wifi_osi_init()` 里被置 1，而这条
         * 隔离测试用例根本不走那条路径 ⇒ 一直是 0 ⇒ W2C 壳里的 WIN_SPILL
         * 被跳过 ⇒ "深递归帧还活着的窗口" 在被切走时**没有被存到栈上**，
         * 切回来就再也找不回来了（阶段 3c 在 dep=1 就卡死，而 3a/3b 通过，
         * 正是因为它们切换时窗口已经弹空/还没压深）。 */
        extern u32 g_s3_win_spill_en;
        g_s3_win_spill_en = 1u;
    }
    d[0] = 0xC0DE1234u;              /* 已进入 windowed 任务 */
    d[3] = (u32)s3_wifi_test_recv(1u);   /* 库→_queue_recv 最小复现 */
    d[4] = 0xC0DE5678u;              /* 那条路径没崩 */

    /* 阶段 1：只 delay（浅 windowed 链 + 真的切走再回来） */
    {
        u32 i;
        for (i = 0u; i < 3u; i++) {
            s3_w2c_task_delay(1u);
            d[5] = d[5] + 1u;
        }
    }
    d[8] = 0xAAAA0001u;

    /* 阶段 2：只跑递归深链（不切换，纯窗口溢出/欠载） */
    {
        u32 v = s3_win_deep12_entry(8u);   /* 期望 74 */
        u32 w = s3_win_deep(16u);          /* 期望 17 */

        d[6] = v * 1000u + w;
    }
    d[8] = 0xAAAA0002u;

    /* 阶段 3：递归深链 + 切换。逐级加深度，d[9] 记下最后一次尝试的深度，
     * 卡住时读 d[9] 就知道多深的链 + 切换会坏。 */
    {
        u32 dep;

        /* §382：把阶段 3 拆成三个隔离项，一次上板就能定位：
         *   d[10] = 3a：深递归**之后**才 delay（递归已经返回、栈已弹空）
         *   d[11] = 3b：delay **之后**再做深递归
         *   d[12] = 3c：递归里 delay（原来的 s3_win_stress 形态）
         * 每一步都写自己的槽，卡在哪一步一眼可见。 */
        d[10] = 0u;
        d[11] = 0u;
        d[12] = 0u;
        for (dep = 1u; dep < 6u; dep++) {
            d[9] = dep;
            d[10] = dep;
            d[7] = d[7] + s3_win_deep12_entry(8u) + s3_win_deep(16u);
            s3_w2c_task_delay(1u);           /* 3a：递归完再切 */
            d[10] = 0x100u + dep;

            d[11] = dep;
            s3_w2c_task_delay(1u);           /* 3b：先切再递归 */
            d[7] = d[7] + s3_win_deep12_entry(8u) + s3_win_deep(16u);
            d[11] = 0x200u + dep;

            d[12] = dep;
            /* §394：先试**不递归、只切**（等同 s3_win_stress 的最内层），
             * 用来区分"切换本身坏"与"深链 + 切换交互坏"。 */
            d[13] = 0x500u + dep;
            d[7] = d[7] + s3_win_deep12_entry(8u);
            s3_w2c_task_delay(1u);
            d[7] = d[7] + s3_win_deep(16u);
            d[13] = 0x600u + dep;
            /* 真正的交互形态 */
            d[7] = d[7] + s3_win_stress(dep, dep);
            d[12] = 0x300u + dep;

            d[9] = 0x400u + dep;
        }
    }
    d[8] = 0xAAAA0003u;

    for (;;) {
        d[1] = d[1] + 1u;
        s3_w2c_task_delay(1u);
    }
}

