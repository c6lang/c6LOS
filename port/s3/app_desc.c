/* IDF 应用描述符：标准 app 镜像的**第一个段（DROM）开头**必须有这个 256 字节结构，
 * 二级 bootloader 用它校验/记录应用信息。没有它，IDF bootloader 会拒绝引导
 * （表现为 bootloader_reset()，见 docs/wifi-plan.md §17/§26）。
 */
#include "c6os/types.h"

typedef struct {
    u32 magic_word;
    u32 secure_version;
    u32 reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    u8 app_elf_sha256[32];
    u16 min_efuse_blk_rev_full;
    u16 max_efuse_blk_rev_full;
    u8 mmu_page_size;
    u8 reserv3[3];
    u32 reserv2[18];
} AppDesc;

__attribute__((section(".flash.rodata.desc"), used))
const AppDesc g_app_desc = {
    0xABCD5432u,        /* ESP_APP_DESC_MAGIC_WORD */
    0u,
    {0u, 0u},
    "c6os-0.1",         /* version */
    "c6os-s3",          /* project_name */
    "12:00:00",
    "Jan 01 2026",
    "c6os",
    {0},
    0u, 0u,
    16u,                /* mmu_page_size: log2(64KB) = 16 */
    {0u, 0u, 0u},
    {0u},
};
