/* ESP32-S3 的 flash 窗口（IROM/DROM）映射与 I-cache 使能。
 * 寄存器定义来自 IDF 的 soc/esp32s3：
 *   - MMU 表基址 DR_REG_MMU_TABLE = 0x600C5000，512 个表项，每项管 64KB；
 *   - 表项格式：bit[13:0] = 物理页号，bit14 = 0 表示有效（SOC_MMU_INVALID 才是 bit14），
 *     bit15 = 0 表示目标是 flash（1 表示 PSRAM）；
 *   - 表项下标 = (vaddr & 0x1FFFFFF) >> 16，IROM 从 0 开始、DROM 从 0x1C0 开始；
 *   - I-cache 使能：EXTMEM(0x600C4000) + 0x60 的 bit0。
 */
#include "c6os/types.h"
#include "c6os/flash.h"

#define S3_MMU_TABLE      (*(volatile u32 *)0x600C5000u)
#define S3_MMU_ENTRY_NUM  512u
#define S3_MMU_VADDR_MASK 0x1FFFFFFu
#define S3_MMU_PAGE       0x10000u
#define S3_MMU_VALID      0u        /* 有效位的值（无效是 bit14=1） */
#define S3_EXTMEM_ICACHE_CTRL (*(volatile u32 *)0x600C4060u)
#define S3_EXTMEM_DCACHE_CTRL (*(volatile u32 *)0x600C4000u)

static volatile u32 *s3_mmu_entry(u32 vaddr)
{
    u32 id = (vaddr & S3_MMU_VADDR_MASK) >> 16;

    if (id >= S3_MMU_ENTRY_NUM) {
        return 0;
    }
    return (volatile u32 *)0x600C5000u + id;
}

int os_flash_map(u32 flash_off, u32 vaddr, u32 bytes, u32 kind)
{
    u32 npages;
    u32 i;

    if ((flash_off & (S3_MMU_PAGE - 1u)) != 0u) { return -1; }
    if ((vaddr & (S3_MMU_PAGE - 1u)) != 0u) { return -1; }
    if (bytes == 0u) { return -1; }

    npages = (bytes + S3_MMU_PAGE - 1u) / S3_MMU_PAGE;
    i = 0;
    while (i < npages) {
        volatile u32 *e = s3_mmu_entry(vaddr + i * S3_MMU_PAGE);

        if (e == 0) { return -2; }
        /* 物理页号 | 有效 | 目标=flash(bit15=0) */
        *e = ((flash_off >> 16) + i) & 0x3FFFu;
        i++;
    }
    (void)kind;      /* IROM 和 DROM 在表项格式上一致 */
    return 0;
}

u32 os_flash_mmu_entry(u32 vaddr)
{
    volatile u32 *e = s3_mmu_entry(vaddr & ~(S3_MMU_PAGE - 1u));

    return (e == 0) ? 0u : *e;
}

void os_icache_enable(void)
{
    S3_EXTMEM_ICACHE_CTRL = S3_EXTMEM_ICACHE_CTRL | 1u;   /* ICACHE_ENABLE */
}

/* ROM 里的 cache 操作函数（IDF 的 esp_rom/cache.h 同款）。
 * 关键：ROM 启动阶段可能已经把 0x42xxxxxx 的旧 MMU 映射用进了 I-cache，
 * 我们自己改完 MMU 必须清一次 I-cache，否则取到的是旧内容（表现为
 * IllegalInstruction、PC 落在指令中间）。 */
extern void Cache_Invalidate_ICache_All(void);

/* 链接脚本提供的 flash 段边界（没有 flash 段时三者相等） */
/* 注意：**不能加 weak**。加了 GCC 会把符号值实现成"从该地址读 4 字节"，
 * 而 0x42xxxxxx 是 IROM（只能取指，读数据会 LoadStoreError）。
 * 这几个符号由 link.ld 无条件定义，普通 extern 就够。 */
extern const u8 _flash_text_start[];
extern const u8 _flash_text_end[];
extern const u8 _flash_text_lma[];

void os_flash_init(void)
{
#ifdef S3_FLASH_AUTO_MAP
    /* 默认关闭：我们的 flash 窗口（IROM/DROM）映射 + cache 使能还在调试中
     * （见 docs/wifi-plan.md §15~17：MMU 表项和 I-cache 使能都确认生效，
     * 但在 IROM 里取指仍 IllegalInstruction）。在搞定之前不要自动动它，
     * 以免影响普通（无 flash 段）镜像 —— 那些镜像根本用不到这里。 */
    u32 va;
    u32 size;
    u32 lma;

    va = (u32)_flash_text_start;
    size = (u32)_flash_text_end - va;
    if (size == 0u) {
        return;                       /* 没有 flash 段：普通镜像，什么都不用做 */
    }
    lma = (u32)_flash_text_lma;

    /* 如果平台（比如 IDF 的标准 bootloader）**已经**把这一页映射好了，就别再动它：
     * bootloader 是按 "app 分区偏移 + 镜像内偏移" 映射的，我们只知道镜像内偏移，
     * 重新映射反而会弄错。表项 bit14=1 表示无效（SOC_MMU_INVALID）。 */
#ifndef S3_FLASH_FORCE_MAP
    if ((os_flash_mmu_entry(va & ~(S3_MMU_PAGE - 1u)) & 0x4000u) == 0u) {
        os_icache_enable();               /* 映射已就绪，只需确保 I-cache 打开 */
        return;
    }
#endif
    if (os_flash_map(lma & ~(S3_MMU_PAGE - 1u), va & ~(S3_MMU_PAGE - 1u),
                     size + 0xFFFFu, OS_FLASH_IROM) == 0) {
        Cache_Invalidate_ICache_All();     /* 清掉 ROM 遗留的旧 tag */
        os_icache_enable();
    }
#endif /* S3_FLASH_AUTO_MAP */
}

u32 os_icache_state(void)
{
    return S3_EXTMEM_ICACHE_CTRL;
}

void os_dcache_enable(void)
{
    S3_EXTMEM_DCACHE_CTRL = S3_EXTMEM_DCACHE_CTRL | 1u;   /* DCACHE_ENABLE */
}

u32 os_dcache_state(void)
{
    return S3_EXTMEM_DCACHE_CTRL;
}
