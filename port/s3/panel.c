/* Xingzhi Cube 1.54TFT WiFi：ST7789 240x240，硬件 SPI2（GPSPI）驱动。
 *
 * 引脚（原厂 v2.5.0_xingzhi-cube-1.54tft-wifi 固件确认）：
 *   SCLK=GPIO9  MOSI/SDA=GPIO10  DC=GPIO8  CS=GPIO14  RST=GPIO18  BL=GPIO13
 *
 * 为什么不用 bit-bang：240x240x16bit = 921600 bit，40MHz CPU 位翻转最多
 * ~3Mbit/s（≈3fps），达不到 M7-3 的「全屏 ≥10fps」。这里用 SPI2 的 CPU
 * 直写 FIFO（W0..W15 = 64 字节/次），时钟由外设产生，CPU 只负责喂数据。
 *
 * 寄存器（IDF v5.3 esp32s3 spi_reg.h / system_reg.h / io_mux_reg.h：
 *   SPI2 基址 0x60024000，GPIO 0x60004000，IO_MUX 0x60009000（GPIO0 在 +0x04）
 */
#include "c6os/types.h"
#include "c6os/port.h"

/* ---- GPIO / IO_MUX ---------------------------------------------------- */
#define GPIO_BASE      0x60004000u
#define GPIO_OUT_W1TS  (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC  (GPIO_BASE + 0x0Cu)
#define GPIO_ENA_W1TS  (GPIO_BASE + 0x24u)
#define GPIO_ENA_W1TC  (GPIO_BASE + 0x28u)
#define GPIO_OUT_SEL(p) (GPIO_BASE + 0x554u + 4u * (p))
#define IO_MUX_REG(p)   (0x60009000u + 0x04u + 4u * (p))

#define SIG_GPIO_OUT    0x100u   /* GPIO matrix: 普通 GPIO 输出信号（默认值） */
#define OEN_ALWAYS      (1u << 10)

/* ---- SPI2 ------------------------------------------------------------- */
/* SPI 主机可切换：SPI2(GPSPI2, 0x60024000) 或 SPI3(GPSPI3, 0x60025000)。
 * 上游 xiaozhi v2.5.0 这块板用的是 SPI3_HOST，所以优先试 SPI3。 */
static u32 g_spi_base = 0x60025000u;   /* 默认 SPI3 */
#define SPI2_BASE      g_spi_base
#define SPI2_CMD       (*(volatile u32 *)(SPI2_BASE + 0x00u))
#define SPI2_CTRL      (*(volatile u32 *)(SPI2_BASE + 0x08u))
#define SPI2_CLOCK     (*(volatile u32 *)(SPI2_BASE + 0x0Cu))
#define SPI2_USER      (*(volatile u32 *)(SPI2_BASE + 0x10u))
#define SPI2_USER1     (*(volatile u32 *)(SPI2_BASE + 0x14u))
#define SPI2_SLAVE     (*(volatile u32 *)(SPI2_BASE + 0xE0u))
#define SPI2_USER2     (*(volatile u32 *)(SPI2_BASE + 0x18u))
#define SPI2_MS_DLEN   (*(volatile u32 *)(SPI2_BASE + 0x1Cu))
#define SPI2_MISC      (*(volatile u32 *)(SPI2_BASE + 0x20u))
#define SPI2_W(n)      (*(volatile u32 *)(SPI2_BASE + 0x98u + 4u * (n)))
#define SPI2_DMA_CONF  (*(volatile u32 *)(SPI2_BASE + 0x30u))

#define SPI_CMD_USR      (1u << 24)
#define SPI_CMD_UPDATE   (1u << 23)
#define SPI_USR_MOSI     (1u << 27)
#define SPI_USR_MISO     (1u << 28)
#define SPI_USR_DUMMY    (1u << 29)
#define SPI_USR_ADDR     (1u << 30)
#define SPI_USR_COMMAND  (1u << 31)
#define SPI_MISC_CK_IDLE (1u << 29)
#define SPI_CK_IDLE_EDGE  (1u << 29)   /* MISC: CK 空闲电平（mode 3 → 1） */
#define SPI_CK_OUT_EDGE   (1u << 9)    /* USER: 输出沿。**mode 3 下必须为 0**：
                                        * 置 1 时数据在上升沿更新，同时面板也在上升沿采样
                                        * → 采到的是刚换的新数据（错位）。实测（软件当从机、
                                        * 78kHz 慢时钟、上升沿采样 MOSI）：
                                        *   idle=1,out_edge=1 → rx=4be01eaa... （全错）
                                        *   idle=1,out_edge=0 → rx=a5f00f55... （字节序另论，见下） */
#define SPI_DOUTDIN       (1u << 0)    /* USER: 1=全双工（MOSI/MISO 同时） */
#define SPI_CS_KEEP_ACTIVE (1u << 30)  /* MISC: 事务结束后不抬 CS */

static u32 g_sig_clk = 66u;    /* SPI3_CLK_OUT_IDX */
static u32 g_sig_d   = 68u;    /* SPI3_D_OUT_IDX */
static u32 g_sig_cs  = 71u;    /* SPI3_CS0_OUT_IDX */

/* 切换主机：0=SPI2(FSPI 信号 101/103/110，时钟位 6)
 *           1=SPI3(SPI3 信号 66/68/71，时钟位 16) */
void s3_lcd_use_spi3(u32 on)
{
    if (on != 0u) {
        g_spi_base = 0x60025000u;
        g_sig_clk = 66u;
        g_sig_d = 68u;
        g_sig_cs = 71u;
    } else {
        g_spi_base = 0x60024000u;
        g_sig_clk = 101u;
        g_sig_d = 103u;
        g_sig_cs = 110u;
    }
}

/* 上游 xiaozhi 用 pclk_hz = 80MHz = APB 频率，IDF 的 spi_ll_master_cal_clock
 * 在这种情况下置 CLK_EQU_SYSCLK(bit31)，SCLK 直接等于系统时钟。 */
static u32 g_equ_sysclk = 0;

void s3_lcd_set_equ_sysclk(u32 on)
{
    g_equ_sysclk = on;
}

u32 s3_lcd_get_spi_host(void)
{
    return (g_spi_base == 0x60025000u) ? 3u : 2u;
}

#define SYS_PERIP_CLK_EN0 (*(volatile u32 *)0x600C0018u)
#define SYS_PERIP_RST_EN0 (*(volatile u32 *)0x600C0020u)
#define SYS_SPI2_BIT      (1u << 6)
#define SYS_SPI3_BIT      (1u << 16)

/* ---- 面板 ------------------------------------------------------------- */
#define PIN_SCLK 9u
#define PIN_SDA  10u
#define PIN_DC   8u
#define PIN_CS   14u
#define PIN_RST  18u
#define PIN_BL   13u
/* 原厂 xiaozhi-esp32 v2.5.0（nologo/xingzhi-cube-1.54tft-wifi）启动时用 RTC GPIO
 * 把 GPIO21 拉高，关机流程里把它拉 0 再 deep sleep —— 板级电源保持/使能脚。 */
#define PIN_PWR_HOLD 21u

#define LCD_W 240u
#define LCD_H 240u

#define SPI_CHUNK 64u          /* W0..W15 */

extern void s3_mark(u32 idx, u32 v);
static u32 g_xfers;

static void wr(volatile u32 *r, u32 v) { *r = v; }
static void hi(u32 p) { wr((volatile u32 *)GPIO_OUT_W1TS, 1u << p); }
static void lo(u32 p) { wr((volatile u32 *)GPIO_OUT_W1TC, 1u << p); }
/* 注意：这里**不能**用 volatile 计数。GCC 会给每个 volatile 访存插 memw
 * 内存屏障，而 SPI2 事务之后总有 APB 事务挂着没回，memw 会一直等它 →
 * CPU 当场停摆（串口静默、定时器中断也不再进，看起来像死机/复位）。
 * 实测：volatile 版本发完第一条面板命令就卡死，nop 版本整屏刷新正常。 */
static void dly(u32 n)
{
    u32 i = 0;
    while (i < n) {
        __asm__ __volatile__("nop");
        i++;
    }
}

/* 面板调试开关：上板定位 SPI2 时打印，定下来后置 0。 */
#define LCD_DEBUG 0
#if LCD_DEBUG
static void dbg(const char *s) { port_puts(s); }
static void dbg_hex(u32 v)
{
    static const char h[] = "0123456789abcdef";
    int i = 28;
    while (i >= 0) {
        port_putc((int)h[(v >> (u32)i) & 0xFu]);
        i -= 4;
    }
}
static void dbg_reg(const char *n, u32 v) { dbg(n); dbg_hex(v); dbg("\n"); }
#else
static void dbg(const char *s) { (void)s; }
static void dbg_hex(u32 v) { (void)v; }
static void dbg_reg(const char *n, u32 v) { (void)n; (void)v; }
#endif

/* IO_MUX：MCU_SEL=0（GPIO 功能）+ 上拉 + DRV=2，输入关掉（都是输出脚）。 */
static void mux_gpio(u32 p)
{
    volatile u32 *mux = (volatile u32 *)IO_MUX_REG(p);
    u32 v = *mux;
    v = (v & ~(7u << 12)) | (1u << 8) | (2u << 10);
    v = v & ~(1u << 9);
    *mux = v;
}

static void plain_out(u32 p)
{
    *(volatile u32 *)GPIO_OUT_SEL(p) = SIG_GPIO_OUT;   /* 走 GPIO_ENABLE/OUT */
    wr((volatile u32 *)GPIO_ENA_W1TS, 1u << p);
}

/* 把外设信号（101=FSPICLK / 103=FSPID / 110=FSPICS0）接到某根 pad 上。
 *
 * 两个都要做，少一个 pad 就是"高阻+内部上拉"，示波器/回读都只会看到 1：
 *   1) GPIO_FUNCx_OUT_SEL_CFG.func_sel = 信号号；
 *   2) 输出使能 GPIO_ENABLE_REG 对应位置 1（IDF 的 gpio_set_direction(OUTPUT)
 *      做的就是这件事）。只写 oen_sel 位不等于把 pad 打开。
 */
static void matrix_out(u32 p, u32 sig)
{
    mux_gpio(p);
    *(volatile u32 *)GPIO_OUT_SEL(p) = sig & 0x1FFu;   /* oen_sel=0 → 由 GPIO_ENABLE 控制 */
    *(volatile u32 *)GPIO_ENA_W1TS = 1u << p;
}

static void bb_dc(u32 level);

/* ---- bit-bang 兜底通路 ------------------------------------------------ */

/* 默认走**硬件 SPI**（20MHz）。2025 本轮找到"SCLK 打不出边沿"的真因：
 * SPI_CLOCK 的 CLKCNT_L 必须等于 CLKCNT_N（IDF: l = n），我们原来按
 * H+L=N 写成了 N/2 → 模块内部照常移位、MOSI 有边沿，但时钟不出脚。
 * 改正后真板实测 64 字节 = 512 个 SCLK 边沿，面板正常出图。
 * bit-bang 通路完整保留（s3_lcd_use_bitbang(1)），作为硬件 SPI 出问题时的兜底。 */
static u32 g_bitbang = 0;
static u32 g_hwspi;
static u32 g_bb_pad = 0;      /* bit-bang 每个半周期额外空转次数（降时钟） */
static u32 g_bb_fast = 1;     /* 默认用整字写 GPIO_OUT 的快速内层循环（快 2.2x） */
static u32 g_dc_inv = 0;      /* DC 极性取反（有些板子反相） */

void s3_lcd_use_bitbang(u32 on)
{
    g_bitbang = on;
    if (on != 0u) {
        plain_out(PIN_SCLK);
        plain_out(PIN_SDA);
        plain_out(PIN_CS);
        hi(PIN_SCLK);          /* mode 3：SCLK 空闲高 */
        lo(PIN_CS);            /* 常低选中 */
        lo(PIN_SDA);
    } else {
        matrix_out(PIN_SCLK, g_sig_clk);
        matrix_out(PIN_SDA, g_sig_d);
        /* CS 不用外设的 FSPICS0，直接普通 GPIO 拉低常选中：ST7789 靠 DC 区分
         * 命令/数据，CS 常低完全合法，也免得外设事务之间的 CS 时序成为变量。 */
        plain_out(PIN_CS);
        lo(PIN_CS);
    }
}

static void bb_write(const u8 *p, u32 n);

/* bit-bang 模式下读回任意 SPI 命令的返回字节：SDA 是普通 GPIO，可以在时钟
 * 低电平期间采样。用 RDDCOLMOD(0x0B) 读回我们写进去的 0x55 就能证明面板
 * 真的收到了初始化命令；RDDID(0x04) 则应回 0x85 0x85 0x52。
 * 注意：读之前必须把 SDA 的输出关掉（清 GPIO_ENABLE），否则两边抢总线。 */
int s3_lcd_read_bb(u32 cmd, u8 *out, u32 n)
{
    u32 i;
    u32 b;
    u32 v;

    if (g_bitbang == 0u || n == 0u || n > 8u) { return -1; }

    /* 命令字节（DC=0） */
    lo(PIN_DC);
    {
        u8 c = (u8)cmd;
        bb_write(&c, 1u);
    }
    hi(PIN_DC);

    /* 放开 SDA（三态），改成输入 */
    wr((volatile u32 *)GPIO_ENA_W1TC, 1u << PIN_SDA);
    *(volatile u32 *)IO_MUX_REG(PIN_SDA) |= (1u << 9);   /* FUN_IE */

    i = 0;
    while (i < n) {
        v = 0u;
        b = 0;
        while (b < 8u) {
            /* 下降沿面板换数据，在低电平期间采样，再抬时钟 */
            wr((volatile u32 *)GPIO_OUT_W1TC, 1u << PIN_SCLK);
            v = (v << 1) | ((*(volatile u32 *)(GPIO_BASE + 0x3Cu) >> PIN_SDA) & 1u);
            wr((volatile u32 *)GPIO_OUT_W1TS, 1u << PIN_SCLK);
            b++;
        }
        out[i] = (u8)v;
        i++;
    }

    /* 收回 SDA 输出 */
    wr((volatile u32 *)GPIO_ENA_W1TS, 1u << PIN_SDA);
    return 0;
}

void s3_lcd_set_bb_pad(u32 n)
{
    g_bb_pad = n;
}

void s3_lcd_set_dc_inv(u32 on)
{
    g_dc_inv = on;
}

static void bb_dc(u32 level)
{
    if ((level ^ g_dc_inv) != 0u) { hi(PIN_DC); } else { lo(PIN_DC); }
}

static void bb_delay(void)
{
    volatile u32 k = g_bb_pad;
    while (k != 0u) { k--; }
}

/* 快速版：整字写 GPIO_OUT（0x04），用阴影字保住 DC/CS/BL/RST 等静态位。
 * 每 bit 只要 2 次 store，比 W1TS/W1TC 版（3 次）少一次，也没有分支开销。
 * 阴影在每次调用入口按当前 GPIO_OUT 重新取，所以 hi/lo 改过 DC 也不会错。 */
void s3_lcd_set_bb_fast(u32 on)
{
    g_bb_fast = on;
}

static void bb_write_fast(const u8 *p, u32 n)
{
    volatile u32 *out = (volatile u32 *)(GPIO_BASE + 0x04u);
    u32 sh = *out & ~((1u << PIN_SCLK) | (1u << PIN_SDA));
    u32 i = 0;

    while (i < n) {
        u32 w = p[i];
        u32 b = 0;
        while (b < 8u) {
            u32 v = sh;
            if ((w & 0x80u) != 0u) { v = v | (1u << PIN_SDA); }
            *out = v;                              /* SCLK 低电平 */
            *out = v | (1u << PIN_SCLK);           /* SCLK 高电平 */
            w = w << 1;
            b++;
        }
        i++;
    }
}

static void bb_write_fast(const u8 *p, u32 n);

static void hw_xfer(const u8 *p, u32 n);

static void bb_write(const u8 *p, u32 n)
{
    if (g_hwspi != 0u) { hw_xfer(p, n); return; }

    if (g_bb_fast != 0u) { bb_write_fast(p, n); return; }
    u32 i;
    u32 b;
    u32 v;

    i = 0;
    while (i < n) {
        v = p[i];
        b = 0;
        while (b < 8u) {
            if ((v & 0x80u) != 0u) {
                wr((volatile u32 *)GPIO_OUT_W1TS, 1u << PIN_SDA);
            } else {
                wr((volatile u32 *)GPIO_OUT_W1TC, 1u << PIN_SDA);
            }
            v = v << 1;
            wr((volatile u32 *)GPIO_OUT_W1TC, 1u << PIN_SCLK);   /* 下降沿改数据 */
            if (g_bb_pad != 0u) { bb_delay(); }
            wr((volatile u32 *)GPIO_OUT_W1TS, 1u << PIN_SCLK);   /* 上升沿采样 */
            if (g_bb_pad != 0u) { bb_delay(); }
            b++;
        }
        i++;
    }
}

/* ---- SPI 传输 --------------------------------------------------------- */

static void bb_write(const u8 *p, u32 n);

/* 一次最多 64 字节（16 个字）：MSB first，W0 先发。 */
static void bb_write(const u8 *p, u32 n);
static void spi_xfer_hw(const u8 *buf, u32 len);

/* 统一的出口：bit-bang 模式下所有字节（命令/参数/像素）都必须走 GPIO 翻转。
 * 之前只有 cmd/dat 做了分派，而 fill/fill_rect/blit 里是直接调 spi_xfer，
 * 于是像素数据仍然从"打不出时钟"的硬件 SPI 出去 → 屏上一直是原厂残留画面。 */
static void spi_xfer(const u8 *buf, u32 len)
{
    if (g_bitbang != 0u) {
        bb_write(buf, len);
        return;
    }
    spi_xfer_hw(buf, len);
}

static void spi_xfer_hw(const u8 *buf, u32 len)
{
    u32 nw = (len + 3u) / 4u;

    g_xfers = g_xfers + 1u;
    s3_mark(1u, g_xfers);
    u32 i;
    u32 guard;

    for (i = 0; i < nw; i++) {
        u32 w = 0;
        u32 k = 0;
        while (k < 4u) {
            u32 idx = i * 4u + k;
            u32 b = (idx < len) ? (u32)buf[idx] : 0u;
            /* **低字节先出**：S3 GPSPI 的 data_buf 是从 bit[7:0] 开始往外移的
             * （IDF 的 spi_ll_write_buffer 就是直接 memcpy，不做字节交换）。
             * 之前按 MSB 打包，结果每 4 个字节被整组倒序；单个命令字节更是
             * 变成 0x00 → 面板一条命令都没收到（黑屏）。实测对照见
             * tests/t_s3_spiwire.c6：want=550ff0a53cc39966 收到 a5f00f556699c33c。 */
            w |= b << (8u * k);
            k++;
        }
        SPI2_W(i) = w;
    }
    SPI2_MS_DLEN = len * 8u - 1u;
    dbg("x1 dlen_written\n");
    /* 和 IDF spi_ll_user_start 一样：先 UPDATE 同步配置，等它自清之后再置 USR，
     * 两个写紧跟会丢掉配置同步。 */
    SPI2_CMD = SPI_CMD_UPDATE;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_UPDATE) != 0u) {
        guard++;
        if (guard > 100000u) { break; }
    }
    SPI2_CMD = SPI_CMD_USR;
    dbg("x3 usr, cmd=");
    dbg_hex(SPI2_CMD);
    dbg(" cc=");
    dbg_hex((u32)port_cycles());
    dbg("\n");
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_USR) != 0u) {
        guard++;
        if ((guard & 0x3FFFu) == 0u) {
            dbg("x4 polling cmd=");
            dbg_hex(SPI2_CMD);
            dbg(" cc=");
            dbg_hex((u32)port_cycles());
            dbg("\n");
        }
        if (guard > 2000000u) {
            dbg("xfer timeout cmd=");
            dbg_hex(SPI2_CMD);
            dbg(" clock=");
            dbg_hex(SPI2_CLOCK);
            dbg(" user=");
            dbg_hex(SPI2_USER);
            dbg("\n");
            return;
        }
    }
    dbg("x5 done raw=");
    dbg_hex(*(volatile u32 *)(SPI2_BASE + 0x3Cu));
    dbg(" ena=");
    dbg_hex(*(volatile u32 *)(SPI2_BASE + 0x34u));
    dbg(" clr=");
    dbg_hex(*(volatile u32 *)(SPI2_BASE + 0x38u));
    dbg("\n");
}

static void spi_write(const u8 *buf, u32 len)
{
    if (g_bitbang != 0u) {
        bb_write(buf, len);
        return;
    }
    while (len != 0u) {
        u32 n = (len > SPI_CHUNK) ? SPI_CHUNK : len;
        spi_xfer(buf, n);
        buf += n;
        len -= n;
    }
}

static void cmd(u8 c)
{
    dbg(" cmd ");
    dbg_hex((u32)c);
    dbg("\n");
    bb_dc(0u);
    /* 必须和 dat() 走同一条通路：bit-bang 模式下命令字节也必须用 GPIO 打，
     * 否则命令全走（这块板子上不出的）硬件 SPI，面板一条命令都收不到。 */
    if (g_bitbang != 0u) {
        bb_write(&c, 1u);
    } else {
        spi_xfer(&c, 1u);
    }
}

static void dat(const u8 *p, u32 n)
{
    bb_dc(1u);
    spi_write(p, n);
}

static void dat8(u8 v)
{
    hi(PIN_DC);
    spi_xfer(&v, 1u);
}

static void window(u32 x, u32 y, u32 w, u32 h)
{
    u8 b[4];
    u32 xe = x + w - 1u;
    u32 ye = y + h - 1u;

    b[0] = (u8)(x >> 8);  b[1] = (u8)x;  b[2] = (u8)(xe >> 8);  b[3] = (u8)xe;
    cmd(0x2Au);           /* CASET */
    dat(b, 4u);
    b[0] = (u8)(y >> 8);  b[1] = (u8)y;  b[2] = (u8)(ye >> 8);  b[3] = (u8)ye;
    cmd(0x2Bu);           /* RASET */
    dat(b, 4u);
    cmd(0x2Cu);           /* RAMWR */
}

static void spi_init(void)
{
    u32 clk;

    s3_mark(9u, 0x1001u);

    {
        u32 bit = (g_spi_base == 0x60025000u) ? SYS_SPI3_BIT : SYS_SPI2_BIT;
        SYS_PERIP_RST_EN0 |= bit;
        SYS_PERIP_CLK_EN0 |= bit;
        SYS_PERIP_RST_EN0 &= ~bit;
    }


    /* SPI2 的 DMA/TRANS_DONE 中断必须先关掉并清挂起：ROM 里它可能已经经中断
     * 矩阵接到了 CPU，而本内核的 level-1 向量只按 EXCCAUSE 分流（定时器/异常），
     * 一旦 SPI2 中断上来就会被当成定时器处理并反复重入 → 主流程卡死。 */
    *(volatile u32 *)(SPI2_BASE + 0x38u) = 0xFFFFFFFFu;   /* DMA_INT_CLR */
    *(volatile u32 *)(SPI2_BASE + 0x34u) = 0u;            /* DMA_INT_ENA */

    /* SPI_CLK_GATE（0xE8）：按 IDF spi_ll_master_init 三段都要置
     *   CLK_EN(bit0, "enable clk gate"，默认 0)
     *   MST_CLK_ACTIVE(bit1, 打开模块时钟)
     *   MST_CLK_SEL(bit2, 1=80MHz PLL 源 / 0=XTAL)
     * 少 bit0 时模块时钟被门控：写 CMD.USR 读回仍是 0、总线没有时钟输出，
     * 面板一个字节都收不到（但轮询看起来"很快完成"，很容易误判成正常）。 */
    *(volatile u32 *)(SPI2_BASE + 0xE8u) = (1u << 0) | (1u << 1) | (1u << 2);

    /* 源选 80MHz PLL（MST_CLK_SEL=1），分频 N+1=4 → 20MHz：
     * CLKDIV_PRE=0（bits21:18）、CLKCNT_N=3（17:12）、CLKCNT_H=1（11:6）、
     * CLKCNT_L=3（5:0）、CLK_EQU_SYSCLK=0（bit31）→ 0x00003043。
     *
     * 【2025 本轮定位】**CLKCNT_L 必须等于 CLKCNT_N**（IDF
     * esp_hal_gpspi/esp32s3/spi_ll.h: spi_ll_master_cal_clock() 里就是
     * `l = n; reg.clkcnt_l = l - 1;`），不是"高 H + 低 L = 一个周期 N"。
     * 这里曾按 H+L=N 写成 CLKCNT_L=1（0x3041，也是复位默认值），结果：
     * SPI 内部照常移位（MOSI 有边沿、事务耗时随分频线性变化），但
     * **SCLK 引脚不出任何边沿** → 面板只有背光。真板对照实验（同一次
     * 传输里软件轮询 GPIO9/10，78kHz 慢时钟）：
     *     L=N/2 → sclk9=0    mosi10=256
     *     L=N   → sclk9=1024 mosi10=256
     * 20MHz 下用 PCNT 复核同样结论（0 边沿 vs 512 边沿）。
     * 这就是"硬件 SPI 打不出时钟"的最终真因。 */
    clk = (g_equ_sysclk != 0u) ? 0x80003043u : 0x00003043u;
    dbg("clock_want=");
    dbg_hex(clk);
    dbg("\n");
    SPI2_CLOCK = clk;
    dbg_reg(" clock_now=", SPI2_CLOCK);

    /* 以下按 IDF spi_ll_master_init + spi_hal_init 逐项对齐（之前漏掉的）：
     * USER1 的 cs_setup/cs_hold 清 0、SLAVE 整个清 0（含 dma_seg_magic_value）、
     * DMA_CONF 的 seg_trans 清使能、MOSI 空闲电平 d_pol=0。 */
    SPI2_USER1 = 0u;
    SPI2_SLAVE = 0u;
    SPI2_DMA_CONF = (1u << 1) | (1u << 2);   /* tx/rx_seg_trans_clr_en=1 */
    SPI2_CTRL = 0u;                     /* MSB first，d_pol=0（MOSI 空闲低） */
    /* ST7789 是 mode 3（CPOL=1/CPHA=1）：MISC.CK_IDLE_EDGE=1（空闲高）+
     * USER 里**不置** CK_OUT_EDGE（数据在下降沿更新、面板在上升沿采样）。
     * 两者都置 1 会让"更新沿"和"采样沿"撞在一起，采到错位数据（实测见
     * §4.6.3，wire 测试直接能看出来）。 */
    SPI2_USER = SPI_USR_MOSI;
    /* CS 常低（ST7789 靠 DC 区分命令/数据，CS 一直有效是合法的，也避免
     * 命令字节和参数分属两次事务时面板接口状态机被 CS 打断）。 */
    SPI2_MISC = (g_equ_sysclk != 0u) ? (SPI_CK_IDLE_EDGE)          /* 照 IDF：不常低 CS */
                                      : (SPI_CK_IDLE_EDGE | SPI_CS_KEEP_ACTIVE);

    /* 注意：bit-bang 模式下这里不能把引脚接回 SPI 矩阵，否则后面的
     * GPIO_OUT 写不到 pad 上（init 序列就得用 bit-bang 打出去）。 */
    s3_lcd_use_bitbang(g_bitbang);
    plain_out(PIN_DC);
    plain_out(PIN_RST);
    plain_out(PIN_BL);
    hi(PIN_DC);

    s3_mark(9u, 0x1002u);
    dbg("spi2 ready\n");
    dbg_reg(" cmd=", SPI2_CMD);
    dbg_reg(" clock=", SPI2_CLOCK);
    dbg_reg(" user=", SPI2_USER);
    dbg_reg(" misc=", SPI2_MISC);
    dbg_reg(" slave=", *(volatile u32 *)(SPI2_BASE + 0xE0u));
}

/* dly() 的实测定标：**5 个 CPU 周期 / 单位**（80MHz 下 1 单位 = 62.5ns）。
 *   dly(1200000) 只有 6.0M cycles ≈ 75ms —— 不够 ST7789 要求的 120ms！
 * ST7789 手册要求：复位释放后 ≥120ms 才能发 SWRESET，SWRESET 后 ≥120ms，
 * SLPOUT 后 ≥120ms 才能发后续命令，DISPON 后再等 ≥20ms。
 * 命令发太早面板会直接不理（表现为只有背光），所以这里统一用 3000000
 * （15M cycles ≈ 187ms）留够余量。 */
#define DLY_120MS 3000000u   /* 187ms */
#define DLY_20MS  500000u    /* 31ms  */

static void init_seq(void)
{
    static const u8 b2[] = {0x55u};
    static const u8 b1[] = {0x00u};

    s3_mark(9u, 0x1003u);
    lo(PIN_RST); dly(200000u);           /* 12.5ms 复位低 */
    hi(PIN_RST); dly(DLY_120MS);         /* 复位释放后等 >=120ms */
    s3_mark(9u, 0x1004u);
    dbg("rst done\n");

    cmd(0x01u); dly(DLY_120MS);          /* SWRESET */
    s3_mark(9u, 0x1005u);
    cmd(0x11u); dly(DLY_120MS);          /* SLPOUT */
    s3_mark(9u, 0x1006u);
    cmd(0x3Au); dat(b2, 1u);             /* COLMOD 16bpp */
    cmd(0x36u); dat(b1, 1u);             /* MADCTL */

    /* ST7789 240x240 常用的电源/门控/gamma 表：不少屏在默认 VCOM/porch 下
     * 就是不亮，补齐这些（与各家开板示例一致，无副作用）。 */
    { static const u8 t[] = {0x0Cu, 0x0Cu, 0x00u, 0x33u, 0x33u}; cmd(0xB2u); dat(t, 5u); }
    { static const u8 t[] = {0x35u}; cmd(0xB7u); dat(t, 1u); }
    { static const u8 t[] = {0x19u}; cmd(0xBBu); dat(t, 1u); }
    { static const u8 t[] = {0x2Cu}; cmd(0xC0u); dat(t, 1u); }
    { static const u8 t[] = {0x01u}; cmd(0xC2u); dat(t, 1u); }
    { static const u8 t[] = {0x12u}; cmd(0xC3u); dat(t, 1u); }
    { static const u8 t[] = {0x20u}; cmd(0xC4u); dat(t, 1u); }
    { static const u8 t[] = {0x0Fu}; cmd(0xC6u); dat(t, 1u); }
    { static const u8 t[] = {0xA4u, 0xA1u}; cmd(0xD0u); dat(t, 2u); }
    { static const u8 t[] = {0xD0u, 0x04u, 0x0Du, 0x11u, 0x13u, 0x2Bu, 0x3Fu,
                             0x54u, 0x4Cu, 0x18u, 0x0Du, 0x0Bu, 0x1Fu, 0x23u};
      cmd(0xE0u); dat(t, 14u); }
    { static const u8 t[] = {0xD0u, 0x04u, 0x0Cu, 0x11u, 0x13u, 0x2Cu, 0x3Fu,
                             0x44u, 0x51u, 0x2Fu, 0x1Fu, 0x1Fu, 0x20u, 0x23u};
      cmd(0xE1u); dat(t, 14u); }
    cmd(0x21u);                          /* INVON */
    cmd(0x13u);                          /* NORON */
    dbg("cmds ok\n");
    cmd(0x29u); dly(DLY_20MS);           /* DISPON */
}

/* 给 c6 测试用的忙等延时（c6 不支持 inline asm，而 volatile 计数会插 memw）。 */
void s3_lcd_delay(u32 n)
{
    dly(n);
}

void s3_lcd_init(void)
{
    s3_mark(9u, 0x1000u);
    plain_out(PIN_PWR_HOLD);          /* 原厂：板级电源保持脚，启动即拉高 */
    hi(PIN_PWR_HOLD);
    spi_init();
    lo(PIN_RST);
    lo(PIN_BL);
    init_seq();
    hi(PIN_BL);
    s3_mark(9u, 0x1007u);
}

void s3_lcd_bl(u32 on)
{
    if (on != 0u) { hi(PIN_BL); } else { lo(PIN_BL); }
}

void s3_lcd_window(u32 x, u32 y, u32 w, u32 h)
{
    if (w == 0u || h == 0u || x >= LCD_W || y >= LCD_H) { return; }
    if (x + w > LCD_W) { w = LCD_W - x; }
    if (y + h > LCD_H) { h = LCD_H - y; }
    window(x, y, w, h);
}

void s3_lcd_pixels(const u16 *px, u32 n)
{
    u8 buf[SPI_CHUNK];
    u32 i = 0;
    u32 k;

    if (px == 0) { return; }
    while (i < n) {
        for (k = 0; k < SPI_CHUNK / 2u && i + k < n; k++) {
            u16 v = px[i + k];
            buf[k * 2u] = (u8)(v >> 8);
            buf[k * 2u + 1u] = (u8)v;
        }
        {
            u32 bytes = k * 2u;
            hi(PIN_DC);
            spi_xfer(buf, bytes);

        }
        i += k;
    }
}

void s3_lcd_fill(u16 color)
{
    u8 buf[SPI_CHUNK];
    u32 n = LCD_W * LCD_H;   /* 像素数 */
    u32 i = 0;
    u32 k;

    for (k = 0; k < SPI_CHUNK; k += 2u) {
        buf[k] = (u8)(color >> 8);
        buf[k + 1u] = (u8)color;
    }
    s3_mark(9u, 0x2001u);
    window(0u, 0u, LCD_W, LCD_H);
    hi(PIN_DC);
    while (i < n) {
        spi_xfer(buf, SPI_CHUNK);
        i += SPI_CHUNK / 2u;
    }
}

/* 读面板的 RDDID(0x04)：ST7789 应答 0x85 0x85 0x52（前面可能先来一个 dummy
 * 字节）。这是"面板到底有没有在听"的客观证据，不需要人眼。
 *
 * 关键三点：
 *   1) SDA 是双向脚：命令阶段由 MCU 驱动，读阶段必须把 pad 的输出关掉
 *      （OUT_SEL_CFG 里去掉 OEN_ALWAYS + GPIO_ENA 清位），否则两边抢总线；
 *   2) GPIO 输入矩阵要真的打开：IN_SEL 寄存器 bit6=sig_in_sel，
 *      bit[5:0]=pad 号，把 FSPIQ(102) 选到 SDA 那根 pad；
 *   3) IO_MUX 里 FUN_IE 置位。
 */
int s3_lcd_read_id(u8 *out, u32 n)
{
    u32 i;
    u32 w;
    u32 nw;
    u32 guard;
    volatile u32 *mux = (volatile u32 *)IO_MUX_REG(PIN_SDA);

    if (n == 0u || n > 4u) { return -1; }

    /* 输入通路：pad 输入使能 + FSPIQ 选到 SDA */
    *mux = *mux | (1u << 9);
    *(volatile u32 *)(GPIO_BASE + 0x154u + 4u * 102u) = (PIN_SDA & 0x3Fu) | (1u << 6);

    /* 命令字节 0x04（DC=0），SDA 由 MCU 驱动 */
    lo(PIN_DC);
    {
        static const u8 c = 0x04u;
        spi_xfer(&c, 1u);
    }
    /* 读阶段：放开 SDA（三态），DC=1 */
    *(volatile u32 *)GPIO_OUT_SEL(PIN_SDA) = SIG_GPIO_OUT;
    *(volatile u32 *)GPIO_ENA_W1TC = (1u << PIN_SDA);
    hi(PIN_DC);

    for (i = 0; i < SPI_CHUNK / 4u; i++) { SPI2_W(i) = 0u; }
    SPI2_USER = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN;
    SPI2_MS_DLEN = n * 8u - 1u;
    SPI2_CMD = SPI_CMD_UPDATE;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_UPDATE) != 0u) {
        guard++;
        if (guard > 2000000u) { return -3; }
    }
    SPI2_CMD = SPI_CMD_USR;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_USR) != 0u) {
        guard++;
        if (guard > 2000000u) { return -4; }
    }
    nw = (n + 3u) / 4u;
    for (i = 0; i < nw; i++) {
        w = SPI2_W(i);
        /* 和发送侧对称：RX FIFO 也是**低位字节是先收到的那一个** */
        out[i * 4u] = (u8)w;
        if (i * 4u + 1u < n) { out[i * 4u + 1u] = (u8)(w >> 8); }
        if (i * 4u + 2u < n) { out[i * 4u + 2u] = (u8)(w >> 16); }
        if (i * 4u + 3u < n) { out[i * 4u + 3u] = (u8)(w >> 24); }
    }
    SPI2_USER = SPI_USR_MOSI;
    *(volatile u32 *)GPIO_OUT_SEL(PIN_SDA) = SIG_GPIO_OUT | OEN_ALWAYS;
    return 0;
}

/* 调试用：把一根脚设成普通 GPIO 输出并给电平 / 读回输入电平。 */
void s3_pin_out(u32 pin)
{
    plain_out(pin);
}

void s3_pin_set(u32 pin, u32 v)
{
    if (v != 0u) { hi(pin); } else { lo(pin); }
}

u32 s3_pin_get(u32 pin)
{
    return (*(volatile u32 *)(GPIO_BASE + 0x3Cu) >> pin) & 1u;
}

/* 只收不发地读一根 pad 的电平（FSPIQ 输入矩阵 + SPI 时钟），用于确认
 * 输入通路本身是好的：把 pad 用普通 GPIO 驱动成 0/1，读回应分别是 0x00/0xFF。 */
int s3_spi_read_pin(u32 pin, u8 *rx, u32 n)
{
    u32 i;
    u32 w;
    u32 nw;
    u32 guard;
    volatile u32 *mux = (volatile u32 *)IO_MUX_REG(pin);

    if (n == 0u || n > SPI_CHUNK) { return -1; }
    *mux = *mux | (1u << 9);
    *(volatile u32 *)(GPIO_BASE + 0x154u + 4u * 102u) = (pin & 0x3Fu) | (1u << 6);

    for (i = 0; i < SPI_CHUNK / 4u; i++) { SPI2_W(i) = 0u; }
    SPI2_USER = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN;
    SPI2_MS_DLEN = n * 8u - 1u;
    SPI2_CMD = SPI_CMD_UPDATE;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_UPDATE) != 0u) {
        guard++;
        if (guard > 2000000u) { return -2; }
    }
    SPI2_CMD = SPI_CMD_USR;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_USR) != 0u) {
        guard++;
        if (guard > 2000000u) { return -3; }
    }
    nw = (n + 3u) / 4u;
    for (i = 0; i < nw; i++) {
        w = SPI2_W(i);
        rx[i * 4u] = (u8)(w >> 24);
        if (i * 4u + 1u < n) { rx[i * 4u + 1u] = (u8)(w >> 16); }
        if (i * 4u + 2u < n) { rx[i * 4u + 2u] = (u8)(w >> 8); }
        if (i * 4u + 3u < n) { rx[i * 4u + 3u] = (u8)w; }
    }
    SPI2_USER = SPI_USR_MOSI;
    return 0;
}

/* LOOPBACK 自检：把 FSPIQ（SPI 的 MISO 输入）映射到与 MOSI 同一根 pad，
 * 全双工发一段数据再读回 RX FIFO。读回等于发出 → 主控确实在 MOSI 上打数据；
 * 读回全 0 → 主控没把数据送出去（配置问题），与面板无关。 */
int s3_spi_loopback(u32 pin, const u8 *tx, u8 *rx, u32 n)
{
    u32 i;
    u32 w;
    u32 nw;
    u32 guard;
    volatile u32 *mux = (volatile u32 *)IO_MUX_REG(pin);

    if (n == 0u || n > SPI_CHUNK) { return -1; }

    /* pad 输入通路打开（FUN_IE），并把 FSPIQ 的输入选到这根 pad */
    *mux = *mux | (1u << 9);
    /* IN_SEL 寄存器：bit[5:0]=func_sel（选哪个 pad），bit6=sig_in_sel（使能）。
     * 之前漏了 bit6，输入通路没打开，读回来永远是 0xFF。 */
    *(volatile u32 *)(GPIO_BASE + 0x154u + 4u * 102u) = (pin & 0x3Fu) | (1u << 6);

    for (i = 0; i < SPI_CHUNK / 4u; i++) {
        SPI2_W(i) = 0u;
    }
    nw = (n + 3u) / 4u;
    for (i = 0; i < nw; i++) {
        w = 0;
        w |= (u32)tx[i * 4u] << 24;
        if (i * 4u + 1u < n) { w |= (u32)tx[i * 4u + 1u] << 16; }
        if (i * 4u + 2u < n) { w |= (u32)tx[i * 4u + 2u] << 8; }
        if (i * 4u + 3u < n) { w |= (u32)tx[i * 4u + 3u]; }
        SPI2_W(i) = w;
    }
    SPI2_USER = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN;
    SPI2_MS_DLEN = n * 8u - 1u;
    SPI2_CMD = SPI_CMD_UPDATE;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_UPDATE) != 0u) {
        guard++;
        if (guard > 2000000u) { return -2; }
    }
    SPI2_CMD = SPI_CMD_USR;
    guard = 0;
    while ((SPI2_CMD & SPI_CMD_USR) != 0u) {
        guard++;
        if (guard > 2000000u) { return -3; }
    }
    for (i = 0; i < nw; i++) {
        w = SPI2_W(i);
        rx[i * 4u] = (u8)(w >> 24);
        if (i * 4u + 1u < n) { rx[i * 4u + 1u] = (u8)(w >> 16); }
        if (i * 4u + 2u < n) { rx[i * 4u + 2u] = (u8)(w >> 8); }
        if (i * 4u + 3u < n) { rx[i * 4u + 3u] = (u8)w; }
    }
    SPI2_USER = SPI_USR_MOSI;
    return 0;
}

/* 给自检用例用：改分频 / 直接发一段数据 / 读回 CLOCK 与 CLK_GATE 寄存器。 */
void s3_lcd_set_clock(u32 clk) { SPI2_CLOCK = clk; }
u32 s3_lcd_get_clock(void) { return SPI2_CLOCK; }
u32 s3_lcd_get_gate(void) { return *(volatile u32 *)(SPI2_BASE + 0xE8u); }
void s3_lcd_dat_raw(const u8 *p, u32 n) { hi(PIN_DC); spi_write(p, n); }

/* 给调试用例用：直接发一条命令 / 一个参数。 */
void s3_lcd_cmd(u32 c) { if (g_bitbang != 0u) { bb_dc(0u); } cmd((u8)c); }
void s3_lcd_dat(u32 v) { u8 b = (u8)v; if (g_bitbang != 0u) { bb_dc(1u); } dat(&b, 1u); }

/* 只填一块矩形（带窗口），用来验证 CASET/RASET 寻址。 */
void s3_lcd_fill_rect(u32 x, u32 y, u32 w, u32 h, u16 color)
{
    u8 buf[SPI_CHUNK];
    u32 n;
    u32 i = 0;
    u32 k;

    if (w == 0u || h == 0u || x >= LCD_W || y >= LCD_H) { return; }
    if (x + w > LCD_W) { w = LCD_W - x; }
    if (y + h > LCD_H) { h = LCD_H - y; }
    for (k = 0; k < SPI_CHUNK; k += 2u) {
        buf[k] = (u8)(color >> 8);
        buf[k + 1u] = (u8)color;
    }
    n = w * h;
    window(x, y, w, h);
    hi(PIN_DC);
    while (i < n) {
        spi_xfer(buf, SPI_CHUNK);
        i += SPI_CHUNK / 2u;
    }
}

/* 把帧缓冲里的一块（行跨度 fb_w）贴到面板 (x,y) 起的 w*h 区域。 */
void s3_lcd_blit_rect(const u16 *fb, u32 fb_w, u32 x, u32 y, u32 w, u32 h)
{
    u8 buf[SPI_CHUNK];
    u32 n;
    u32 i = 0;
    u32 k;

    if (fb == 0 || w == 0u || h == 0u || x >= LCD_W || y >= LCD_H) { return; }
    if (x + w > LCD_W) { w = LCD_W - x; }
    if (y + h > LCD_H) { h = LCD_H - y; }
    n = w * h;
    window(x, y, w, h);
    while (i < n) {
        for (k = 0; k < SPI_CHUNK / 2u && i + k < n; k++) {
            u32 p = i + k;
            u16 v = fb[(y + p / w) * fb_w + (x + p % w)];
            buf[k * 2u] = (u8)(v >> 8);
            buf[k * 2u + 1u] = (u8)v;
        }
        {
            u32 bytes = k * 2u;
            hi(PIN_DC);
            spi_xfer(buf, bytes);

        }
        i += k;
    }
}

void s3_lcd_blit(const u16 *fb, u32 w, u32 h)
{
    if (fb == 0 || w == 0u || h == 0u) { return; }
    if (w > LCD_W) { w = LCD_W; }
    if (h > LCD_H) { h = LCD_H; }
    window(0u, 0u, w, h);
    s3_lcd_pixels(fb, w * h);
}

/* 调试：打开 pad 输入使能（FUN_IE）后读 GPIO_IN，用来检查输出脚的静态电平。 */
u32 s3_pin_level(u32 pin)
{
    volatile u32 *mux = (volatile u32 *)IO_MUX_REG(pin);

    *mux = *mux | (1u << 9);
    return (*(volatile u32 *)(GPIO_BASE + 0x3Cu) >> pin) & 1u;
}

/* 调试：起一次慢速（2MHz）64 字节传输后立刻返回，让调用方在传输进行中
 * 采样某根 pad 的电平——相当于给 SPI 做了个"逻辑分析仪"。 */
void s3_spi_begin(void)
{
    u32 i;

    for (i = 0; i < SPI_CHUNK / 4u; i++) { SPI2_W(i) = 0x55555555u; }
    /* 注意不能置 CLK_EQU_SYSCLK(bit31)，置了分频就被忽略（跑 80MHz）。
     * 125kHz：半周期 4us，远大于软件采样间隔，避免采样混叠。 */
    /* 125kHz：CLKDIV_PRE=9（前置 10 分频），N=63、H=32、L=N=63（L 必须等于 N）。 */
    SPI2_CLOCK = (9u << 18) | (63u << 12) | (31u << 6) | 63u;
    SPI2_MS_DLEN = SPI_CHUNK * 8u - 1u;
    SPI2_CMD = SPI_CMD_UPDATE;
    while ((SPI2_CMD & SPI_CMD_UPDATE) != 0u) { }
    SPI2_CMD = SPI_CMD_USR;
}

int s3_spi_end(void)
{
    u32 guard = 0;

    while ((SPI2_CMD & SPI_CMD_USR) != 0u) {
        guard++;
        if (guard > 4000000u) { return -1; }
    }
    SPI2_CLOCK = 0x00003043u;
    return 0;
}

/* 事务是否还在进行（USR 位） */
u32 s3_spi_busy(void)
{
    return SPI2_CMD & SPI_CMD_USR;
}

/* 读 SPI2 的 MISC/USER/CLOCK，便于确认配置是否写进去了 */
u32 s3_spi_reg(u32 off)
{
    return *(volatile u32 *)(SPI2_BASE + off);
}

/* 调试：读 IO_MUX（含 MCU_SEL/FUN_IE/FUN_WPU）与 GPIO_OUT_SEL_CFG 原始值。 */
u32 s3_reg32(u32 addr)
{
    return *(volatile u32 *)addr;
}

/* 调试：直接读写任意寄存器（排查 SPI 配置用） */
void s3_reg_wr(u32 addr, u32 v)
{
    *(volatile u32 *)addr = v;
}

/* 兜底：面板若因供电抖动自己复位（表现为画面定格、仍保留旧图），
 * 定期重发一遍初始化即可恢复。只发命令，不重做上电延时。 */
void s3_lcd_resync(void)
{
    s3_lcd_cmd(0x11u);          /* SLPOUT */
    s3_lcd_delay(500000u);
    s3_lcd_cmd(0x3Au); s3_lcd_dat(0x55u);   /* COLMOD 16bit */
    s3_lcd_cmd(0x36u); s3_lcd_dat(0x70u);   /* MADCTL */
    s3_lcd_cmd(0x21u);          /* INVON */
    s3_lcd_cmd(0x13u);          /* NORON */
    s3_lcd_cmd(0x29u);          /* DISPON */
    s3_lcd_delay(200000u);
}

/* 彻底复位面板：拉 RST + 重跑初始化表（软件兜底，用于面板接口状态卡死时） */
void s3_lcd_hard_reset(void)
{
    plain_out(PIN_RST);
    hi(PIN_RST);
    dly(200000u);
    lo(PIN_RST);
    dly(600000u);
    hi(PIN_RST);
    dly(3000000u);
    init_seq();
}

/* ================= 硬件 SPI2(FSPI) 通路 =================
 * S3 GPSPI2 寄存器偏移（曾用错 0x18/0x1C 导致"时钟不出来"）：
 *   CMD 0x00(USR bit24) / CTRL 0x08 / CLOCK 0x0C / USER 0x10 /
 *   MS_DLEN 0x1C / W0 0x98 / CLK_GATE 0xE8
 * GPIO 矩阵输出号：FSPICLK=101、FSPID=103、FSPICS0=110
 */
#define SPI2_BASE   0x60024000u
#define SPI2_CMD    (SPI2_BASE + 0x00u)
#define SPI2_CTRL   (SPI2_BASE + 0x08u)
#define SPI2_CLOCK  (SPI2_BASE + 0x0Cu)
#define SPI2_USER   (SPI2_BASE + 0x10u)
#define SPI2_DLEN   (SPI2_BASE + 0x1Cu)
#define SPI2_W0     (SPI2_BASE + 0x98u)
#define SPI2_GATE   (SPI2_BASE + 0xE8u)



static u32 rd_reg(u32 a)
{
    return *(volatile u32 *)a;
}

/* div: CLKCNT_N（80MHz/div），div=4 → 20MHz。
 * 照 IDF spi_ll_master_cal_clock()：n / h=占空比拍数 / **l = n**（不是 n-h）。 */
void s3_lcd_use_spi2hw(u32 div)
{
    u32 n = div;
    u32 h = div / 2u;

    if (n < 2u) { n = 2u; }
    if (n > 64u) { n = 64u; }
    if (h == 0u) { h = 1u; }
    if (h >= n) { h = n - 1u; }

    wr((volatile u32 *)0x600C0018u, rd_reg(0x600C0018u) | (1u << 6));   /* SPI2 clk en */
    wr((volatile u32 *)0x600C0020u, rd_reg(0x600C0020u) & ~(1u << 6));  /* 释放复位 */
    wr((volatile u32 *)SPI2_GATE, 7u);                                  /* CLK_EN|MST_ACTIVE|SEL */
    /* CLOCK: pre-1=0(即 pre=1), n-1, h-1, l-1（l = n） */
    wr((volatile u32 *)SPI2_CLOCK, (((n - 1u) & 0x3Fu) << 12) |
                                   (((h - 1u) & 0x3Fu) << 6) |
                                   ((n - 1u) & 0x3Fu));
    wr((volatile u32 *)SPI2_CTRL, 0u);                    /* MSB first */
    wr((volatile u32 *)SPI2_USER, (1u << 27) |            /* USR_MOSI */
                                  (1u << 7) | (1u << 6)); /* CS_SETUP/CS_HOLD */

    /* 应用配置：IDF 的 spi_ll_apply_config 是置 UPDATE(bit23) 并等它自清 */
    wr((volatile u32 *)SPI2_CMD, (1u << 23));
    while ((rd_reg(SPI2_CMD) & (1u << 23)) != 0u) { }

    /* 引脚走 GPIO 矩阵：SCLK=GPIO9←101，MOSI=GPIO10←103，CS 用普通 GPIO */
    matrix_out(PIN_SCLK, 101u);
    matrix_out(PIN_SDA, 103u);
    g_hwspi = 1u;
}

static void hw_xfer(const u8 *p, u32 n)
{
    u32 i = 0;

    if (n == 0u) { return; }
    while (i < n) {
        u32 w = 0;
        u32 k = 0;
        while (k < 4u && i + k < n) {
            w |= ((u32)p[i + k]) << (8u * k);   /* 低字节先出，见 spi_xfer_hw 注释 */
            k++;
        }
        wr((volatile u32 *)(SPI2_W0 + 4u * (i / 4u)), w);
        i += 4u;
    }
    while ((rd_reg(SPI2_CMD) & (1u << 24)) != 0u) { }  /* 等上一次结束 */
    wr((volatile u32 *)SPI2_DLEN, (n * 8u) - 1u);
    wr((volatile u32 *)SPI2_CMD, (1u << 24));       /* USR=1 启动 */
    while ((rd_reg(SPI2_CMD) & (1u << 24)) != 0u) { }  /* 等传输完成 */
}

/* 供自测：单独发一笔（最多 64 字节 = 16 个 FIFO 字） */
void s3_spi2_send(const u8 *p, u32 n)
{
    if (n > 64u) { n = 64u; }
    hw_xfer(p, n);
}
