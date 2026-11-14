/* IRAM 填充物：只用于实验"IRAM 内容越过某地址后镜像还能不能启动"。
 * 用 -DS3_PAD_N=... 控制大小（默认 0 = 不占空间）。 */
#include "c6os/types.h"

#ifndef S3_PAD_N
#define S3_PAD_N 0u
#endif

#if S3_PAD_N > 0
__attribute__((section(".text.pad"), used))
static const u8 g_iram_pad[S3_PAD_N] = {1u};
const u8 *s3_iram_pad_ptr(void) { return g_iram_pad; }
#endif
