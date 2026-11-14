/* Xingzhi Cube 1.54TFT WiFi 的音频通路（真板验证版）：
 *   喇叭 I2S0（只发）：BCLK=GPIO15  WS=GPIO16  DOUT=GPIO7
 *   麦克风 I2S1（只收）：SCK=GPIO5   WS=GPIO4   DIN=GPIO6
 * 引脚/格式来自上游 xiaozhi `NoAudioCodecSimplex`，两个方向同一套标准格式：
 *   32bit 数据 / 32bit 槽（AUTO）/ 单声道 / 只左槽 / 左对齐 / MSB first / Philips
 *   → 每帧 64 个 BCLK，左槽 32bit 是样本，右槽不参与（TX 直接输出 0）。
 * 采样率：录音 16000（上游 AUDIO_INPUT_SAMPLE_RATE），放音也取 16k，这样
 * "录完直接回放"不需要重采样。
 *
 * 时钟：源 XTAL 40MHz、整数分频：mclk = 40e6/div，BCLK = mclk/bck_div(1)，
 *   一帧 64 BCLK → rate = 40e6/(div*64)。16kHz→div=39（16025.6Hz），
 *   24kHz→div=26。分频系数写 RX/TX_CLKM_DIV_CONF = 0（纯整数模式）。
 *
 * S3 的 I2S **没有 CPU 可读写的 FIFO**，收发全靠 GDMA：
 *   I2S0 TX → GDMA 通道0、触发号 3；I2S1 RX → GDMA 通道2、触发号 4
 *   （通道1 归面板 SPI，见 panel.c）。描述符沿用 lldesc（12 字节，
 *   size/length 只有 12 位 → 每段最多 4092 字节）。
 *
 * GDMA 侧的坑（和 panel.c 上屏那批是同一批，全部真板逼出来）：
 *   1) 描述符 owner(bit31) 必须置 1，否则报 dscr_err、一个字节都不搬；
 *   2) LINK 的地址字段是**相对 DRAM 基址 0x3FC00000 的偏移**（20 位），
 *      不是 "& 0xFFFFF"（本板描述符刚好落在 0x3FCA0000 附近时两者数值相等，
 *      换块缓冲就全 0，属于"看起来对"的假象）；
 *   3) OUT_CONF0 只设 eof_mode|outdscr_burst|data_burst，CONF1=0x2000，
 *      不要开 owner check（开了以后环形缓冲第二圈就报 owner error）；
 *   4) 武装完要**立刻**置 start，中间插延时会让 GDMA 空转报错。
 * 还有方向性：OUT_LINK 的 start 是 bit21（stop 20 / restart 22），
 *   IN_LINK 的 start 是 bit22（stop 21 / restart 23）——两个方向不一样。
 */
#include "c6os/types.h"
#include "c6os/port.h"

/* ---- 寄存器基址与偏移（I2S0/I2S1 完全相同）---------------------------- */
#define I2S0_BASE     0x6000F000u
#define I2S1_BASE     0x6002D000u

#define R_INT_RAW     0x0Cu
#define R_INT_CLR     0x18u
#define R_RX_CONF     0x20u
#define R_TX_CONF     0x24u
#define R_RX_CONF1    0x28u
#define R_TX_CONF1    0x2Cu
#define R_RX_CLKM     0x30u
#define R_TX_CLKM     0x34u
#define R_RX_CLKM_DIV 0x38u
#define R_TX_CLKM_DIV 0x3Cu
#define R_RX_TDM      0x50u
#define R_TX_TDM      0x54u
#define R_RXEOF_NUM   0x64u

#define I2S_R(base, off)   (*(volatile u32 *)((base) + (off)))

/* S3 的 STD 模式其实就是"2 槽 TDM"：IDF 的 i2s_hal_std_enable_tx_channel()
 * 调到 i2s_ll_tx_enable_std()，而它只做一件事——置 TX_CONF.tx_tdm_en(bit19)。
 * 少了这一位，槽/通道掩码那套逻辑根本没生效，TX FIFO 也就不会向 GDMA 要数据
 * （实测：BCLK/WS 正常、DOUT 恒 0、GDMA int_raw 恒 0、链路 park=0 干等）。
 * 左对齐(bit15) + pcm_bypass(bit12) 一起带上。 */
/* **tx_mono(bit5) + tx_mono_fst_vld(bit9) 必须开**：IDF 的
 * i2s_ll_tx_enable_mono_mode() 就是同时置这两位。不开的话硬件会认为一帧要
 * 从 FIFO 取两个样本（左槽 + 右槽），喂单声道语音就会**以 2 倍速播放**。
 * 之前只放方波听不出来，换成录音回放立刻就露馅。 */
#define TX_CONF_BASE  ((1u << 19) | (1u << 15) | (1u << 12) | (1u << 9) | (1u << 5))
/* 麦克风侧同理，另外加 rx_mono(bit5)：每帧只把左槽的 32bit 样本压进 FIFO。
 * **bit9(rx_mono_fst_vld) 必须一起置**：IDF 的 i2s_ll_rx_enable_mono_mode() 就是
 * 同时写 rx_mono 和 rx_mono_fst_vld 两位。bit9=0 的含义是"单声道模式取第二个槽"，
 * 而第二个槽的 chan_en 我们没开 → RX FIFO 一个字节都不推（描述符 owner 恒 1、
 * GDMA int_raw 恒 0，就是这么来的）。 */
#define RX_CONF_BASE  ((1u << 19) | (1u << 15) | (1u << 12) | (1u << 9) | (1u << 5))

/* ---- 外设时钟/复位（SYSTEM_PERIP_CLK_EN0 = 0x18 / RST_EN0 = 0x20）---- */
#define SYS_CLK_EN0   (*(volatile u32 *)0x600C0018u)
#define SYS_RST_EN0   (*(volatile u32 *)0x600C0020u)
#define SYS_CLK_EN1   (*(volatile u32 *)0x600C001Cu)
#define SYS_RST_EN1   (*(volatile u32 *)0x600C0024u)
#define I2S0_BIT      (1u << 4)
#define I2S1_BIT      (1u << 21)
#define DMA_BIT       (1u << 6)

/* ---- 引脚与 GPIO 矩阵信号号（soc/esp32s3/gpio_sig_map.h）------------- */
#define PIN_BCLK     15u
#define PIN_WS       16u
#define PIN_DOUT     7u
#define PIN_MIC_SCK  5u
#define PIN_MIC_WS   4u
#define PIN_MIC_DIN  6u

#define SIG_I2S_BCK   22u   /* I2S0O_BCK_OUT：TX 侧 BCK */
#define SIG_I2S_WS    24u   /* I2S0O_WS_OUT：TX 侧 WS */
#define SIG_I2S_SD    25u   /* I2S0O_SD_OUT / I2S0I_SD_IN */
#define SIG_I2S1_BCK  28u   /* I2S1O_BCK_OUT：I2S1 的 TX 侧 BCK */
#define SIG_I2S1_WS   29u   /* I2S1O_WS_OUT */
#define SIG_I2S1_SD   30u   /* I2S1O_SD_OUT / I2S1I_SD_IN */
/* **RX 主模式的时钟输出是另一组信号**：I2S0I_BCK_OUT=26 / I2S0I_WS_OUT=27，
 * I2S1I_BCK_OUT=31 / I2S1I_WS_OUT=32。用 TX 侧那组编号去接 pad 的话，
 * RX 时钟照样在内部跑、pad 上却永远是静态的（实测：一条边沿都没有）。 */
#define SIG_I2S0I_BCK 26u
#define SIG_I2S0I_WS  27u
#define SIG_I2S1I_BCK 31u
#define SIG_I2S1I_WS  32u

#define GPIO_BASE     0x60004000u
#define GPIO_OUT_W1TS (*(volatile u32 *)(GPIO_BASE + 0x24u))
#define IO_MUX(p)     (0x60009000u + 0x04u + 4u * (p))
#define GPIO_OUT_SEL(p)   (*(volatile u32 *)(GPIO_BASE + 0x554u + 4u * (p)))
#define GPIO_FUNC_IN(sig) (*(volatile u32 *)(GPIO_BASE + 0x154u + 4u * (sig)))

/* 把外设信号接到某根 pad：IO_MUX 选 GPIO 模式 + OUT_SEL + 输出使能 */
static void matrix_out(u32 pin, u32 sig)
{
    volatile u32 *mux = (volatile u32 *)IO_MUX(pin);
    u32 v = *mux;

    v = (v & ~(7u << 12)) | (1u << 12);     /* MCU_SEL=1 = GPIO */
    *mux = v;
    GPIO_OUT_SEL(pin) = sig & 0x1FFu;
    GPIO_OUT_W1TS = 1u << pin;              /* GPIO_ENABLE_W1TS */
}

/* 把某根 pad 接到外设输入：IO_MUX 输入使能 + GPIO 矩阵 in_sel（不反相）*/
static void matrix_in(u32 pin, u32 sig)
{
    volatile u32 *mux = (volatile u32 *)IO_MUX(pin);
    u32 v = *mux;

    v = (v & ~(7u << 12)) | (1u << 12);     /* MCU_SEL=1 = GPIO */
    v = v | (1u << 9);                      /* FUN_IE：输入使能 */
    *mux = v;
    GPIO_FUNC_IN(sig) = pin & 0x3Fu;        /* bit6=IN_INV 保持 0（不反相） */
}

/* 采样率 → 整数分频系数（一帧 64 BCLK，源 XTAL 40MHz） */
static u32 i2s_div(u32 rate)
{
    u32 d;

    if (rate == 0u) { rate = 16000u; }
    d = 40000000u / (64u * rate);
    if (d == 0u) { d = 1u; }
    if (d > 255u) { d = 255u; }
    return d;
}

/* ---- I2S0：喇叭发送 ---------------------------------------------------
 * 老版本把 TX_CONF1 写成 bits_mod=15 / half_sample_bits=0（16bit、半帧 1 个
 * BCLK），WS 每 2 个 BCLK 就翻一次、DMA 跟着空转 → 只出一声"噗"。
 * 这里按上游的 32bit 槽重新配：半帧 32 BCLK、帧 64 BCLK。
 */
void s3_i2s_init(u32 sample_rate)
{
    u32 div = i2s_div(sample_rate);

    SYS_CLK_EN0 = SYS_CLK_EN0 | I2S0_BIT;
    SYS_RST_EN0 = SYS_RST_EN0 & ~I2S0_BIT;

    matrix_out(PIN_BCLK, SIG_I2S_BCK);
    matrix_out(PIN_WS, SIG_I2S_WS);
    matrix_out(PIN_DOUT, SIG_I2S_SD);

    /* 复位：tx_reset / tx_fifo_reset 都是自清位，直接整写 */
    I2S_R(I2S0_BASE, R_TX_CONF) = (1u << 0) | (1u << 1);
    I2S_R(I2S0_BASE, R_INT_CLR) = 0xFFFFFFFFu;

    I2S_R(I2S0_BASE, R_TX_CLKM_DIV) = 0u;              /* 纯整数分频 */
    I2S_R(I2S0_BASE, R_TX_CLKM) = (1u << 29)           /* clk_en：模块时钟门控 */
                                | (1u << 26)           /* tx_clk_active */
                                | (0u << 27)           /* 源 = XTAL 40MHz */
                                | div;

    /* CONF1：ws=32 / bck_div=1 / 32bit 数据 / 半帧 32 / 通道 32bit / MSB shift */
    I2S_R(I2S0_BASE, R_TX_CONF1) = (31u << 0) | (0u << 7) | (31u << 13)
                                 | (31u << 18) | (31u << 24) | (1u << 29);
    /* TDM_CTRL：总共 2 个槽（tot_chan_num=1），只使能左槽（chan0） */
    I2S_R(I2S0_BASE, R_TX_TDM) = (1u << 16) | 1u;

    /* CONF：tdm_en + 左对齐 + pcm bypass，再 update 一次推进 I2S 时钟域 */
    I2S_R(I2S0_BASE, R_TX_CONF) = TX_CONF_BASE;
    I2S_R(I2S0_BASE, R_TX_CONF) = TX_CONF_BASE | (1u << 8);
    while ((I2S_R(I2S0_BASE, R_TX_CONF) & (1u << 8)) != 0u) { }
}

/* ---- 16bit 槽变体：为"录 5 秒"准备 -------------------------------------
 * 32bit 槽在 16kHz 下每样本 4 字节，5 秒就要 320KB，DRAM 装不下（见
 * docs/todo.md 里 0x3FCD9500 坏区的说明）。换成 16bit 槽后每样本 2 字节，
 * 5 秒 = 160KB 正好塞得下；单声道下一帧吃 16bit，缓冲区就是紧凑的 u16 数组。
 * 帧 = 32 BCLK，所以分频是 40MHz/(32*rate)。
 */
void s3_i2s_init16(u32 sample_rate)
{
    u32 div;

    if (sample_rate == 0u) { sample_rate = 16000u; }
    div = 40000000u / (32u * sample_rate);
    if (div == 0u) { div = 1u; }
    if (div > 255u) { div = 255u; }

    SYS_CLK_EN0 = SYS_CLK_EN0 | I2S0_BIT;
    SYS_RST_EN0 = SYS_RST_EN0 & ~I2S0_BIT;

    matrix_out(PIN_BCLK, SIG_I2S_BCK);
    matrix_out(PIN_WS, SIG_I2S_WS);
    matrix_out(PIN_DOUT, SIG_I2S_SD);

    I2S_R(I2S0_BASE, R_TX_CONF) = (1u << 0) | (1u << 1);
    I2S_R(I2S0_BASE, R_INT_CLR) = 0xFFFFFFFFu;

    I2S_R(I2S0_BASE, R_TX_CLKM_DIV) = 0u;
    I2S_R(I2S0_BASE, R_TX_CLKM) = (1u << 29) | (1u << 26) | (0u << 27) | div;

    /* CONF1：16bit 槽 —— ws=16 / bits_mod=16 / 半帧 16 / 通道 16 */
    I2S_R(I2S0_BASE, R_TX_CONF1) = (15u << 0) | (0u << 7) | (15u << 13)
                                 | (15u << 18) | (15u << 24) | (1u << 29);
    I2S_R(I2S0_BASE, R_TX_TDM) = (1u << 16) | 1u;

    I2S_R(I2S0_BASE, R_TX_CONF) = TX_CONF_BASE;
    I2S_R(I2S0_BASE, R_TX_CONF) = TX_CONF_BASE | (1u << 8);
    while ((I2S_R(I2S0_BASE, R_TX_CONF) & (1u << 8)) != 0u) { }
}

void s3_i2s_tx_start(void)
{
    I2S_R(I2S0_BASE, R_TX_CONF) = I2S_R(I2S0_BASE, R_TX_CONF) | (1u << 2);
}

void s3_i2s_tx_stop(void)
{
    I2S_R(I2S0_BASE, R_TX_CONF) = I2S_R(I2S0_BASE, R_TX_CONF) & ~(1u << 2);
}

/* ---- I2S1：麦克风接收 -------------------------------------------------
 * 与 TX 完全相同的槽格式；只是方向反过来，另外要配 RXEOF_NUM（每段字节数，
 * 单位是**字节**，必须等于描述符的 size，否则 DMA 一拍永远等不到 eof）。
 */
void s3_i2s_rx_init(u32 sample_rate)
{
    u32 div = i2s_div(sample_rate);

    SYS_CLK_EN0 = SYS_CLK_EN0 | I2S1_BIT;
    SYS_RST_EN0 = SYS_RST_EN0 & ~I2S1_BIT;

    matrix_out(PIN_MIC_SCK, SIG_I2S1I_BCK);     /* 主机：给 mic 送 BCLK（RX 侧信号 31） */
    matrix_out(PIN_MIC_WS, SIG_I2S1I_WS);       /* 主机：给 mic 送 WS（RX 侧信号 32） */
    matrix_in(PIN_MIC_DIN, SIG_I2S1_SD);        /* mic → ESP32（数据输入 30） */

    I2S_R(I2S1_BASE, R_RX_CONF) = (1u << 0) | (1u << 1);
    I2S_R(I2S1_BASE, R_INT_CLR) = 0xFFFFFFFFu;

    I2S_R(I2S1_BASE, R_RX_CLKM_DIV) = 0u;
    I2S_R(I2S1_BASE, R_TX_CLKM_DIV) = 0u;
    /* 模块时钟门控位在 TX_CLKM_CONF 的 clk_en（IDF i2s_ll_enable_module_clock
     * 就是写这一位，RX-only 实例也必须打开）；RX 自己的分频在 RX_CLKM_CONF */
    I2S_R(I2S1_BASE, R_TX_CLKM) = (1u << 29) | (1u << 26) | div;
    I2S_R(I2S1_BASE, R_RX_CLKM) = (1u << 29)           /* mclk_sel=1：mclk 绑到 rx clk */
                                | (1u << 26)           /* rx_clk_active */
                                | (0u << 27)           /* 源 = XTAL 40MHz */
                                | div;

    I2S_R(I2S1_BASE, R_RX_CONF1) = (31u << 0) | (0u << 7) | (31u << 13)
                                 | (31u << 18) | (31u << 24) | (1u << 29);
    I2S_R(I2S1_BASE, R_RX_TDM) = (1u << 16) | 1u;      /* 2 槽，只存左槽 */
    I2S_R(I2S1_BASE, R_RXEOF_NUM) = 0u;                /* 由 capture 按段大小填 */

    /* CONF：tdm_en（S3 的 STD 就是 2 槽 TDM）+ rx_mono + 左对齐 + pcm bypass */
    I2S_R(I2S1_BASE, R_RX_CONF) = RX_CONF_BASE;
    I2S_R(I2S1_BASE, R_RX_CONF) = RX_CONF_BASE | (1u << 8);
    while ((I2S_R(I2S1_BASE, R_RX_CONF) & (1u << 8)) != 0u) { }
}

/* ---- GDMA 通道寄存器（通道步长 0xC0）--------------------------------- */
#define GDMA_BASE     0x6003F000u
#define GDMA_STRIDE   0xC0u
#define I2S_TX_CH     0u            /* 通道1 留给面板 SPI */
#define I2S_RX_CH     2u
#define I2S_TRIG_TX   3u            /* SOC_GDMA_TRIG_PERIPH_I2S0 */
#define I2S_TRIG_RX   4u            /* SOC_GDMA_TRIG_PERIPH_I2S1 */

#define GDMA_IN_CONF0(ch)    (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x00u))
#define GDMA_IN_CONF1(ch)    (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x04u))
#define GDMA_IN_INT_RAW(ch)  (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x08u))
#define GDMA_IN_INT_CLR(ch)  (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x14u))
#define GDMA_IN_LINK(ch)     (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x20u))
#define GDMA_IN_PERI(ch)     (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x48u))
#define GDMA_OUT_CONF0(ch)   (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x60u))
#define GDMA_OUT_CONF1(ch)   (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x64u))
#define GDMA_OUT_INT_RAW(ch) (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x68u))
#define GDMA_OUT_INT_CLR(ch) (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x74u))
#define GDMA_OUT_LINK(ch)    (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0x80u))
#define GDMA_OUT_PERI(ch)    (*(volatile u32 *)(GDMA_BASE + (ch) * GDMA_STRIDE + 0xA8u))

/* LINK 的地址字段 = 描述符地址 − DRAM 基址（20 位）。见文件头第 2 条坑。 */
#define I2S_DRAM_BASE 0x3FC00000u
#define I2S_LINK_ADDR(d)  (((u32)(d) - I2S_DRAM_BASE) & 0xFFFFFu)

#define I2S_DMA_SEG   4092u        /* 每段字节数（lldesc size 12 位上限） */
#define I2S_DESC_MAX  64u          /* 64 × 4092 ≈ 262KB（描述符本身才 768 字节） */

typedef struct {
    u32 w0;     /* size[11:0] | length[23:12] | offset[28:24] | sosf | eof | owner */
    u32 buf;
    u32 next;
} I2sDesc;

static I2sDesc g_tx_desc[I2S_DESC_MAX];
static I2sDesc g_rx_desc[I2S_DESC_MAX];
static u32 g_rx_segs;

void s3_gdma_init(void)
{
    SYS_CLK_EN1 = SYS_CLK_EN1 | DMA_BIT;
    SYS_RST_EN1 = SYS_RST_EN1 & ~DMA_BIT;
}

/* 建链：每段 ≤ 4092 字节，owner=1、eof=1（DMA 每段结束都写回描述符）。
 * ring=1 时最后一段的 next 指回第一段，形成无限循环。 */
static void chain_build(I2sDesc *d, u32 segs, const void *buf, u32 bytes, u32 ring)
{
    u32 i = 0;
    u32 left = bytes;

    while (i < segs && left != 0u) {
        u32 take = (left > I2S_DMA_SEG) ? I2S_DMA_SEG : left;

        d[i].w0 = (take & 0xFFFu) | ((take & 0xFFFu) << 12) | (1u << 30) | (1u << 31);
        d[i].buf = (u32)buf + i * I2S_DMA_SEG;
        d[i].next = (u32)&d[i + 1u];
        left = left - take;
        i++;
    }
    d[i - 1u].next = (ring != 0u) ? (u32)&d[0] : 0u;
}

/* ---- 放音：一段缓冲（32bit/帧，单声道左槽）交给 GDMA 反复推给 I2S0 ---- */
void s3_i2s_play(const void *buf, u32 bytes, u32 loop)
{
    u32 segs = (bytes + I2S_DMA_SEG - 1u) / I2S_DMA_SEG;

    if (buf == 0 || segs == 0u) { return; }
    if (segs > I2S_DESC_MAX) { segs = I2S_DESC_MAX; }
    chain_build(g_tx_desc, segs, buf, bytes, loop);

    s3_gdma_init();

    /* TX 的 FSM/FIFO 复位（读改写，别碰配置位） */
    I2S_R(I2S0_BASE, R_TX_CONF) = I2S_R(I2S0_BASE, R_TX_CONF) | (1u << 0);
    I2S_R(I2S0_BASE, R_TX_CONF) = I2S_R(I2S0_BASE, R_TX_CONF) & ~(1u << 0);
    I2S_R(I2S0_BASE, R_TX_CONF) = I2S_R(I2S0_BASE, R_TX_CONF) | (1u << 1);
    I2S_R(I2S0_BASE, R_TX_CONF) = I2S_R(I2S0_BASE, R_TX_CONF) & ~(1u << 1);

    GDMA_OUT_CONF0(I2S_TX_CH) = 1u;                        /* out_rst */
    GDMA_OUT_CONF0(I2S_TX_CH) = 0u;
    /* eof_mode | outdscr_burst | data_burst（不开 owner check / auto_wrback） */
    GDMA_OUT_CONF0(I2S_TX_CH) = (1u << 3) | (1u << 4) | (1u << 5);
    GDMA_OUT_CONF1(I2S_TX_CH) = 0x2000u;
    GDMA_OUT_PERI(I2S_TX_CH) = I2S_TRIG_TX;
    GDMA_OUT_INT_CLR(I2S_TX_CH) = 0xFFFFFFFFu;
    GDMA_OUT_LINK(I2S_TX_CH) = I2S_LINK_ADDR(&g_tx_desc[0]);
    GDMA_OUT_LINK(I2S_TX_CH) = I2S_LINK_ADDR(&g_tx_desc[0]) | (1u << 21);   /* start */

    s3_i2s_tx_start();
}

void s3_i2s_play_stop(void)
{
    s3_i2s_tx_stop();
    GDMA_OUT_LINK(I2S_TX_CH) = (GDMA_OUT_LINK(I2S_TX_CH) & 0xFFFFFu) | (1u << 20);
    GDMA_OUT_CONF0(I2S_TX_CH) = 1u;                        /* out_rst */
    GDMA_OUT_CONF0(I2S_TX_CH) = 0u;
}

/* ---- 录音：麦克风数据经 GDMA 直接落进调用方的缓冲 -------------------
 * 返回实际使用的字节数（一定是 I2S_DMA_SEG 的整数倍）。缓冲按段顺序填，
 * 填满后环回覆盖，所以调用方要在填满前调用 s3_i2s_rx_stop()。
 */
u32 s3_i2s_rx_capture(void *buf, u32 bytes)
{
    u32 segs = bytes / I2S_DMA_SEG;

    if (buf == 0 || segs == 0u) { return 0u; }
    if (segs > I2S_DESC_MAX) { segs = I2S_DESC_MAX; }

    chain_build(g_rx_desc, segs, buf, segs * I2S_DMA_SEG, 1u);
    g_rx_segs = segs;

    s3_gdma_init();

    /* RX FSM / FIFO 复位 */
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) | (1u << 0);
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) & ~(1u << 0);
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) | (1u << 1);
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) & ~(1u << 1);

    /* RXEOF_NUM 必须等于每段字节数，DMA 才知道一拍什么时候"收满" */
    I2S_R(I2S1_BASE, R_RXEOF_NUM) = I2S_DMA_SEG;

    GDMA_IN_CONF0(I2S_RX_CH) = 1u;                         /* in_rst */
    GDMA_IN_CONF0(I2S_RX_CH) = 0u;
    GDMA_IN_CONF0(I2S_RX_CH) = (1u << 2) | (1u << 3);      /* indscr_burst | data_burst */
    GDMA_IN_CONF1(I2S_RX_CH) = 12u;                        /* infifo 水位，owner check 关 */
    GDMA_IN_PERI(I2S_RX_CH) = I2S_TRIG_RX;
    GDMA_IN_INT_CLR(I2S_RX_CH) = 0xFFFFFFFFu;
    GDMA_IN_LINK(I2S_RX_CH) = I2S_LINK_ADDR(&g_rx_desc[0]);
    GDMA_IN_LINK(I2S_RX_CH) = I2S_LINK_ADDR(&g_rx_desc[0]) | (1u << 22);   /* start */

    /* 先 GDMA 后 I2S 接收（IDF i2s_rx_channel_start 的顺序） */
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) | (1u << 8);
    while ((I2S_R(I2S1_BASE, R_RX_CONF) & (1u << 8)) != 0u) { }
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) | (1u << 2);  /* rx_start */

    return segs * I2S_DMA_SEG;
}

void s3_i2s_rx_stop(void)
{
    I2S_R(I2S1_BASE, R_RX_CONF) = I2S_R(I2S1_BASE, R_RX_CONF) & ~(1u << 2);
    GDMA_IN_LINK(I2S_RX_CH) = (GDMA_IN_LINK(I2S_RX_CH) & 0xFFFFFu) | (1u << 21);  /* stop */
    GDMA_IN_CONF0(I2S_RX_CH) = 1u;
    GDMA_IN_CONF0(I2S_RX_CH) = 0u;
}

/* 已经录到的字节数：按描述符顺序数"owner 已经被硬件清掉"的段。
 * （RX 方向硬件总是写回描述符：owner→0、length→实际收到的字节数） */
u32 s3_i2s_rx_filled(void)
{
    u32 i = 0;
    u32 n = 0;

    while (i < g_rx_segs) {
        u32 w0 = g_rx_desc[i].w0;

        if ((w0 & (1u << 31)) != 0u) { break; }      /* 这一段还在收 */
        n = n + ((w0 >> 12) & 0xFFFu);
        i++;
    }
    return n;
}

/* ---- 诊断：给真板用例读 GDMA 状态（dscr_err 等）---------------------- */
u32 s3_i2s_tx_int_raw(void) { return GDMA_OUT_INT_RAW(I2S_TX_CH); }
u32 s3_i2s_rx_int_raw(void) { return GDMA_IN_INT_RAW(I2S_RX_CH); }
u32 s3_i2s_tx_link(void)    { return GDMA_OUT_LINK(I2S_TX_CH); }
u32 s3_i2s_rx_link(void)    { return GDMA_IN_LINK(I2S_RX_CH); }
u32 s3_i2s_seg(void)        { return I2S_DMA_SEG; }
u32 s3_i2s_desc_addr(void)  { return (u32)&g_tx_desc[0]; }

/* 老接口（tests/t_s3_spk_dma.c6 还在用）：frames 个"立体声 u16 帧"=4 字节/帧，
 * 正好等于新格式里"单声道 32bit 一帧"的字节数。 */
void s3_i2s_dma_play(const u16 *samples, u32 frames, u32 ch)
{
    (void)ch;
    s3_i2s_play(samples, frames * 4u, 1u);
}

/* ---- 以下为历史通路（保留给老的诊断用例）----------------------------- */

/* 老的 TX FIFO 写端口：S3 上 I2S 没有 CPU 可写的 FIFO，这两个函数只为兼容
 * 早期用例保留，实际不出声。 */
static u32 g_fifo_off = 0x00u;
#define I2S_FIFO      (*(volatile u32 *)(I2S0_BASE + g_fifo_off))

void s3_i2s_set_fifo_off(u32 off)
{
    g_fifo_off = off;
}

void s3_i2s_write(u16 left, u16 right)
{
    I2S_FIFO = ((u32)right << 16) | (u32)left;
}

/* 发一段方波（用 CPU 节拍控制采样率）：freq 赫兹，samples 个采样（立体声） */
void s3_i2s_tone(u32 freq, u32 samples, u32 sample_rate)
{
    u32 period = sample_rate / freq;
    u32 i = 0;
    u32 last = (u32)port_cycles();
    u32 step = 80000000u / sample_rate;      /* 每个采样的周期数 */

    while (i < samples) {
        u16 v;
        while (((u32)port_cycles() - last) < step) { }
        last = (u32)port_cycles();
        v = (((i % period) * 2u) < period) ? 0x4000u : 0xC000u;
        s3_i2s_write(v, v);
        i++;
    }
}

void s3_i2s_silence(u32 samples)
{
    u32 i = 0;
    while (i < samples) {
        s3_i2s_write(0u, 0u);
        i++;
    }
}

/* ---- 软件 I2S（GPIO 翻转，历史通路，真板可用）------------------------
 * S3 的 I2S 是 EDMA-only：没有 CPU 可写的 TX FIFO，所以在 GDMA 打通之前
 * 只能用 GPIO 打标准 I2S（Philips）波形：
 *   BCLK 下降沿换数据、上升沿被采样，WS=0 左声道 / 1 右声道，16bit MSB first，
 *   WS 变化后延迟一个 BCLK 再出数据。
 * 开销：24kHz×32bit = 768k bit/s，每 bit 约 30-40 周期 → 约占用 1/3 CPU。
 */
#define BB_GPIO_OUT (*(volatile u32 *)(0x60004000u + 0x04u))

static u32 bb_shadow(void)
{
    /* 保住 DC/CS/BL 等静态位，只清掉我们要动的三根 */
    return BB_GPIO_OUT & ~((1u << PIN_BCLK) | (1u << PIN_WS) | (1u << PIN_DOUT));
}

void s3_i2s_bb_init(void)
{
    /* 三根脚改成普通 GPIO 输出 */
    GPIO_OUT_SEL(PIN_BCLK) = 0x100u;
    GPIO_OUT_SEL(PIN_WS) = 0x100u;
    GPIO_OUT_SEL(PIN_DOUT) = 0x100u;
    GPIO_OUT_W1TS = (1u << PIN_BCLK) | (1u << PIN_WS) | (1u << PIN_DOUT);
}

/* 每个半周期空转（注意不能用 volatile：会插 memw，慢几十倍） */
static void bb_wait(u32 n)
{
    u32 k = n;
    while (k != 0u) { k--; }
}

/* 输出一帧：按上游 NoAudioCodecSimplex 的格式
 *   data_bit_width=32bit、slot_bit_width=AUTO(32)、slot_mode=MONO、
 *   slot_mask=LEFT、ws_width=32bit、left_align
 * 即每帧 64 个 BCLK：WS=0 的 32 bit 为左声道（高 16 bit 放样本，低 16 bit 补 0），
 * WS=1 的 32 bit 为右声道（全 0）。功放只取左声道，所以右声道填 0 即可。 */
void s3_i2s_bb_frame(u16 left, u16 right)
{
    u32 sh = bb_shadow();
    u32 v;
    int b;

    (void)right;

    /* 左声道：WS=0 */
    sh = sh & ~(1u << PIN_WS);
    BB_GPIO_OUT = sh;
    b = 31;
    while (b >= 0) {
        v = sh;
        if (b >= 16) {
            if (((left >> (b - 16)) & 1u) != 0u) { v = v | (1u << PIN_DOUT); }
        } else {
            v = v & ~(1u << PIN_DOUT);      /* 低 16 bit 补 0 */
        }
        BB_GPIO_OUT = v & ~(1u << PIN_BCLK);    /* 下降沿换数据 */
        bb_wait(1u);
        BB_GPIO_OUT = v | (1u << PIN_BCLK);     /* 上升沿采样 */
        bb_wait(1u);
        b--;
    }

    /* 右声道：WS=1，整槽 0 */
    sh = (sh | (1u << PIN_WS)) & ~(1u << PIN_DOUT);
    BB_GPIO_OUT = sh;
    b = 31;
    while (b >= 0) {
        v = sh;
        BB_GPIO_OUT = v & ~(1u << PIN_BCLK);
        bb_wait(1u);
        BB_GPIO_OUT = v | (1u << PIN_BCLK);
        bb_wait(1u);
        b--;
    }
}

/* 软件 I2S 放一段方波：freq 赫兹、dur_ms 毫秒、rate 采样率 */
void s3_i2s_bb_tone(u32 freq, u32 dur_ms, u32 rate)
{
    u32 frames = (rate / 1000u) * dur_ms;
    u32 period = rate / freq;
    u32 i = 0;

    if (period == 0u) { period = 2u; }
    while (i < frames) {
        u16 v = (((i % period) * 2u) < period) ? 0x2000u : 0xE000u;
        s3_i2s_bb_frame(v, v);
        i++;
    }
}
