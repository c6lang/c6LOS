/* Xingzhi Cube 1.54TFT WiFi 的喇叭：I2S0 主模式输出（引脚来自上游 config.h）
 *   BCLK = GPIO15   LRCK/WS = GPIO16   DOUT/SD = GPIO7
 * 上游用 NoAudioCodecSimplex（无 codec 芯片，I2S 直连功放），
 * 采样率 24kHz、16bit、立体声（BCLK = 24000*16*2 = 768kHz）。
 *
 * 这里只做"能出声"的最小实现：CPU 按采样率往 TX FIFO 喂样本（不做 DMA）。
 */
#include "c6os/types.h"
#include "c6os/port.h"

#define I2S_BASE      0x6000F000u
/* TX FIFO 写端口：S3 上有的资料写 0x00（与 RX 共用），也有 0x04 的独立
 * txfifo_wdata。先用 0x00，可用 s3_i2s_set_fifo_off() 切到 0x04 试。 */
static u32 g_fifo_off = 0x00u;
#define I2S_FIFO      (*(volatile u32 *)(I2S_BASE + g_fifo_off))
#define I2S_INT_RAW   (*(volatile u32 *)(I2S_BASE + 0x0Cu))
#define I2S_INT_CLR   (*(volatile u32 *)(I2S_BASE + 0x18u))
#define I2S_TX_CONF   (*(volatile u32 *)(I2S_BASE + 0x24u))
#define I2S_TX_CONF1  (*(volatile u32 *)(I2S_BASE + 0x2Cu))
#define I2S_TX_CLKM   (*(volatile u32 *)(I2S_BASE + 0x34u))
#define I2S_TX_CLKM_DIV (*(volatile u32 *)(I2S_BASE + 0x38u))

#define SYS_CLK_EN0   (*(volatile u32 *)0x600C0018u)
#define SYS_RST_EN0   (*(volatile u32 *)0x600C0020u)
#define I2S0_BIT      (1u << 4)

#define PIN_BCLK 15u
#define PIN_WS   16u
#define PIN_DOUT 7u

#define SIG_I2S_BCK 22u
#define SIG_I2S_WS  24u
#define SIG_I2S_SD  25u

static void wr(volatile u32 *r, u32 v) { *r = v; }

/* 把外设信号接到某根 pad：IO_MUX 选 GPIO 模式 + OUT_SEL + 输出使能 */
static void matrix_out(u32 pin, u32 sig)
{
    volatile u32 *mux = (volatile u32 *)(0x60009000u + 0x04u + 4u * pin);
    u32 v = *mux;

    v = (v & ~(7u << 12)) | (1u << 12);     /* MCU_SEL=1 = GPIO */
    *mux = v;
    *(volatile u32 *)(0x60004000u + 0x554u + 4u * pin) = sig & 0x1FFu;
    *(volatile u32 *)(0x60004000u + 0x24u) = 1u << pin;   /* GPIO_ENA_W1TS */
}

void s3_i2s_init(u32 sample_rate)
{
    /* 外设时钟/复位 */
    wr(&SYS_CLK_EN0, SYS_CLK_EN0 | I2S0_BIT);
    wr(&SYS_RST_EN0, SYS_RST_EN0 & ~I2S0_BIT);

    /* 引脚：I2S0O_BCK=22 / I2S0O_WS=24 / I2S0O_SD=25 */
    matrix_out(PIN_BCLK, SIG_I2S_BCK);
    matrix_out(PIN_WS, SIG_I2S_WS);
    matrix_out(PIN_DOUT, SIG_I2S_SD);

    /* 软复位（TXRST/ TX FIFO RST 是写 1 自清） */
    wr(&I2S_TX_CONF, (1u << 0) | (1u << 1));
    wr(&I2S_INT_CLR, 0xFFFFFFFFu);

    /* 时钟：源=PLL_160M(sel=1)，mclk = 160MHz/(div)，bck = mclk/(bck_div)
     * 目标 BCLK = sample_rate*32：div=104 → 1.538MHz，bck_div=1 → 769kHz ≈ 24kHz */
    wr(&I2S_TX_CLKM_DIV, (1u << 9));      /* 小数分频 y=1 基准，整数模式 */
    /* 时钟源先用 XTAL 40MHz（一定在跑，排除 PLL 依赖）：
     * 40MHz/26 = 1.538MHz mclk，bck_div=1 → 769kHz ≈ 24kHz×32 */
    wr(&I2S_TX_CLKM, (1u << 29)           /* CLK_EN —— 少它整个 I2S 没有时钟 */
                     | (0u << 27)          /* TX_CLK_SEL = 0 (XTAL 40MHz) */
                     | (1u << 26)          /* TX_CLK_ACTIVE */
                     | 26u);               /* TX_CLKM_DIV_NUM */
    /* TX_CONF1：16bit(15) | bck_div=1 | left align */
    wr(&I2S_TX_CONF1, (15u << 13) | (1u << 7) | (1u << 30));
    /* TX_CONF：立体声(chan_mod=0) + MSB first + 左对齐 + 启动 */
    wr(&I2S_TX_CONF, (1u << 2)             /* TX_START */
                     | (1u << 15)          /* TX_LEFT_ALIGN */
                     | (1u << 16));        /* TX_MSB_FIRST */

    (void)sample_rate;
}

/* 喂一个立体声帧（左/右各 16bit） */
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

void s3_i2s_set_fifo_off(u32 off)
{
    g_fifo_off = off;
}

void s3_i2s_silence(u32 samples)
{
    u32 i = 0;
    while (i < samples) {
        s3_i2s_write(0u, 0u);
        i++;
    }
}

/* ---- 软件 I2S（GPIO 翻转）--------------------------------------------
 * 这块 S3 的 I2S 是 EDMA-only：没有 CPU 可写的 TX FIFO，所以不配 DMA 时只能
 * 出"噗"一声。这里用纯 GPIO 打标准 I2S（Philips）波形当作可用通路：
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
    u32 sh = bb_shadow();

    /* 三根脚改成普通 GPIO 输出 */
    *(volatile u32 *)(0x60004000u + 0x554u + 4u * PIN_BCLK) = 0x100u;
    *(volatile u32 *)(0x60004000u + 0x554u + 4u * PIN_WS) = 0x100u;
    *(volatile u32 *)(0x60004000u + 0x554u + 4u * PIN_DOUT) = 0x100u;
    *(volatile u32 *)(0x60004000u + 0x24u) = (1u << PIN_BCLK) | (1u << PIN_WS) | (1u << PIN_DOUT);
    (void)sh;
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
