#ifndef C6OS_GFX_H
#define C6OS_GFX_H

#include "c6os/types.h"

typedef struct {
    u16 *fb;
    u32 w;
    u32 h;
} OsGfx;

void os_gfx_init(OsGfx *g, u16 *fb, u32 w, u32 h);
void os_gfx_pixel(OsGfx *g, int x, int y, u16 color);
u16 os_gfx_get(const OsGfx *g, int x, int y);
void os_gfx_fill_rect(OsGfx *g, int x, int y, int w, int h, u16 color);
void os_gfx_blit(OsGfx *g, int x, int y, const u16 *src, int sw, int sh);
void os_gfx_char(OsGfx *g, int x, int y, char c, u16 color, int scale);
void os_gfx_text(OsGfx *g, int x, int y, const char *s, u16 color, int scale);

#endif
