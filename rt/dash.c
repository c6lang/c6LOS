#include "c6os/dash.h"

static void out_u32(OsGfx *g, int x, int y, u32 v, u16 color, int scale)
{
    char b[12];
    int n = 0;
    int i = 0;

    if (v == 0u) {
        b[n] = '0';
        n++;
    }
    while (v > 0u && n < 12) {
        b[n] = (char)('0' + (v % 10u));
        n++;
        v = v / 10u;
    }
    while (i < n) {
        int j = n - 1 - i;
        os_gfx_char(g, x + i * 8 * scale, y, b[j], color, scale);
        i++;
    }
}

void os_dash_render(OsGfx *g, u32 tick, u32 tasks, u32 heap_used)
{
    if (g == 0) {
        return;
    }
    os_gfx_fill_rect(g, 0, 0, (int)g->w, (int)g->h, 0x0000u);
    out_u32(g, 4, 2, tick, 0xFFFFu, 2);
    out_u32(g, 4, 22, tasks, 0x07E0u, 2);
    out_u32(g, 4, 42, heap_used, 0xF800u, 2);
}

void os_dash_task_row(OsGfx *g, int y, u32 id, u32 prio, char state,
                      const char *name)
{
    out_u32(g, 4, y, id, 0xFFFFu, 1);
    out_u32(g, 28, y, prio, 0x07E0u, 1);
    os_gfx_char(g, 52, y, state, 0xF800u, 1);
    os_gfx_text(g, 68, y, name, 0xFFFFu, 1);
}

void os_dash_log(OsGfx *g, int y, const char *s)
{
    os_gfx_text(g, 4, y, s, 0x8410u, 1);
}
