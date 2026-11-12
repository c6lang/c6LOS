#include "c6os/panel.h"

static const OsPanel g_panels[] = {
    {"st7789", 240u, 280u, 0x8552u},
    {"st77916", 360u, 360u, 0x7791u},
};

int os_panel_pick(const u8 rddid[3], OsPanel *out)
{
    u16 id = 0;
    u32 i = 0;

    if (out == 0 || rddid == 0) {
        return -1;
    }
    id = ((u16)rddid[1] << 8) | (u16)rddid[2];
    while (i < (u32)(sizeof(g_panels) / sizeof(g_panels[0]))) {
        if (g_panels[i].id == id) {
            *out = g_panels[i];
            return 0;
        }
        i++;
    }
    /* 兜底：1.85 寸板默认 ST7789 240x280。 */
    *out = g_panels[0];
    return 1;
}
