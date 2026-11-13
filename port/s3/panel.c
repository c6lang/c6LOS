/* Xingzhi Cube 1.54TFT WiFi：ST7789 240x240 SPI 屏，bit-bang 驱动。
 * SDA=10 SCL=9 DC=8 CS=14 RES=18 BL=13。 */
#include "c6os/types.h"
#include "c6os/port.h"

#define GPIO_BASE      0x60004000u
#define GPIO_OUT_W1TS  (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC  (GPIO_BASE + 0x0Cu)
#define GPIO_ENA_W1TS  (GPIO_BASE + 0x24u)

#define PIN_SDA 10u
#define PIN_SCL 9u
#define PIN_DC  8u
#define PIN_CS  14u
#define PIN_RST 18u
#define PIN_BL  13u

#define LCD_W 240u
#define LCD_H 240u

static void wr(volatile u32 *r, u32 v) { *r = v; }
static void hi(u32 p) { wr((volatile u32 *)GPIO_OUT_W1TS, 1u << p); }
static void lo(u32 p) { wr((volatile u32 *)GPIO_OUT_W1TC, 1u << p); }
static void out(u32 p) { wr((volatile u32 *)GPIO_ENA_W1TS, 1u << p); }

static void mux_gpio(u32 p)
{
    volatile u32 *mux = (volatile u32 *)(0x60009000u + p * 4u);
    u32 v = *mux;
    v = (v & ~(7u << 12)) | (1u << 8) | (2u << 10);
    v = v & ~(1u << 9);
    *mux = v;
}
static void dly(u32 n) { volatile u32 i = 0; while (i < n) { i++; } }

static void spi_byte(u8 b)
{
    int i = 7;
    while (i >= 0) {
        if (((b >> i) & 1u) != 0u) { hi(PIN_SDA); } else { lo(PIN_SDA); }
        dly(2u);
        hi(PIN_SCL);
        dly(2u);
        lo(PIN_SCL);
        i--;
    }
}

static void cmd(u8 c)
{
    lo(PIN_DC);
    lo(PIN_CS);
    spi_byte(c);
    hi(PIN_CS);
}

static void dat(u8 d)
{
    hi(PIN_DC);
    lo(PIN_CS);
    spi_byte(d);
    hi(PIN_CS);
}

static void dat_start(void) { hi(PIN_DC); lo(PIN_CS); }
static void dat_end(void) { hi(PIN_CS); }

static void dat16(u16 v)
{
    spi_byte((u8)(v >> 8));
    spi_byte((u8)(v & 0xFFu));
}

static void window(u32 x, u32 y, u32 w, u32 h)
{
    cmd(0x2Au);
    dat_start();
    dat16((u16)x); dat16((u16)(x + w - 1u));
    dat_end();
    cmd(0x2Bu);
    dat_start();
    dat16((u16)y); dat16((u16)(y + h - 1u));
    dat_end();
    cmd(0x2Cu);
}

static void init_seq(void)
{
    lo(PIN_RST); dly(200000u);
    hi(PIN_RST); dly(1200000u);

    cmd(0x01u); dly(1200000u);           /* SWRESET */
    cmd(0x11u); dly(1200000u);           /* SLPOUT */
    cmd(0x3Au); dat(0x55u);              /* COLMOD 16bpp */
    cmd(0x36u); dat(0x00u);              /* MADCTL */
    cmd(0x21u);                          /* INVON */
    cmd(0x13u);                          /* NORON */
    cmd(0x29u); dly(200000u);            /* DISPON */
}

void s3_lcd_init(void)
{
    mux_gpio(PIN_SDA); mux_gpio(PIN_SCL); mux_gpio(PIN_DC);
    mux_gpio(PIN_CS); mux_gpio(PIN_RST); mux_gpio(PIN_BL);
    out(PIN_SDA); out(PIN_SCL); out(PIN_DC); out(PIN_CS); out(PIN_RST); out(PIN_BL);
    hi(PIN_CS); hi(PIN_DC); hi(PIN_SCL); lo(PIN_SDA); lo(PIN_RST); lo(PIN_BL);
    init_seq();
    hi(PIN_BL);
}

void s3_lcd_fill(u16 color)
{
    window(0u, 0u, LCD_W, LCD_H);
    dat_start();
    {
        u32 n = LCD_W * LCD_H;
        while (n != 0u) { dat16(color); n--; }
    }
    dat_end();
}

void s3_lcd_blit(const u16 *fb, u32 w, u32 h)
{
    u32 i = 0;
    u32 n = w * h;
    if (fb == 0 || w == 0u || h == 0u) { return; }
    if (w > LCD_W) { w = LCD_W; }
    if (h > LCD_H) { h = LCD_H; n = w * h; }
    window(0u, 0u, w, h);
    dat_start();
    while (i < n) { dat16(fb[i]); i++; }
    dat_end();
}
