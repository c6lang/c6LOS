#include "c6os/cobs.h"

u32 cobs_encode(const u8 *in, u32 n, u8 *out, u32 cap, u32 delim)
{
    u32 w = 1;
    u32 code_pos = 0;
    u32 code = 1;
    u32 i = 0;

    if (in == 0 || out == 0 || cap < 2u) {
        return 0;
    }
    while (i < n) {
        if (w + 1u >= cap) {
            return 0;
        }
        if (in[i] == 0u) {
            out[code_pos] = (u8)code;
            code = 1;
            code_pos = w;
            w++;
            i++;
        } else {
            out[w] = in[i];
            w++;
            code++;
            i++;
            if (code == 0xFFu) {
                out[code_pos] = (u8)code;
                code = 1;
                code_pos = w;
                w++;
            }
        }
    }
    out[code_pos] = (u8)code;
    if (delim != 0u) {
        if (w >= cap) {
            return 0;
        }
        out[w] = 0;
        w++;
    }
    return w;
}

int cobs_decode(const u8 *in, u32 n, u8 *out, u32 cap)
{
    u32 r = 0;
    u32 w = 0;

    if (in == 0 || out == 0) {
        return -1;
    }
    while (r < n) {
        u8 code = in[r];
        u32 i = 1;

        r++;
        if (code == 0u) {
            break;
        }
        while (i < (u32)code) {
            if (r >= n || w >= cap) {
                return -1;
            }
            out[w] = in[r];
            w++;
            r++;
            i++;
        }
        if (code < 0xFFu && r < n) {
            if (w >= cap) {
                return -1;
            }
            out[w] = 0;
            w++;
        }
    }
    return (int)w;
}
