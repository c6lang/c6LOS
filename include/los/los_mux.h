#ifndef _LOS_MUX_H
#define _LOS_MUX_H

#include "los/los_typedef.h"

#define LOS_ERRNO_MUX_NO_MEMORY 0x02000401u
#define LOS_ERRNO_MUX_INVALID 0x02000402u
#define LOS_ERRNO_MUX_TIMEOUT 0x02000403u

UINT32 LOS_MuxCreate(UINT32 *muxId);
UINT32 LOS_MuxDelete(UINT32 muxId);
UINT32 LOS_MuxPend(UINT32 muxId, UINT32 timeout);
UINT32 LOS_MuxPost(UINT32 muxId);

#endif
