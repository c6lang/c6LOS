#include "c6os/gfx.h"

void os_gfx_init(OsGfx *g, u16 *fb, u32 w, u32 h)
{
    if (g == 0) {
        return;
    }
    g->fb = fb;
    g->w = w;
    g->h = h;
}

void os_gfx_pixel(OsGfx *g, int x, int y, u16 color)
{
    if (g == 0 || g->fb == 0 || x < 0 || y < 0 ||
        (u32)x >= g->w || (u32)y >= g->h) {
        return;
    }
    g->fb[(u32)y * g->w + (u32)x] = color;
}

u16 os_gfx_get(const OsGfx *g, int x, int y)
{
    if (g == 0 || g->fb == 0 || x < 0 || y < 0 ||
        (u32)x >= g->w || (u32)y >= g->h) {
        return 0;
    }
    return g->fb[(u32)y * g->w + (u32)x];
}

void os_gfx_fill_rect(OsGfx *g, int x, int y, int w, int h, u16 color)
{
    int xe = x + w;
    int ye = y + h;
    int yy = 0;

    if (g == 0 || g->fb == 0) {
        return;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (xe > (int)g->w) {
        xe = (int)g->w;
    }
    if (ye > (int)g->h) {
        ye = (int)g->h;
    }
    yy = y;
    while (yy < ye) {
        int xx = x;
        u16 *row = g->fb + (u32)yy * g->w;

        while (xx < xe) {
            row[(u32)xx] = color;
            xx++;
        }
        yy++;
    }
}

void os_gfx_blit(OsGfx *g, int x, int y, const u16 *src, int sw, int sh)
{
    int yy = 0;

    if (g == 0 || g->fb == 0 || src == 0) {
        return;
    }
    while (yy < sh) {
        int xx = 0;

        while (xx < sw) {
            os_gfx_pixel(g, x + xx, y + yy, src[(u32)yy * (u32)sw + (u32)xx]);
            xx++;
        }
        yy++;
    }
}

static const u8 g_digits[10][8] = {
    {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00},
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00},
    {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00},
    {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00},
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00},
    {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00},
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00},
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00},
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00},
};

static const u8 g_colon[8] = {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00};

void os_gfx_char(OsGfx *g, int x, int y, char c, u16 color, int scale)
{
    const u8 *glyph = 0;
    int row = 0;

    if (g == 0 || scale <= 0) {
        return;
    }
    if (c >= '0' && c <= '9') {
        glyph = g_digits[(int)(c - '0')];
    } else if (c == ':') {
        glyph = g_colon;
    } else {
        return;
    }
    while (row < 8) {
        u8 bits = glyph[row];
        int col = 0;

        while (col < 8) {
            if ((bits & (0x80u >> col)) != 0u) {
                os_gfx_fill_rect(g, x + col * scale, y + row * scale,
                                 scale, scale, color);
            }
            col++;
        }
        row++;
    }
}

void os_gfx_text(OsGfx *g, int x, int y, const char *s, u16 color, int scale)
{
    int cx = x;

    if (s == 0) {
        return;
    }
    while (*s != '\0') {
        os_gfx_char(g, cx, y, *s, color, scale);
        cx += 8 * scale;
        s++;
    }
}
