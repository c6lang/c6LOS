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

/* 链接脚本提供的 flash 段边界（没有 flash 段时三者相等） */
extern const u8 _flash_text_start[] __attribute__((weak));
extern const u8 _flash_text_end[] __attribute__((weak));
extern const u8 _flash_text_lma[] __attribute__((weak));

void os_flash_init(void)
{
    u32 va;
    u32 size;
    u32 lma;

    if (_flash_text_start == 0 || _flash_text_end == 0 || _flash_text_lma == 0) {
        return;
    }
    va = (u32)_flash_text_start;
    size = (u32)_flash_text_end - va;
    if (size == 0u) {
        return;                       /* 没有 flash 段：普通镜像，什么都不用做 */
    }
    lma = (u32)_flash_text_lma;
    if (os_flash_map(lma & ~(S3_MMU_PAGE - 1u), va & ~(S3_MMU_PAGE - 1u),
                     size + 0xFFFFu, OS_FLASH_IROM) == 0) {
        os_icache_enable();
    }
}

u32 os_icache_state(void)
{
    return S3_EXTMEM_ICACHE_CTRL;
}
