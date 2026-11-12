#include "c6os/gfx.h"

void os_gfx_init_n(OsGfx *g, u16 *fb, u32 w, u32 h, u32 cells)
{
    if (g == 0) {
        return;
    }
    g->fb = fb;
    g->w = w;
    g->h = h;
    g->cells = cells;
}

void os_gfx_init(OsGfx *g, u16 *fb, u32 w, u32 h)
{
    u32 cells = 0;

    if (w != 0u && h != 0u && w <= 0xFFFFFFFFu / h) {
        cells = w * h;
    }
    os_gfx_init_n(g, fb, w, h, cells);
}

void os_gfx_pixel(OsGfx *g, int x, int y, u16 color)
{
    u64 idx = 0;

    if (g == 0 || g->fb == 0 || g->w == 0u || g->h == 0u || g->cells == 0u ||
        x < 0 || y < 0 || (u32)x >= g->w || (u32)y >= g->h) {
        return;
    }
    idx = (u64)(u32)y * (u64)g->w + (u64)(u32)x;
    if (idx >= (u64)g->cells) {
        return;
    }
    g->fb[(u32)idx] = color;
}

u16 os_gfx_get(const OsGfx *g, int x, int y)
{
    u64 idx = 0;

    if (g == 0 || g->fb == 0 || g->w == 0u || g->h == 0u || g->cells == 0u ||
        x < 0 || y < 0 || (u32)x >= g->w || (u32)y >= g->h) {
        return 0;
    }
    idx = (u64)(u32)y * (u64)g->w + (u64)(u32)x;
    if (idx >= (u64)g->cells) {
        return 0;
    }
    return g->fb[(u32)idx];
}

void os_gfx_fill_rect(OsGfx *g, int x, int y, int w, int h, u16 color)
{
    int yy = 0;
    int xe = 0;
    int ye = 0;

    if (g == 0 || g->fb == 0 || g->w == 0u || g->h == 0u ||
        g->w > 0x7FFFFFFFu || g->h > 0x7FFFFFFFu) {
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w = w + x;
        x = 0;
    }
    if (y < 0) {
        h = h + y;
        y = 0;
    }
    if (w <= 0 || h <= 0 || x >= (int)g->w || y >= (int)g->h) {
        return;
    }
    xe = x;
    ye = y;
    if (w > (int)g->w - x) {
        w = (int)g->w - x;
    }
    if (h > (int)g->h - y) {
        h = (int)g->h - y;
    }
    xe = x + w;
    ye = y + h;
    yy = y;
    while (yy < ye) {
        int xx = x;
        while (xx < xe) {
            u64 idx = (u64)(u32)yy * (u64)g->w + (u64)(u32)xx;
            if (idx >= (u64)g->cells) {
                break;
            }
            g->fb[(u32)idx] = color;
            xx++;
        }
        yy++;
    }
}

void os_gfx_blit(OsGfx *g, int x, int y, const u16 *src, int sw, int sh)
{
    int yy = 0;

    if (g == 0 || g->fb == 0 || src == 0 || sw <= 0 || sh <= 0 ||
        sw > 0x7FFFFFFF / sh) {
        return;
    }
    while (yy < sh) {
        int xx = 0;
        u64 row = (u64)(u32)yy * (u64)(u32)sw;

        while (xx < sw) {
            u64 idx = row + (u64)(u32)xx;
            if (idx > 0xFFFFFFFFu) {
                break;
            }
            os_gfx_pixel(g, x + xx, y + yy, src[(u32)idx]);
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
static const u8 g_dash[8] = {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00};
static const u8 g_dot[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00};
static const u8 g_space[8] = {0, 0, 0, 0, 0, 0, 0, 0};

static const u8 g_alpha[26][8] = {
    {0x18, 0x3C, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00},
    {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00},
    {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00},
    {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00},
    {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x7E, 0x00},
    {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x60, 0x00},
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00},
    {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00},
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00},
    {0x66, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x66, 0x00},
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00},
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00},
    {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00},
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},
    {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00},
    {0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00},
    {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00},
    {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00},
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00},
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00},
    {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00},
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00},
};

static const u8 *glyph_for(char c)
{
    if (c >= '0' && c <= '9') {
        return g_digits[(int)(c - '0')];
    }
    if (c >= 'A' && c <= 'Z') {
        return g_alpha[(int)(c - 'A')];
    }
    if (c == ':') {
        return g_colon;
    }
    if (c == '-') {
        return g_dash;
    }
    if (c == '.') {
        return g_dot;
    }
    if (c == ' ') {
        return g_space;
    }
    return 0;
}

void os_gfx_char(OsGfx *g, int x, int y, char c, u16 color, int scale)
{
    const u8 *glyph = glyph_for(c);
    int row = 0;

    if (g == 0 || scale <= 0 || scale > 1024 || glyph == 0) {
        return;
    }
    while (row < 8) {
        u8 bits = glyph[row];
        int col = 0;

        while (col < 8) {
            if ((bits & (0x80u >> col)) != 0u) {
                i64 fx = (i64)x + (i64)col * (i64)scale;
                i64 fy = (i64)y + (i64)row * (i64)scale;
                if (fx >= -2147483648LL && fx <= 2147483647LL &&
                    fy >= -2147483648LL && fy <= 2147483647LL) {
                    os_gfx_fill_rect(g, (int)fx, (int)fy, scale, scale, color);
                }
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
        if (cx > 0x7FFFFF00) {
            break;
        }
        cx += 8 * scale;
        s++;
    }
}

void os_gfx_char16(OsGfx *g, int x, int y, char c, u16 color)
{
    const u8 *glyph = glyph_for(c);
    int row = 0;

    if (g == 0 || glyph == 0 || x > 0x7FFFFF00 || y > 0x7FFFFF00) {
        return;
    }
    while (row < 8) {
        u8 bits = glyph[row];
        int col = 0;

        while (col < 8) {
            if ((bits & (0x80u >> col)) != 0u) {
                os_gfx_pixel(g, x + col, y + row * 2, color);
                os_gfx_pixel(g, x + col, y + row * 2 + 1, color);
            }
            col++;
        }
        row++;
    }
}

void os_gfx_text16(OsGfx *g, int x, int y, const char *s, u16 color)
{
    int cx = x;

    if (s == 0) {
        return;
    }
    while (*s != '\0') {
        os_gfx_char16(g, cx, y, *s, color);
        cx += 8;
        s++;
    }
}
