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
