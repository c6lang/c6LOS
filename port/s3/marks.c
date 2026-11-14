/* 启动/调试进度标记：放在 .data（不会被清 .bss 抹掉），复位后内容保留。
 * 用 `nm build/<b>/c6os.elf | grep g_boot_marks` 拿到地址，
 * 再用 `esptool read-mem <addr>` 读。
 *
 * 为什么不用 RTC 慢速内存：实测 0x50000000 附近会被 ROM/bootloader 覆写
 * （同一块板上读到过 0xA0/0xA1，也读到过 0xb0dc51a9 这种乱值）。
 */
#include "c6os/types.h"

/* 故意用非零初值 → 落在 .data（清 .bss 不会抹掉），这样连"进 _start 之前"
 * 的阶段也能留下痕迹。 */
volatile u32 g_boot_marks[8] = {0xEEu, 0xEEu, 0xEEu, 0xEEu, 0xEEu, 0xEEu, 0xEEu, 0xEEu};

/* ---- 掉电域"信标"：RTC_CNTL_STORE4..7 -------------------------------
 * 0x600080C0/C4/C8/CC —— 属于 RTC 域，**CPU 复位/看门狗复位都不会清**，
 * 而 esptool 又能直接 read-mem 读出来。串口不可靠时，这是最靠谱的诊断通道：
 * 程序把执行进度写进这 4 个字，跑完后用
 *   esptool read-mem 0x600080C0
 * 就能知道它走到哪一步、卡在哪。
 */
#define S3_RTC_STORE4 (*(volatile u32 *)0x600080C0u)

void s3_beacon(u32 slot, u32 val)
{
    volatile u32 *p = &S3_RTC_STORE4;

    if (slot > 3u) { slot = 3u; }
    p[slot] = val;
}
