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
