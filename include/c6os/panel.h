#ifndef C6OS_PANEL_H
#define C6OS_PANEL_H

#include "c6os/types.h"

typedef struct {
    const char *name;
    u16 width;
    u16 height;
    u16 id;
} OsPanel;

int os_panel_pick(const u8 rddid[3], OsPanel *out);

#endif
