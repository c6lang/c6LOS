/* c6OS flash/cache 映射接口。
 *
 * 背景：ESP32-S3 上 IROM(0x42000000)/DROM(0x3C000000) 这些"flash 窗口"必须先由
 * **SPI MMU** 把 flash 页映射过去，指令/数据访问才能通。标准 IDF 流程里这件事是
 * 二级 bootloader 干的；我们的镜像是 ROM 直接加载并跳 entry，没人建映射，
 * 所以一旦有代码/数据被链接进 flash 段，就必须先调用这里的函数。
 */
#ifndef C6OS_FLASH_H
#define C6OS_FLASH_H

#include "c6os/types.h"

/* 映射窗口类型 */
#define OS_FLASH_IROM 0u   /* 0x42000000 起，只用于取指 */
#define OS_FLASH_DROM 1u   /* 0x3C000000 起，用于读数据 */

/* 把 flash 物理地址 flash_off 起、bytes 字节，映射到虚拟窗口 vaddr。
 * 要求：flash_off / vaddr / bytes 都按 64KB 页对齐（bytes 会向上取整）。
 * 返回 0 成功；<0 表示参数不合法。
 * 注意：只写 MMU 表，不碰 cache —— 启用 cache 请调用 os_icache_enable()。 */
int os_flash_map(u32 flash_off, u32 vaddr, u32 bytes, u32 kind);

/* 按链接脚本里的 flash 段自动建立映射并打开 I-cache。
 * port 启动流程里调用一次即可（没有 flash 段时什么都不做）。
 * 依赖 link.ld 提供的 _flash_text_start/_flash_text_end/_flash_text_lma。 */
void os_flash_init(void);

/* 打开指令 cache（IROM 取指前必须）。可重复调用。 */
void os_icache_enable(void);

/* 打开数据 cache（读 DROM 前必须）。可重复调用。 */
void os_dcache_enable(void);

/* 当前 D-cache 控制寄存器值（调试用，bit0 = 已使能） */
u32 os_dcache_state(void);

/* 当前 MMU 表项（调试用）：返回 entry_id 对应的原始值 */
u32 os_flash_mmu_entry(u32 vaddr);

/* 当前 I-cache 控制寄存器值（调试用，bit0 = 已使能） */
u32 os_icache_state(void);

/* ---- RTC 看门狗（RWDT）：真板调试用的"挂死自愈"装置 ----------------
 *
 * 闭源 PHY/WiFi 初始化一旦忙等死转，CPU 不会自己复位，整块板子会从 USB 上
 * 消失，只能人工断电。用它来兜底：挂住 N 秒后整片复位重跑（RTC 慢存里的
 * 信标不会被清，所以还能知道上一轮死在哪一步）。
 *
 * secs = 0 表示关闭。feed 会重新计时。这些都是寄存器级的裸操作，
 * 不影响其它构建（只有显式调用才生效）。 */
void os_wdt_start(u32 secs);
void os_wdt_feed(void);
void os_wdt_stop(void);

#endif /* C6OS_FLASH_H */
