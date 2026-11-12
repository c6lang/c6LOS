/* Waveshare ESP32-S3-LCD-1.85 QSPI 探针（bit-bang，单线读 ID）。
 * PCLK=40 CS=21 D0=46 D1=45 D2=42 D3=41。 */
#include "c6os/types.h"

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
        v = (u8)((v << 1) | (u8)((*(volatile u32 *)GPIO_IN1 >> (PIN_D0 - 32u)) & 1u));
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

static void panel_reset(void)
{
    pin_out(PIN_SCL);
    pin_out(PIN_SDA);
    pin_pullup(PIN_SCL);
    pin_pullup(PIN_SDA);
    tca_write(0x03u, 0xFBu);   /* EXIO2 输出 */
    tca_write(0x01u, 0x00u);
    port_delay_ms(300u);
    tca_write(0x01u, 0x04u);
    port_delay_ms(300u);
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

    pin_in(PIN_D0);
    while (i < 3) {
        out[i] = rx_byte();
        i++;
    }
    pin_hi(PIN_CS);
}
