/* Waveshare ESP32-S3-LCD-1.85 QSPI 探针（bit-bang，单线读 ID）。
 * PCLK=40 CS=21 D0=46 D1=45 D2=42 D3=41。 */
#include "c6os/types.h"
#include "c6os/port.h"

#define GPIO_BASE 0x60004000u
#define GPIO_OUT_W1TS (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC (GPIO_BASE + 0x0Cu)
#define GPIO_OUT1_W1TS (GPIO_BASE + 0x14u)
#define GPIO_OUT1_W1TC (GPIO_BASE + 0x18u)
#define GPIO_ENA_W1TS (GPIO_BASE + 0x24u)
#define GPIO_ENA_W1TC (GPIO_BASE + 0x28u)
#define GPIO_ENA1_W1TS (GPIO_BASE + 0x2Cu)
#define GPIO_ENA1_W1TC (GPIO_BASE + 0x30u)
#define GPIO_IN1 (GPIO_BASE + 0x40u)

#define PIN_CS 21u
#define PIN_CLK 40u
#define PIN_D0 46u
#define PIN_D1 45u
#define PIN_SCL 10u
#define PIN_SDA 11u

extern void port_delay_ms(u32 ms);

static void wr(volatile u32 *r, u32 v) { *r = v; }
static void rd_delay(void) { __asm__ __volatile__("nop; nop; nop; nop"); }

static void pin_hi(u32 p)
{
    if (p < 32u) { wr((volatile u32 *)GPIO_OUT_W1TS, 1u << p); }
    else { wr((volatile u32 *)GPIO_OUT1_W1TS, 1u << (p - 32u)); }
}

static void pin_lo(u32 p)
{
    if (p < 32u) { wr((volatile u32 *)GPIO_OUT_W1TC, 1u << p); }
    else { wr((volatile u32 *)GPIO_OUT1_W1TC, 1u << (p - 32u)); }
}

static void pin_out(u32 p)
{
    if (p < 32u) { wr((volatile u32 *)GPIO_ENA_W1TS, 1u << p); }
    else { wr((volatile u32 *)GPIO_ENA1_W1TS, 1u << (p - 32u)); }
}

static void pin_pullup(u32 p)
{
    volatile u32 *mux = (volatile u32 *)(0x60009000u + p * 4u);
    u32 v = *mux;
    v = (v | (1u << 8)) & ~(1u << 9);
    *mux = v;
}

static void pin_in(u32 p)
{
    if (p < 32u) { wr((volatile u32 *)GPIO_ENA_W1TC, 1u << p); }
    else { wr((volatile u32 *)GPIO_ENA1_W1TC, 1u << (p - 32u)); }
}

static void tx_bit(u32 b)
{
    if (b != 0u) { pin_hi(PIN_D0); } else { pin_lo(PIN_D0); }
    rd_delay();
    pin_hi(PIN_CLK);
    rd_delay();
    pin_lo(PIN_CLK);
}

static void tx_byte(u8 b)
{
    int i = 7;

    while (i >= 0) {
        tx_bit((b >> i) & 1u);
        i--;
    }
}

static u8 rx_byte(void)
{
    u8 v = 0;
    int i = 7;

    while (i >= 0) {
        pin_hi(PIN_CLK);
        rd_delay();
        v = (u8)((v << 1) | (u8)((*(volatile u32 *)GPIO_IN1 >> (PIN_D1 - 32u)) & 1u));
        rd_delay();
        pin_lo(PIN_CLK);
        i--;
    }
    return v;
}

static void i2c_delay(void)
{
    volatile u32 i = 0;
    while (i < 80u) { i++; }
}

static void scl(u32 v)
{
    if (v != 0u) { pin_hi(PIN_SCL); } else { pin_lo(PIN_SCL); }
}

static void sda_out(u32 v)
{
    if (v != 0u) { pin_in(PIN_SDA); } else { pin_lo(PIN_SDA); }
}

static u32 sda_in(void)
{
    return (*(volatile u32 *)(GPIO_BASE + 0x3Cu) >> PIN_SDA) & 1u;
}

static void i2c_start(void)
{
    pin_out(PIN_SDA);
    pin_out(PIN_SCL);
    sda_out(1u); scl(1u); i2c_delay();
    sda_out(0u); i2c_delay(); scl(0u);
}

static void i2c_stop(void)
{
    sda_out(0u); scl(0u); i2c_delay();
    scl(1u); i2c_delay(); sda_out(1u); i2c_delay();
}

static int i2c_write(u8 b)
{
    int i = 7;
    u32 ack = 0;

    while (i >= 0) {
        sda_out((b >> i) & 1u);
        i2c_delay();
        scl(1u); i2c_delay(); scl(0u);
        i--;
    }
    pin_in(PIN_SDA);
    i2c_delay();
    scl(1u); i2c_delay();
    ack = sda_in();
    scl(0u);
    return ack == 0u;
}

static void tca_write(u8 reg, u8 val)
{
    i2c_start();
    (void)i2c_write(0x40u);   /* addr 0x20 << 1 */
    (void)i2c_write(reg);
    (void)i2c_write(val);
    i2c_stop();
}

static void delay_loop(u32 n)
{
    volatile u32 i = 0;
    while (i < n) { i++; }
}

u32 s3_gpio_loop(u32 p);

u32 s3_gpio_loop_sel(u32 p, u32 sel)
{
    volatile u32 *mux = (volatile u32 *)(0x60009000u + p * 4u);
    u32 v = *mux;

    v = (v & ~(7u << 12)) | ((sel & 7u) << 12);
    v = (v | (1u << 9)) & ~(1u << 7);
    *mux = v;
    return s3_gpio_loop(p);
}

u32 s3_gpio_loop(u32 p)
{
    u32 hi = 0;
    u32 lo = 1;

    pin_out(p);
    pin_hi(p);
    rd_delay();
    if (p < 32u) {
        hi = (*(volatile u32 *)GPIO_BASE >> p) & 1u;
    } else {
        hi = (*(volatile u32 *)GPIO_IN1 >> (p - 32u)) & 1u;
    }
    pin_lo(p);
    rd_delay();
    if (p < 32u) {
        lo = (*(volatile u32 *)GPIO_BASE >> p) & 1u;
    } else {
        lo = (*(volatile u32 *)GPIO_IN1 >> (p - 32u)) & 1u;
    }
    return (hi << 1) | lo;
}

void s3_backlight_test(void)
{
    pin_out(42u);
    pin_out(5u);
    pin_lo(5u);

    while (1) {
        port_puts("BL42 on\n");
        pin_hi(42u);
        delay_loop(30000000u);
        pin_lo(42u);
        port_puts("BL5 on\n");
        pin_hi(5u);
        delay_loop(30000000u);
        pin_lo(5u);
        port_puts("BL off\n");
    }
}

u32 s3_i2c_probe(u8 addr)
{
    int ack = 0;

    pin_out(PIN_SCL);
    pin_out(PIN_SDA);
    pin_pullup(PIN_SCL);
    pin_pullup(PIN_SDA);
    i2c_start();
    ack = i2c_write((u8)(addr << 1));
    i2c_stop();
    return ack != 0 ? 1u : 0u;
}

static void panel_reset(void)
{
    /* 1.85b: LCD_RST = GPIO3，低有效 */
    pin_out(3u);
    pin_hi(3u);
    delay_loop(500000u);
    pin_lo(3u);
    delay_loop(2000000u);
    pin_hi(3u);
    delay_loop(2000000u);
}

#define PIN_LCD_CS 41u
#define PIN_LCD_DC 40u
#define PIN_LCD_RST 45u
#define PIN_LCD_CLK 21u
#define PIN_LCD_SDA 47u

static void lcd_tx_bit(u32 b)
{
    if (b != 0u) { pin_hi(PIN_LCD_SDA); } else { pin_lo(PIN_LCD_SDA); }
    rd_delay();
    pin_hi(PIN_LCD_CLK);
    rd_delay();
    pin_lo(PIN_LCD_CLK);
}

static void lcd_tx_byte(u8 b)
{
    int i = 7;
    while (i >= 0) { lcd_tx_bit((b >> i) & 1u); i--; }
}

static u8 lcd_rx_byte(void)
{
    u8 v = 0;
    int i = 7;

    while (i >= 0) {
        pin_hi(PIN_LCD_CLK);
        rd_delay();
        v = (u8)((v << 1) | (u8)((*(volatile u32 *)GPIO_IN1 >> (PIN_LCD_SDA - 32u)) & 1u));
        rd_delay();
        pin_lo(PIN_LCD_CLK);
        i--;
    }
    return v;
}

void s3_st7789_probe(u8 out[4])
{
    int i = 0;

    pin_out(PIN_LCD_CS);
    pin_out(PIN_LCD_DC);
    pin_out(PIN_LCD_RST);
    pin_out(PIN_LCD_CLK);
    pin_out(PIN_LCD_SDA);
    pin_hi(PIN_LCD_CS);
    pin_lo(PIN_LCD_CLK);
    pin_hi(PIN_LCD_RST);
    delay_loop(200000u);

    pin_lo(PIN_LCD_CS);
    pin_lo(PIN_LCD_DC);
    lcd_tx_byte(0x04u);
    pin_hi(PIN_LCD_DC);
    pin_in(PIN_LCD_SDA);
    pin_pullup(PIN_LCD_SDA);
    while (i < 4) { out[i] = lcd_rx_byte(); i++; }
    pin_hi(PIN_LCD_CS);
}

void s3_panel_probe(u8 out[3])
{
    int i = 0;

    panel_reset();
    pin_out(PIN_CS);
    pin_out(PIN_CLK);
    pin_hi(PIN_CS);
    pin_lo(PIN_CLK);
    pin_out(PIN_D0);
    pin_pullup(PIN_D0);
    pin_out(45u);
    pin_out(42u);
    pin_out(41u);
    pin_hi(45u);
    pin_hi(42u);
    pin_hi(41u);

    pin_lo(PIN_CS);
    tx_byte(0x0Bu);
    tx_byte(0x00u);
    tx_byte(0x04u);
    tx_byte(0x00u);
    tx_byte(0x00u);

    pin_in(PIN_D1);
    pin_pullup(PIN_D1);
    while (i < 3) {
        out[i] = rx_byte();
        i++;
    }
    pin_hi(PIN_CS);
}

/* ---- ST77916 360x360 QSPI 极简驱动（bit-bang 4-line） ---- */
#include "c6os/st77916_table.h"

#define LCD_W 360
#define LCD_H 360

static void qnib(u8 n)
{
    if (n & 1u) { pin_hi(46u); } else { pin_lo(46u); }
    if (n & 2u) { pin_hi(45u); } else { pin_lo(45u); }
    if (n & 4u) { pin_hi(42u); } else { pin_lo(42u); }
    if (n & 8u) { pin_hi(41u); } else { pin_lo(41u); }
    rd_delay();
    pin_hi(40u);
    rd_delay();
    pin_lo(40u);
}

static void qcmd(u8 op, u8 cmd)
{
    u32 w = ((u32)op << 24) | ((u32)cmd << 8);
    int sh = 28;

    while (sh >= 0) {
        qnib((u8)((w >> sh) & 0xFu));
        sh -= 4;
    }
}

static void qdata(const u8 *p, u32 n)
{
    u32 i = 0;

    while (i < n) {
        qnib((u8)(p[i] >> 4));
        qnib((u8)(p[i] & 0xFu));
        i++;
    }
}

static void qcmd_data(u8 cmd, const u8 *p, u32 n)
{
    pin_lo(21u);
    qcmd(0x02u, cmd);
    if (n != 0u) {
        qdata(p, n);
    }
    pin_hi(21u);
}

static void qcolor(u16 color, u32 n)
{
    u32 i = 0;
    u8 hi = (u8)(color >> 8);
    u8 lo = (u8)(color & 0xFFu);

    pin_lo(21u);
    qcmd(0x32u, 0x2Cu);
    while (i < n) {
        qnib((u8)(hi >> 4));
        qnib((u8)(hi & 0xFu));
        qnib((u8)(lo >> 4));
        qnib((u8)(lo & 0xFu));
        i++;
    }
    pin_hi(21u);
}

static void lcd_pins_init(void)
{
    pin_out(21u); pin_out(40u); pin_out(46u); pin_out(45u);
    pin_out(42u); pin_out(41u); pin_out(3u); pin_out(5u);
    pin_hi(21u);
    pin_lo(40u);
    pin_hi(3u);
    pin_hi(5u);
}

void s3_lcd_init(void)
{
    const St77916Cmd *c = g_st77916_v1;
    u8 col[4];
    u8 row[4];
    u32 n = 0;

    lcd_pins_init();
    pin_lo(3u);
    delay_loop(2000000u);
    pin_hi(3u);
    delay_loop(3000000u);

    qcmd_data(0x01u, 0, 0);
    delay_loop(2000000u);
    while (c->cmd != 0x00u) {
        qcmd_data(c->cmd, c->data, c->len);
        if (c->delay != 0u) {
            delay_loop((u32)c->delay * 40000u);
        }
        c++;
        n++;
    }
    qcmd_data(0x21u, 0, 0);
    qcmd_data(0x11u, 0, 0);
    delay_loop(4000000u);
    qcmd_data(0x29u, 0, 0);

    col[0] = 0; col[1] = 0;
    col[2] = (u8)((LCD_W - 1) >> 8); col[3] = (u8)((LCD_W - 1) & 0xFF);
    row[0] = 0; row[1] = 0;
    row[2] = (u8)((LCD_H - 1) >> 8); row[3] = (u8)((LCD_H - 1) & 0xFF);
    qcmd_data(0x2Au, col, 4);
    qcmd_data(0x2Bu, row, 4);
    (void)n;
}

void s3_lcd_blit(const u16 *fb, u32 w, u32 h)
{
    u8 col[4];
    u8 row[4];
    u32 i = 0;
    u32 n = w * h;

    if (fb == 0 || w == 0u || h == 0u) {
        return;
    }
    col[0] = 0; col[1] = 0;
    col[2] = (u8)((w - 1u) >> 8); col[3] = (u8)((w - 1u) & 0xFFu);
    row[0] = 0; row[1] = 0;
    row[2] = (u8)((h - 1u) >> 8); row[3] = (u8)((h - 1u) & 0xFFu);
    qcmd_data(0x2Au, col, 4);
    qcmd_data(0x2Bu, row, 4);

    pin_lo(21u);
    qcmd(0x32u, 0x2Cu);
    while (i < n) {
        u8 hi = (u8)(fb[i] >> 8);
        u8 lo = (u8)(fb[i] & 0xFFu);
        qnib((u8)(hi >> 4));
        qnib((u8)(hi & 0xFu));
        qnib((u8)(lo >> 4));
        qnib((u8)(lo & 0xFu));
        i++;
    }
    pin_hi(21u);
}

void s3_lcd_fill(u16 color)
{
    qcolor(color, LCD_W * LCD_H);
}
