#ifndef C6OS_COBS_H
#define C6OS_COBS_H

#include "c6os/types.h"

u32 cobs_encode(const u8 *in, u32 n, u8 *out, u32 cap, u32 delim);
int cobs_decode(const u8 *in, u32 n, u8 *out, u32 cap);

#endif
