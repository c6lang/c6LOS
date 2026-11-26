#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"
#include "c6os/linux_dev.h"
#include "c6os/linux_vma.h"
#if C6OS_NET
#include "los/net.h"
#endif

/* 只读 initramfs（标准 newc cpio，和 Linux 的 initramfs 同格式）+ fd 表 +
 * 文件类系统调用。
 *
 * 为什么是 cpio/newc 而不是自造格式：内核侧解析器只有 60 行，而工具链那边
 * 任何 Linux 都能生成（`find | cpio -o -H newc`），L4 的 ld.so/libc.so 直接
 * 从宿主拷进来即可。目录语义只做只读 + 扁平表，L5 的写路径留给 tmpfs。 */

/* 全量 Alpine minirootfs 有 518 个 cpio 成员（目录 + 文件 + 符号链接）；
 * Chromium rootfs 预计 >10k 个节点。节点表从 1024 起步、按需翻倍，
 * 不再有 FS_MAX_FILES 编译期硬上限（旧实现超过 1024 会静默丢弃）。 */
#define FS_INIT_NODES 1024u
#define FS_NAME       96u
/* 符号链接最多跟几层。Alpine 的 /etc/ssl1.1/certs -> /etc/ssl/certs 与
 * /etc/ssl/certs 之间互相指向，必须有上限，否则解析会转圈。 */
#define FS_LINK_MAX  8u

typedef struct {
    char name[FS_NAME];
    u64  off;                              /* 数据在 initramfs 物理映像内的偏移 */
    u64  size;
    u32  mode;
    u32  writable;                         /* 1 = ram_data 可写（memfd/ramfs 节点） */
    u32  hidden;                           /* 1 = 不参与路径查找（memfd） */
    u32  seals;                            /* memfd 的 F_SEAL_* 集合 */
    u64  cap;                              /* ram_data 的容量（按页/8 对齐） */
    u64  ram_pa;                           /* 页对齐物理基址（ram_data 的 DMAP 源） */
    u32  ram_pages;
    u32  ram_pad;
    u8  *ram_data;                         /* M57：运行时节点的数据（非 0 时优先于 off） */
} LxNode;

static LxNode *g_node;
static u32     g_node_cap;
static u32     g_nodes;

/* 保证节点表至少有 need 个槽；保留已有内容，失败返回 -1。 */
static int node_reserve_(u32 need)
{
    u32 ncap;
    u32 i;
    LxNode *p;

    if (need <= g_node_cap) {
        return 0;
    }
    ncap = (g_node_cap == 0u) ? FS_INIT_NODES : g_node_cap;
    while (ncap < need) {
        if (ncap > 0x40000000u) {
            return -1;
        }
        ncap <<= 1;
    }
    p = (LxNode *)port_kcalloc((u64)ncap, sizeof(LxNode));
    if (p == (LxNode *)0) {
        return -1;
    }
    for (i = 0u; i < g_nodes; i++) {
        p[i] = g_node[i];
    }
    if (g_node != (LxNode *)0) {
        port_kfree(g_node);
    }
    g_node = p;
    g_node_cap = ncap;
    return 0;
}
/* 单进程载荷（L2–L4）在 lx_proc_init 之前就会 open：那段时间用这张兜底表，
 * 进程登记后 proc.c 会 lx_fs_set_fdtab() 换成进程自己的动态表。 */
static LxFdTab *g_fdtab;
static LxFile *g_fd;
static u64    g_fs_pa;
/* 当前目录（不带前导 '/'，空串 = 根）。M12-4：cd 之前是个空操作，
 * getcwd 恒回 "/"，相对路径一律当根路径查 —— 所以 `cd /bin; ls` 列的还是 /。 */
static char   g_cwd_root[FS_NAME];
/* 指向**当前进程**的 cwd（不带前导 '/'，空串=根）。进程切换时由 proc.c 调
 * lx_fs_set_cwd() 换指针 —— cwd 必须跟进程走：`top` 自己会 chdir("/proc")，
 * 共享一份的话它一退出，父 shell 的 cwd 就跟着跑进 /proc，`ls` 列出来是空的。 */
static char  *g_cwd = g_cwd_root;


/* 目录 fd 不经过 LOS_FsOpen（ramfs 不让 open 目录），只记路径；
 * node 落在 LX_VDIR_BASE.. 这一段，和真实 VFS fd（0..31）不会撞。
 * 这两个常量是 LxFile.node 的布局约定，C6OS_VFS=0 的构建（l2/elf 等不带
 * VFS 的目标）也要看得见 —— 放到 #if 外面。 */
#define LX_VDIR_BASE 0x100ull
#define LX_MAX_VDIR  8u

#if C6OS_VFS
#include "los/los_fs.h"
#include "c6os/vfs.h"

/* M9-4：/tmp 及其子路径交给内核 VFS（挂的是 ramfs）。
 * g_vpath 按 **VFS fd** 索引 —— VFS fd 全局唯一，所以多个进程同时列
 * 各自 /tmp 子目录也不会串。 */
static char g_vpath[OS_MAX_FILES][LOS_FS_PATH_MAX];

static char g_dpath[LX_MAX_VDIR][LOS_FS_PATH_MAX];


static u64 vfs_dir_slot_(const char *abspath)
{
    u32 i = 0u;

    while (i < (u32)LX_MAX_VDIR) {
        if (g_dpath[i][0] == '\0') {
            (void)os_str_cpy(g_dpath[i], abspath, (u32)LOS_FS_PATH_MAX);
            return LX_VDIR_BASE + (u64)i;
        }
        i++;
    }
    return 0ull;
}

/* 注意：传进来的是 strip_() 之后的路径（已去掉前导 '/'）。
 * M11-3 起不再写死 "/tmp"：只要挂载表认这个路径（/tmp 的 ramfs、/data 的 fatfs、
 * 以及以后任何新挂载），就交给 VFS；没挂载的还走只读 initramfs。 */
static int vfs_owns_(const char *path)
{
    char full[LOS_FS_PATH_MAX] = {0};
    char rel[LOS_FS_PATH_MAX] = {0};
    u32 mnt = 0u;
    u64 n = 0u;

    if (os_str_eq(path, "tmp") == 1 || os_str_prefix(path, "tmp/") == 1) {
        return 1;
    }
    while (path[n] != '\0') {
        n++;
    }
    if (n + 2u > (u64)sizeof(full)) {
        return 0;
    }
    full[0] = '/';
    (void)os_str_cpy((char *)&full[1], path, (u32)sizeof(full) - 2u);
    if (os_vfs_lookup((const char *)&full[0], &mnt, (char *)&rel[0],
                      (u32)sizeof(rel)) != 0) {
        return 0;
    }
    return 1;
}

static u64 vfs_err_(int rc)
{
    return (rc < 0) ? ((u64)0 - (u64)(0 - rc)) : (u64)rc;
}

static int vfs_open_(const char *path, u64 flags)
{
    UINT32 f = (UINT32)LOS_O_RDONLY;

    if ((flags & 3ull) == 1ull) {
        f = (UINT32)LOS_O_WRONLY;
    } else if ((flags & 3ull) == 2ull) {
        f = (UINT32)LOS_O_RDWR;
    }
    if ((flags & 64ull) != 0ull) {           /* O_CREAT */
        f |= (UINT32)LOS_O_CREAT;
    }
    if ((flags & 512ull) != 0ull) {          /* O_TRUNC */
        f |= (UINT32)LOS_O_TRUNC;
    }
    if ((flags & 1024ull) != 0ull) {         /* O_APPEND */
        f |= (UINT32)LOS_O_APPEND;
    }
    return (int)LOS_FsOpen((const CHAR *)path, f);
}

static int vfs_is_dir_(const char *path)
{
    LosFsStat st = {0};

    st.size = 0u;
    st.type = 0u;
    st.mode = 0u;
    st.mtime = 0u;
    if (LOS_FsStat((const CHAR *)path, &st) != 0) {
        return 0;
    }
    return (st.type == (UINT32)LOS_FS_TYPE_DIR) ? 1 : 0;
}
#else
/* 写类 syscall 的路径缓冲在 C6OS_VFS=0 时也要有确定的容量（FS_NAME 足够）。 */
#define LOS_FS_PATH_MAX FS_NAME
#endif

static u64 slen_(const char *s)
{
    u64 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* 不依赖 C6OS_VFS 的定长字符串拷贝（*at 路径辅助用，调用点已保证容量）。 */
static void scpy_(char *dst, const char *src, u64 n)
{
    u64 i;

    for (i = 0ull; i < n; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            break;
        }
    }
}

static int seq_(const char *a, const char *b)
{
    u64 i = 0;

    for (;;) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
        i++;
    }
}

static u64 h2u_(const u8 *p, u64 n)
{
    u64 v = 0;
    u64 i;

    for (i = 0; i < n; i++) {
        u8 c = p[i];
        u64 d;

        if (c >= '0' && c <= '9') {
            d = (u64)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (u64)(c - 'a') + 10ull;
        } else if (c >= 'A' && c <= 'F') {
            d = (u64)(c - 'A') + 10ull;
        } else {
            d = 0ull;
        }
        v = (v << 4) | d;
    }
    return v;
}

static u64 align4_(u64 v)
{
    return (v + 3ull) & ~3ull;
}

/* 去掉前导 "/" 与 "./"：内部一律用相对路径（"bin/busybox"）。 */
static const char *strip_(const char *p)
{
    while (*p == '/') {
        p++;
    }
    if (p[0] == '.' && p[1] == '\0') {
        return "";                     /* "." 就是根目录 */
    }
    while (p[0] == '.' && p[1] == '/') {
        p += 2;
        while (*p == '/') {
            p++;
        }
    }
    return p;
}

int lx_fs_init(u64 pa, u64 len)
{
    const u8 *img = (const u8 *)port_phys_map(pa);
    u64 off = 0;

    /* 设备节点（/dev/fb0、/dev/input/event0）与 initramfs 无关，但必须在
     * 用户程序第一次 open 之前注册好；放在这里保证所有装载路径都覆盖。 */
    lx_dev_init();
    g_fs_pa = pa;
    g_nodes = 0;
    while (off + 110ull <= len) {
        const u8 *h = img + off;
        u64 filesize;
        u64 namesize;
        u64 nameoff = off + 110ull;
        u64 dataoff;
        u64 i;

        if (h[0] != '0' || h[1] != '7' || h[2] != '0' || h[3] != '7' ||
            h[4] != '0' || h[5] != '1') {
            break;                                     /* 不是 newc */
        }
        filesize = h2u_(h + 54u, 8ull);
        namesize = h2u_(h + 94u, 8ull);
        if (namesize == 0ull || nameoff + namesize > len) {
            break;
        }
        {
            const char *nm = (const char *)(img + nameoff);

            if (nm[0] == 'T' && nm[1] == 'R' && nm[2] == 'A' && nm[3] == 'I') {
#if C6OS_VFS
                if (LOS_FsMount("", "/tmp", "ramfs", (VOID *)0) != 0) {
                    return -1;
                }
                /* Chrome / Wayland 的硬前置：/dev/shm 与 XDG_RUNTIME_DIR 必须
                 * 存在且可写；都挂 ramfs，目录由 rootfs init 建。 */
                (void)LOS_FsMount("", "/dev/shm", "ramfs", (VOID *)0);
                (void)LOS_FsMount("", "/run", "ramfs", (VOID *)0);
#endif
                return 0;                              /* TRAILER!!! */
            }
            if (node_reserve_(g_nodes + 1u) != 0) {
                return -1;
            }
            {
                const char *s = strip_(nm);
                u64 n = slen_(s);

                if (n >= FS_NAME) {
                    n = FS_NAME - 1ull;
                }
                for (i = 0; i < n; i++) {
                    g_node[g_nodes].name[i] = s[i];
                }
                g_node[g_nodes].name[n] = '\0';
                g_node[g_nodes].mode = (u32)h2u_(h + 14u, 8ull);
                g_node[g_nodes].size = filesize;
                g_node[g_nodes].off = off;             /* 先记头部偏移，下面修正 */
                g_nodes++;
            }
        }
        dataoff = align4_(nameoff + namesize);
        /* 记录数据偏移（数据在 dataoff 处） */
        g_node[g_nodes - 1ull].off = dataoff;
        off = align4_(dataoff + filesize);
    }
    port_puts("fs: 没有找到 newc 尾标，映像可能不完整\n");
    return -1;
}

u32 lx_fs_nodes(void)
{
    return g_nodes;
}

/* 路径归一化：把 "." / ".." / 重复斜杠塌掉（in → out，out 不带前导 '/'）。
 * 发行版的符号链接目标里有 ".."，不归一化就会拼出查不到的路径。 */
static int norm_(const char *in, char *out, u64 cap);

/* 把用户给的路径变成"规范化 + 相对当前 cwd"的形式（不带前导 '/'）。
 * 和 strip_() 的区别就在这里：strip_ 只去前导斜杠，相对的 "." 会被当成根。 */
static int path_(const char *in, char *out, u64 cap)
{
    char tmp[FS_NAME * 2u];
    u64 w = 0ull;

    if (in == (const char *)0) {
        return -1;
    }
    if (in[0] == '/') {
        return norm_(in, out, cap);
    }
    while (g_cwd[w] != '\0' && w + 2ull < (u64)sizeof(tmp)) {
        tmp[w] = g_cwd[w];
        w++;
    }
    if (w != 0ull) {
        tmp[w] = '/';
        w++;
    }
    while (in[0] != '\0' && w + 1ull < (u64)sizeof(tmp)) {
        tmp[w] = in[0];
        w++;
        in++;
    }
    tmp[w] = '\0';
    return norm_(tmp, out, cap);
}

static int norm_(const char *in, char *out, u64 cap)
{
    u64 w = 0;
    u64 i = 0;

    while (in[i] != '\0') {
        u64 start;
        u64 len;
        u64 k;

        while (in[i] == '/') {
            i++;
        }
        if (in[i] == '\0') {
            break;
        }
        start = i;
        while (in[i] != '\0' && in[i] != '/') {
            i++;
        }
        len = i - start;
        if (len == 1u && in[start] == '.') {
            continue;
        }
        if (len == 2u && in[start] == '.' && in[start + 1u] == '.') {
            while (w > 0u && out[w - 1u] != '/') {       /* 退一级 */
                w--;
            }
            if (w > 0u) {
                w--;
            }
            continue;
        }
        if (w > 0u) {
            if (w + 1u >= cap) {
                return -1;
            }
            out[w] = '/';
            w++;
        }
        if (w + len >= cap) {
            return -1;
        }
        for (k = 0; k < len; k++) {
            out[w] = in[start + k];
            w++;
        }
    }
    out[w] = '\0';
    return 0;
}

/* mmap(MAP_SHARED) 用：返回 memfd/ramfs 节点数据的页对齐物理基址。 */
int lx_fs_shared_pa(u64 fd, u64 off, u64 len, u64 *out_pa, u64 *out_pages)
{
    LxFile *f = lx_fdtab_get(lx_cur_proc() ? lx_cur_proc()->fd : (LxFdTab *)0, fd);

    if (f == (LxFile *)0 || f->kind != LX_FD_FILE || f->node >= (u64)g_nodes) {
        return -1;
    }
    if (g_node[f->node].ram_pa == 0ull || (off & 4095ull) != 0ull) {
        return -1;
    }
    if (len == 0ull) {
        len = 4096ull;
    }
    if (off + len > g_node[f->node].cap) {
        return -1;
    }
    *out_pa = g_node[f->node].ram_pa + off;
    *out_pages = (len + 4095ull) >> 12;
    return 0;
}

/* M57：节点数据统一访问 —— 运行时节点用 ram_data，固件 cpio 节点走物理映射。 */
static const u8 *node_data_(u32 i);

/* 纯查表，不跟符号链接 */
static int fs_find_raw(const char *path)
{
    u32 i;

    for (i = 0; i < g_nodes; i++) {
        if (g_node[i].hidden == 0u && seq_(g_node[i].name, path)) {
            return (int)i;
        }
    }
    return -1;
}

/* 查表 + 解析符号链接（保持 shortname 风格的 follow_symlinks）。
 *
 * 发行版的根文件系统里符号链接是主力：/bin/sh、/lib/libc.musl-x86_64.so.1、
 * /etc/ssl1.1/certs …… Alpine minirootfs 里有 334 个。链接目标就存在 cpio 的
 * 数据区（mode 是 S_IFLNK，数据是目标字符串），和 Linux 的 initramfs 一模一样，
 * 所以这里按 Linux 的语义解析：绝对目标从根重来，相对目标拼到链接所在目录上，
 * 最多跟 FS_LINK_MAX 层（防环，Alpine 的 /etc/ssl1.1/certs -> /etc/ssl/certs
 * 就有一对互相指向的）。 */
static int fs_find(const char *path)
{
    char cur[FS_NAME] = {0};
    char nxt[FS_NAME * 2u] = {0};
    u32 depth = 0u;

    if (norm_(path, (char *)&cur[0], (u64)sizeof(cur)) != 0) {
        return -1;
    }
    for (;;) {
        int i = fs_find_raw((const char *)&cur[0]);
        const u8 *src;
        u64 m;
        u64 n;
        u64 k;

        if (i < 0) {
            return -1;
        }
        m = (u64)g_node[(u32)i].mode;
        if ((m & 0170000ull) != 0120000ull) {
            return i;                                  /* 不是符号链接 */
        }
        if (depth >= FS_LINK_MAX) {
            return -1;
        }
        depth++;
        n = g_node[(u32)i].size;
        src = node_data_((u32)i);
        if (n == 0ull || n >= (u64)sizeof(nxt) - 2ull) {
            return -1;
        }
        if (src[0] == '/') {                           /* 绝对目标 */
            if (n >= (u64)sizeof(nxt)) {
                return -1;
            }
            for (k = 0; k < n; k++) {
                nxt[k] = (char)src[k];
            }
            nxt[n] = '\0';
        } else {                                       /* 相对目标 */
            u64 last = 0ull;
            u64 has = 0ull;
            u64 w = 0ull;

            k = 0ull;
            while (cur[k] != '\0') {
                if (cur[k] == '/') {
                    last = k;
                    has = 1ull;
                }
                k++;
            }
            if (last + n + 2ull > (u64)sizeof(nxt)) {
                return -1;
            }
            k = 0ull;
            while (k < last) {
                nxt[w] = cur[k];
                w++;
                k++;
            }
            if (has != 0ull) {
                nxt[w] = '/';
                w++;
            }
            for (k = 0ull; k < n; k++) {
                nxt[w] = (char)src[k];
                w++;
            }
            nxt[w] = '\0';
        }
        if (norm_((const char *)&nxt[0], (char *)&cur[0], (u64)sizeof(cur)) != 0) {
            return -1;
        }
    }
}

/* M57：节点数据统一访问 —— 运行时节点用 ram_data，固件 cpio 节点走物理映射。 */
static const u8 *node_data_(u32 i)
{
    if (g_node[i].ram_data != (u8 *)0) {
        return (const u8 *)g_node[i].ram_data;
    }
    return (const u8 *)port_phys_map(g_fs_pa + g_node[i].off);
}

/* M57：追加一个运行时节点（符号链接/硬链接），借用 g_node[] 的连续空间。 */
static int node_add_(const char *name, u32 mode, const u8 *data, u64 size)
{
    u32 i;

    if (name[0] == '\0' || node_reserve_(g_nodes + 1u) != 0) {
        return -1;
    }
    for (i = 0u; name[i] != '\0' && i < FS_NAME - 1u; i++) {
        g_node[g_nodes].name[i] = name[i];
    }
    g_node[g_nodes].name[i] = '\0';
    g_node[g_nodes].mode = mode;
    g_node[g_nodes].size = size;
    {
        u64 np = (size + 4095ull) >> 12;
        u64 pa = 0ull;
        if (np == 0ull) np = 1ull;
        pa = port_phys_alloc_pages(np);
        if (pa == 0ull) {
            g_node[g_nodes].name[0] = '\0';
            return -1;
        }
        g_node[g_nodes].ram_pa = pa;
        g_node[g_nodes].ram_pages = (u32)np;
        g_node[g_nodes].ram_data = (u8 *)port_phys_map(pa);
        for (u64 k = 0ull; k < np * 4096ull; k++) {
            g_node[g_nodes].ram_data[k] = 0u;
        }
    }
    g_node[g_nodes].cap = size;
    g_node[g_nodes].writable = 0u;
    g_node[g_nodes].hidden = 0u;
    g_node[g_nodes].seals = 0u;
    if (size > 0ull && data != (const u8 *)0) {
        u64 k;
        for (k = 0ull; k < size; k++) {
            g_node[g_nodes].ram_data[k] = data[k];
        }
    }
    g_node[g_nodes].off = 0ull;
    return (int)g_nodes++;
}

/* 把 ram_data 扩到至少 need 字节（按 4KB 增长，内容保留）。 */
static int node_ram_grow_(u32 idx, u64 need)
{
    u64 ncap;
    u8 *p;

    if (need <= g_node[idx].cap) {
        return 0;
    }
    ncap = (g_node[idx].cap == 0ull) ? 4096ull : g_node[idx].cap;
    while (ncap < need) {
        if (ncap > 0x40000000ull) {
            return -1;
        }
        ncap <<= 1;
    }
    {
        u64 np = (ncap + 4095ull) >> 12;
        u64 pa = port_phys_alloc_pages(np);
        if (pa == 0ull) {
            return -1;
        }
        p = (u8 *)port_phys_map(pa);
        for (u64 k = 0ull; k < np * 4096ull; k++) p[k] = 0u;
        if (g_node[idx].ram_data != (u8 *)0 && g_node[idx].size != 0ull) {
            u64 k;
            u64 n = (g_node[idx].size < ncap) ? g_node[idx].size : ncap;
            for (k = 0ull; k < n; k++) p[k] = g_node[idx].ram_data[k];
        }
        if (g_node[idx].ram_pa != 0ull) {
            for (u64 k = 0ull; k < (u64)g_node[idx].ram_pages; k++) {
                port_phys_put(g_node[idx].ram_pa + k * 4096ull);
            }
        }
        g_node[idx].ram_pa = pa;
        g_node[idx].ram_pages = (u32)np;
    }
    g_node[idx].ram_data = p;
    g_node[idx].cap = ncap;
    return 0;
}

/* M57：按名字找可写节点（含运行时节点），存在返回下标，否则 -1。 */
static int node_find_rw_(const char *name)
{
    u32 i;

    for (i = 0u; i < g_nodes; i++) {
        if (seq_(g_node[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static const char *fs_node_name(u32 i)
{
    return g_node[i].name;
}

u64 lx_fs_node_size(u32 i)
{
    return g_node[i].size;
}

u64 lx_fs_node_data_pa(u32 i)
{
    return g_fs_pa + g_node[i].off;
}

u32 lx_fs_node_mode(u32 i)
{
    return g_node[i].mode;
}

static int is_dir(const char *path)
{
    int i = fs_find(path);
    u32 m;

    if (path[0] == '\0') {
        return 1;                                      /* 根 */
    }
    if (i < 0) {
        return 0;
    }
    m = g_node[(u32)i].mode;
    return (m & 0040000u) != 0u;
}

/* 名字是否直接挂在 dir 下（不含更深层）。 */
static int in_dir(const char *name, const char *dir)
{
    u64 dl = slen_(dir);
    u64 i;

    if (dl == 0ull) {
        for (i = 0; name[i] != '\0'; i++) {
            if (name[i] == '/') {
                return 0;
            }
        }
        return name[0] != '\0';
    }
    for (i = 0; i < dl; i++) {
        if (name[i] != dir[i]) {
            return 0;
        }
    }
    if (name[dl] != '/') {
        return 0;
    }
    for (i = dl + 1ull; name[i] != '\0'; i++) {
        if (name[i] == '/') {
            return 0;
        }
    }
    return name[dl + 1ull] != '\0';
}

/* ---- fd 表 ---- */

static LxFile *fd_get(u64 fd)
{
    return lx_fdtab_get(g_fdtab, fd);
}

/* fd 表跟着进程走：proc.c 在切换/登记时传入进程自己的表。NULL 表示退回
 * "内核自带那张表"（还没登记进程的单进程载荷）。 */
void lx_fs_set_fdtab(LxFdTab *t)
{
    if (t == (LxFdTab *)0) {
        if (g_fdtab == (LxFdTab *)0) {
            g_fdtab = lx_fdtab_new();
        }
        g_fd = (g_fdtab != (LxFdTab *)0) ? g_fdtab->ent : (LxFile *)0;
        return;
    }
    g_fdtab = t;
    g_fd = t->ent;
}

/* execve 用：把路径换成节点号；<0 表示不存在。 */
i64 lx_fs_lookup(const char *path)
{
    const char *s = strip_(path);

    if (s[0] == '\0') {
        return 0;
    }
    return (i64)fs_find(s);
}

/* 进程登记时接管可能已经存在的那张兜底表：单进程载荷在 lx_proc_init 之前就会
 * open（比如 initramfs 里的运行时），那些 fd 属于第一个进程，不能丢。 */
LxFdTab *lx_fs_get_fdtab(void)
{
    return g_fdtab;
}

static u64 fd_alloc(void)
{
    u64 fd = lx_fdtab_alloc(g_fdtab);

    g_fd = (g_fdtab != (LxFdTab *)0) ? g_fdtab->ent : (LxFile *)0;
    return fd;
}

/* 填一个刚 alloc 出来的槽：node/is_dir/kind 之外，把 open(2) 的 flags 记成
 * "文件状态标志"（F_GETFL 要看得见 O_APPEND/O_NONBLOCK/访问模式），
 * O_CLOEXEC 记成 fd 标志（F_GETFD 要看得见），execve 时据此关 fd。 */
static void fd_set(u64 fd, u64 node, u32 is_dir, u32 kind, u64 flags)
{
    LxFile *e = &g_fd[fd];

    e->node = node;
    e->pos = 0ull;
    e->is_dir = is_dir;
    e->kind = kind;
    e->oflags = (u32)(flags & (u64)(LX_O_ACCMODE | LX_O_APPEND | LX_O_NONBLOCK));
    e->flags = ((flags & (u64)LX_O_CLOEXEC) != 0ull) ? (u32)LX_FD_CLOEXEC : 0u;
    e->dev = 0u;
}

/* memfd_create(2)：匿名可写内存文件。节点 hidden=1，没有路径；fd 指向它，
 * mmap/read/write/pread/pwrite/ftruncate 都按普通文件处理。 */
u64 lx_fs_memfd_create(const char *name, u32 flags)
{
    u64 fd;
    u32 i;
    u32 idx;

    if ((flags & ~(u32)(0x0001u | 0x0002u)) != 0u) {  /* MFD_CLOEXEC|ALLOW_SEALING */
        return (u64)0 - LX_EINVAL;
    }
    if (node_reserve_(g_nodes + 1u) != 0) {
        return (u64)0 - LX_ENOMEM;
    }
    idx = g_nodes;
    for (i = 0u; i < FS_NAME - 1u; i++) {
        char c = (name != (const char *)0) ? name[i] : 0;

        if (c == '\0') {
            break;
        }
        if (c == '/') {
            c = '_';
        }
        g_node[idx].name[i] = c;
    }
    if (i == 0u) {
        g_node[idx].name[0] = 'm';
        g_node[idx].name[1] = 'e';
        g_node[idx].name[2] = 'm';
        g_node[idx].name[3] = 'f';
        g_node[idx].name[4] = 'd';
        g_node[idx].name[5] = '\0';
        i = 5u;
    }
    g_node[idx].name[i] = '\0';
    g_node[idx].off = 0ull;
    g_node[idx].size = 0ull;
    g_node[idx].mode = 0100600u;             /* S_IFREG | 0600 */
    g_node[idx].writable = 1u;
    g_node[idx].hidden = 1u;
    g_node[idx].seals = ((flags & 0x0002u) == 0u) ? 0x0001u : 0u;
    g_node[idx].cap = 0ull;
    g_node[idx].ram_data = (u8 *)0;
    g_nodes++;
    fd = fd_alloc();
    if (fd == LX_FD_NONE) {
        return (u64)0 - LX_EMFILE;
    }
    fd_set(fd, (u64)idx, 0u, LX_FD_FILE,
           ((flags & 0x0001u) != 0u) ? (u64)LX_O_CLOEXEC : 0ull);
    if (g_fd != (LxFile *)0) {
        g_fd[fd].oflags = LX_O_RDWR;
    }
    return fd;
}

/* fcntl(F_ADD_SEALS/F_GET_SEALS)：只对 memfd 节点生效。 */
u64 lx_fs_seals(u64 fd, u32 cmd, u32 seals)
{
    LxFile *f = fd_get(fd);
    u32 idx;

    if (f == (LxFile *)0 || (u32)f->node >= g_nodes) {
        return (u64)0 - LX_EBADF;
    }
    idx = (u32)f->node;
    if (g_node[idx].hidden == 0u || g_node[idx].writable == 0u) {
        return (u64)0 - LX_EINVAL;
    }
    if (cmd == 1034u) {                      /* F_GET_SEALS */
        return (u64)g_node[idx].seals;
    }
    if (cmd != 1033u) {                      /* F_ADD_SEALS */
        return (u64)0 - LX_EINVAL;
    }
    if ((seals & ~0x001fu) != 0u) {
        return (u64)0 - LX_EINVAL;
    }
    if ((g_node[idx].seals & 0x0001u) != 0u) {   /* F_SEAL_SEAL */
        return (u64)0 - LX_EPERM;
    }
    if ((seals & 0x0001u) != 0u) {
        g_node[idx].seals |= 0x0001u;
    }
    if ((seals & 0x0002u) != 0u && g_node[idx].size != 0ull) {
        return (u64)0 - LX_EBUSY;
    }
    g_node[idx].seals |= seals;
    return 0ull;
}

void lx_fs_fd_reset(void)
{
    u64 i;
    u64 n;

    if (g_fdtab == (LxFdTab *)0) {
        g_fdtab = lx_fdtab_new();
        g_fd = (g_fdtab != (LxFdTab *)0) ? g_fdtab->ent : (LxFile *)0;
        if (g_fd == (LxFile *)0) {
            return;
        }
    }
    n = (u64)g_fdtab->cap;
    for (i = 0; i < n; i++) {              /* execve：除 0/1/2 全关掉（同 Linux） */
        if (g_fd[i].kind == LX_FD_DEV && g_fd[i].dev != 0u) {
            lx_dev_release(lx_dev_of(g_fd[i].node));
        }
        g_fd[i].node = LX_FD_NONE;
        g_fd[i].pos = 0ull;
        g_fd[i].is_dir = 0u;
        g_fd[i].kind = LX_FD_FILE;
        g_fd[i].dev = 0u;
    }
    g_fd[0].node = LX_FD_CONSOLE;
    g_fd[1].node = LX_FD_CONSOLE;
    g_fd[2].node = LX_FD_CONSOLE;
}

/* ---- stat / dirent ---- */

static void put64(u8 *p, u64 v)
{
    u64 i;

    for (i = 0; i < 8ull; i++) {
        p[i] = (u8)((v >> (8ull * i)) & 0xffull);
    }
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xffu);
    p[1] = (u8)((v >> 8) & 0xffu);
    p[2] = (u8)((v >> 16) & 0xffu);
    p[3] = (u8)((v >> 24) & 0xffu);
}

/* x86-64 的 struct stat：144 字节，字段偏移见 Linux 头文件。 */
static int fill_stat(u64 ustat, u32 ino, u64 size, u32 mode)
{
    u8 st[144];
    u64 i;

    if (lx_user_ok(ustat, 144ull, C6OS_PROT_W) == 0) {
        return -1;
    }
    for (i = 0; i < 144ull; i++) {
        st[i] = 0;
    }
    put64(st + 0u, 0x0801ull);                  /* st_dev */
    put64(st + 8u, (u64)ino + 1ull);            /* st_ino */
    put64(st + 16u, 1ull);                      /* st_nlink */
    put32(st + 24u, mode);                      /* st_mode */
    put64(st + 40u, 0ull);                      /* st_rdev */
    put64(st + 48u, size);                      /* st_size */
    put64(st + 56u, 4096ull);                   /* st_blksize */
    put64(st + 64u, (size + 511ull) / 512ull);  /* st_blocks */
    for (i = 0; i < 144ull; i++) {
        *(u8 *)(ustat + i) = st[i];
    }
    return 0;
}

/* statx(2)（nr 332）：真实程序（如 glibc 的 ls）用它取目录/文件元数据。
 * 复用 lx_fs_statat 的完整路径解析（initramfs/proc/VFS 挂载点 at），
 * 做法是先把 legacy stat(144B) 暂存到用户给的 256B statx 缓冲前部，
 * 读出 ino/mode/size 后原地改写成 statx 布局，省掉重复一套路径解析。
 * x86-64 struct statx 256B：mask@0 blksize@4 attributes@8 nlink@16
 * uid@20 gid@24 mode@28 ino@32 size@40 blocks@48 btime@80 ctime@96 mtime@112。 */
u64 lx_fs_statx(u64 dirfd, u64 upath, u64 flags, u64 ustatx)
{
    u32 i;

    if (lx_user_ok(ustatx, 256ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    {
        u64 r = lx_fs_statat(dirfd, upath, ustatx, flags);

        if ((i64)r < 0) {
            return r;                      /* ENOENT/EFAULT 等原样透传 */
        }
    }
    /* 从刚写好的 legacy stat 取字段：ino@8 / mode@24 / size@48 */
    {
        u64 ino = *(const u64 *)(ustatx + 8ull);
        u64 size = *(const u64 *)(ustatx + 48ull);
        u32 mode = *(const u32 *)(ustatx + 24ull);
        u8 sx[256];

        for (i = 0u; i < 256u; i++) { sx[i] = 0u; }
        /* STATX_BASIC_STATS = 0x7ff；VFS 只能可靠提供 mode/size/ino/nlink */
        put32(sx + 0u, 0x7ffu);            /* stx_mask */
        put32(sx + 4u, 4096u);             /* stx_blksize */
        put32(sx + 16u, 1u);               /* stx_nlink */
        sx[20] = 0u; sx[21] = 0u;          /* stx_uid (u16) */
        sx[24] = 0u; sx[25] = 0u;          /* stx_gid (u16) */
        sx[28] = (u8)(mode & 0xffu);       /* stx_mode (u16) */
        sx[29] = (u8)((mode >> 8) & 0xffu);
        put64(sx + 32u, ino);              /* stx_ino */
        put64(sx + 40u, size);             /* stx_size */
        put64(sx + 48u, (size + 511ull) / 512ull);  /* stx_blocks */
        for (i = 0u; i < 256u; i++) {
            *(u8 *)(ustatx + i) = sx[i];
        }
    }
    return 0ull;
}

/* ---- 系统调用实现 ---- */

/* M15-1a：*at 系列的相对路径解析（B7）。
 * 以前所有 *at 都 `(void)dirfd`，相对路径一律按 cwd 算 —— 但 busybox 的
 * `cp -a`/`find`/`unlinkat` 会先 open 一个目录拿 fd，再对那个 fd 做相对
 * 操作；不认 dirfd 就会操作错目录。这里把 dirfd 翻译成"基准绝对路径"：
 *   AT_FDCWD (-100) → 当前 cwd
 *   LX_FD_ROOT      → 根
 *   VFS 目录 fd     → g_dpath[slot]（/tmp 等挂载点里的子目录）
 *   initramfs 目录  → 该节点的 name
 * 返回 0 成功，负数 = -errno。 */
#define LX_AT_FDCWD ((u64)0 - 100ull)

/* glibc/busybox 里 AT_FDCWD 是 int -100。做系统调用时上游有符号扩展的
 * （0xffff...ff9c），也有只置低 32 位的（0x00000000ffffff9c）—— 后者是 gcc
 * 把 int 常量零扩展的结果。两种都认，否则 `stat(".")` 会被当成"fd -100
 * 不存在"报 ENOTDIR（L4 的 `ls: .: Not a directory`）。 */
static int is_at_fdcwd_(u64 dirfd)
{
    return (dirfd == LX_AT_FDCWD) || ((dirfd & 0xffffffffull) == 0xffffff9cull);
}

static void proc_u32_(u32 v, char *out);

/* proc 虚拟目录 node 区段（定义在后面的 procfs 段；dirfd_base_ 先用）。 */
#define LX_PROC_PIDDIR_BASE  0x10000u
#define LX_PROC_NETDIR_BASE  0x20000u
#define LX_PROC_FDDIR_BASE   0x30000u
#define LX_PROC_TASKDIR_BASE 0x40000u

static int dirfd_base_(u64 dirfd, char *base, u64 cap)
{
    LxFile *f;

    if (is_at_fdcwd_(dirfd)) {
        u64 n = slen_(g_cwd);

        base[0] = '\0';
        if (g_cwd[0] != '\0') {
            if (n + 2ull >= cap) {
                return -LX_ENAMETOOLONG;
            }
            (void)scpy_(base, g_cwd, cap);
            base[n] = '/';
            base[n + 1ull] = '\0';
        }
        return 0;
    }
    f = fd_get(dirfd);
    if (f == (LxFile *)0 || f->is_dir == 0u) {
        return -LX_ENOTDIR;
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS && f->node >= LX_VDIR_BASE) {
        u32 slot = (u32)(f->node - LX_VDIR_BASE);
        const char *p;
        u64 n;

        if (slot >= (u32)LX_MAX_VDIR || g_dpath[slot][0] == '\0') {
            return -LX_EBADF;
        }
        p = g_dpath[slot];
        n = slen_(p);
        if (n + 2ull >= cap) {
            return -LX_ENAMETOOLONG;
        }
        (void)scpy_(base, p, cap);
        base[n] = '/';
        base[n + 1ull] = '\0';
        return 0;
    }
#endif
    /* procfs 目录 fd：fstatat(proc_fd, "self/task") 相对路径要拼回
     * "proc/..." 前缀（Crashpad/sandbox 依赖这个相对路径）。 */
    if (f->kind == LX_FD_PROC) {
        u64 n = 0ull;
        const char *pfx = "proc";
        u32 i;
        for (i = 0u; pfx[i] != '\0' && n + 1ull < cap; i++) {
            base[n++] = pfx[i];
        }
        if (f->node >= LX_PROC_PIDDIR_BASE && f->node < LX_PROC_PIDDIR_BASE + 0x10000u) {
            u32 pid = (u32)(f->node - LX_PROC_PIDDIR_BASE);
            char num[12];
            proc_u32_(pid, num);
            for (i = 0u; num[i] != '\0' && n + 1ull < cap; i++) {
                base[n++] = num[i];
            }
        } else if (f->node >= LX_PROC_TASKDIR_BASE && f->node < LX_PROC_TASKDIR_BASE + 0x10000u) {
            u32 pid = (u32)(f->node - LX_PROC_TASKDIR_BASE);
            char num[12];
            const char *sfx = "/task";
            proc_u32_(pid, num);
            base[n++] = '/';
            for (i = 0u; num[i] != '\0' && n + 1ull < cap; i++) base[n++] = num[i];
            for (i = 0u; sfx[i] != '\0' && n + 1ull < cap; i++) base[n++] = sfx[i];
        } else if (f->node >= LX_PROC_FDDIR_BASE && f->node < LX_PROC_FDDIR_BASE + 0x10000u) {
            u32 pid = (u32)(f->node - LX_PROC_FDDIR_BASE);
            char num[12];
            const char *sfx = "/fd";
            proc_u32_(pid, num);
            base[n++] = '/';
            for (i = 0u; num[i] != '\0' && n + 1ull < cap; i++) base[n++] = num[i];
            for (i = 0u; sfx[i] != '\0' && n + 1ull < cap; i++) base[n++] = sfx[i];
        } else if (f->node == LX_PROC_NETDIR_BASE) {
            const char *sfx = "/net";
            for (i = 0u; sfx[i] != '\0' && n + 1ull < cap; i++) base[n++] = sfx[i];
        }
        base[n] = '\0';
        if (n + 1ull < cap) { base[n++] = '/'; base[n] = '\0'; }
        return 0;
    }
    if (f->node == LX_FD_ROOT) {
        base[0] = '\0';
        return 0;
    }
    if (f->kind == LX_FD_FILE && f->node < (u64)g_nodes) {
        const char *p = g_node[(u32)f->node].name;
        u64 n = slen_(p);

        if (n + 2ull >= cap) {
            return -LX_ENAMETOOLONG;
        }
        (void)scpy_(base, p, cap);
        base[n] = '/';
        base[n + 1ull] = '\0';
        return 0;
    }
    return -LX_EBADF;
}

/* 把用户路径串拷进 buf（返回 0 / -EFAULT / -EINVAL），和原来各处的内联循环等价。 */
static int get_user_path_(u64 upath, char *buf, u64 cap)
{
    u64 i = 0ull;

    if (lx_user_ok(upath, 1ull, C6OS_PROT_R) == 0) {
        return -LX_EFAULT;
    }
    for (;;) {
        u8 c;

        if (i + 1ull >= cap) {
            return -LX_EINVAL;
        }
        if (lx_user_ok(upath + i, 1ull, C6OS_PROT_R) == 0) {
            return -LX_EFAULT;
        }
        c = *(const u8 *)(upath + i);
        buf[i] = (char)c;
        if (c == 0u) {
            return 0;
        }
        i++;
    }
}

/* dirfd + 用户路径 → 内核规范化的绝对路径（ap，不带前导 '/'）。
 * 绝对路径直接规范之；相对路径拼到 dirfd_base_ 上（不是 cwd）。 */
static int at_path_(u64 dirfd, u64 upath, char *ap, u64 cap)
{
    char path[FS_NAME];
    char base[FS_NAME];
    char tmp[FS_NAME * 2u];
    u64 w = 0ull;
    u64 i = 0ull;
    int rc;

    rc = get_user_path_(upath, path, (u64)sizeof path);
    if (rc != 0) {
        return rc;
    }
    if (path[0] == '/') {
        return norm_(path, ap, cap) == 0 ? 0 : -LX_ENAMETOOLONG;
    }
    rc = dirfd_base_(dirfd, base, (u64)sizeof base);
    if (rc != 0) {
        return rc;
    }
    while (base[w] != '\0' && w + 1ull < (u64)sizeof tmp - 1ull) {
        tmp[w] = base[w];
        w++;
    }
    while (path[i] != '\0' && w + 1ull < (u64)sizeof tmp - 1ull) {
        tmp[w++] = path[i++];
    }
    tmp[w] = '\0';
    return norm_(tmp, ap, cap) == 0 ? 0 : -LX_ENAMETOOLONG;
}

/* 写类 syscall 用：dirfd+路径 → 带前导 '/' 的绝对路径（喂给 LOS_Fs*）。
 * 返回 0 / -errno；abspath 至少 LOS_FS_PATH_MAX 字节。 */
static int at_abspath_(u64 dirfd, u64 upath, char *abspath, u64 cap)
{
    char ap[FS_NAME] = {0};
    u64 n = 0ull;
    int rc = at_path_(dirfd, upath, ap, (u64)sizeof ap);

    if (rc != 0) {
        return rc;
    }
    if (slen_(ap) + 2ull > cap) {
        return -LX_ENAMETOOLONG;
    }
    if (ap[0] != '\0') {
        abspath[n++] = '/';
        while (ap[n - 1ull] != '\0') {
            abspath[n] = ap[n - 1ull];
            n++;
        }
    }
    abspath[n] = '\0';
    return 0;
}

/* M15-2 / G2：把 /dev/xxx 映射到设备注册表里的 LxDev id。
 * 返回 0 表示不是设备路径；LX_DEV_PTMX 是 pty 子系统自己的特殊值（不是
 * LxDev id，open 里必须先判断它，否则 0xfffffff3 会被当成合法 id）。 */
static u64 dev_node_(const char *s)
{
    LxDev *d;

    if (seq_(s, "dev/ptmx") == 1) {
        return LX_DEV_PTMX;
    }
    d = lx_dev_find(s);
    return (d != (LxDev *)0) ? (u64)d->id : 0ull;
}

/* /dev/pts/N：返回 pty 对象 id（N），不是 dev 哨兵。返回 ~0 表示不匹配。 */
static u64 pts_node_(const char *s)
{
    u32 i;

    /* 前缀比较（seq_ 要求完全相等，这里后面还有 N） */
    {
        const char *pfx = "dev/pts/";

        for (u32 k = 0u; pfx[k] != '\0'; k++) {
            if (s[k] != pfx[k]) { return ~0ull; }
        }
    }
    i = 0u;
    for (u32 k = 8u; s[k] != '\0'; k++) {
        if (s[k] < '0' || s[k] > '9') { return ~0ull; }
        i = i * 10u + (u32)(s[k] - '0');
    }
    return (u64)i;
}

/* ---- M17-1：procfs 最小集 -------------------------------------------------
 * busybox ps/top/free/df 读的是 /proc/{meminfo,stat,uptime,cpuinfo,
 * self/status,self/stat,self/cmdline,PID/...}。我们用一个"虚拟文件"层：
 * 每个 proc 文件对应一个 id，内容在首次访问时一次性生成到内核堆缓冲，
 * 之后 open/read/pread/stat/getdents 都走同一套 fd 机制（kind=LX_FD_PROC）。
 * 内容常驻（条目数固定、总量 < 4KB），不需要 fd close hook，也不占 VFS 文件槽。 */
#define PROC_BUF_MAX 32768u

typedef struct {
    u32 id;
    u32 size;
    char *buf;
} LxProcFile;

static LxProcFile g_procf[LX_PROC_MAX + 1u];

static char *proc_buf_(u32 id)
{
    if (g_procf[id].buf == (char *)0) {
        g_procf[id].buf = (char *)port_kcalloc(1ull, (u64)PROC_BUF_MAX);
    }
    return g_procf[id].buf;
}

/* 极简缓冲追加：内核侧没有 snprintf，proc 内容的数字都是无符号小整数。 */
static u32 pcat_(char *b, u32 off, const char *s)
{
    while (*s != '\0') {
        b[off++] = *s++;
    }
    return off;
}

static u32 pnum_(char *b, u32 off, u64 v)
{
    char tmp[24];
    u32 n = 0u;
    u32 i;

    if (v == 0ull) {
        b[off++] = '0';
        return off;
    }
    while (v != 0ull) {
        tmp[n++] = (char)('0' + (u32)(v % 10ull));
        v /= 10ull;
    }
    while (n > 0u) {
        b[off++] = tmp[--n];
    }
    return off;
}

static u32 proc_meminfo_(char *b)
{
    u32 o = 0u;
    u64 total = port_phys_total_kb();
    u64 freekb = port_phys_free_kb();

    o = pcat_(b, o, "MemTotal:       ");
    o = pnum_(b, o, total);
    o = pcat_(b, o, " kB\nMemFree:        ");
    o = pnum_(b, o, freekb);
    o = pcat_(b, o, " kB\nMemAvailable:   ");
    o = pnum_(b, o, freekb);
    o = pcat_(b, o, " kB\nBuffers:            0 kB\n"
                    "Cached:          2048 kB\nSwapTotal:             0 kB\n"
                    "SwapFree:              0 kB\n");
    return o;
}

static u32 proc_uptime_(char *b)
{
    u64 ms = port_cycles() / 1000000ull;
    u32 o = 0u;
    u32 sec = (u32)(ms / 1000ull);
    u32 cs = (u32)((ms % 1000ull) / 10ull);

    o = pnum_(b, o, sec);
    b[o++] = '.';
    if (cs < 10u) { b[o++] = '0'; }
    o = pnum_(b, o, cs);
    b[o++] = ' ';
    o = pnum_(b, o, sec);
    b[o++] = '.';
    if (cs < 10u) { b[o++] = '0'; }
    o = pnum_(b, o, cs);
    b[o++] = '\n';
    return o;
}

static u32 proc_cpuinfo_(char *b)
{
    u32 o = 0u;

    o = pcat_(b, o, "processor\t: 0\nvendor_id\t: c6os\n"
                    "model name\t: c6os x86-64 Linux ABI\n"
                    "cpu MHz\t\t: 1000.000\n"
                    "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep "
                    "mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss "
                    "syscall nx lm constant_tsc pni ssse3 sse4_1 sse4_2\n");
    return o;
}

static u32 proc_status_(char *b, u32 pid)
{
    u32 o = 0u;

    o = pcat_(b, o, "Name:\tbusybox\nState:\tS (sleeping)\nTgid:\t");
    o = pnum_(b, o, pid);
    o = pcat_(b, o, "\nPid:\t");
    o = pnum_(b, o, pid);
    o = pcat_(b, o, "\nPPid:\t0\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n"
                    "VmPeak:\t    8192 kB\nVmSize:\t    8192 kB\n"
                    "VmRSS:\t    4096 kB\nThreads:\t1\n");
    return o;
}

static u32 proc_cmdline_(char *b)
{
    u32 o = pcat_(b, 0u, "/bin/busybox");

    b[o++] = '\0';                             /* cmdline 以 NUL 分隔参数 */
    return o;
}

static u32 proc_stat_(char *b)
{
    u32 o = 0u;

    o = pcat_(b, o, "cpu  100 0 50 1000 0 0 0 0 0 0\ncpu0 100 0 50 1000 0 0 0 0 0 0\n"
                    "intr 0\nctxt 100\nbtime 1700000000\n"
                    "processes 1\nprocs_running 1\nprocs_blocked 0\n");
    return o;
}

/* /proc/mounts：busybox df -h 用它列已挂载文件系统。以前写死 rootfs+tmpfs，
 * /data 这类运行时挂上的 FAT 卷根本看不到。现在按挂载表逐项枚举，6 列格式
 * 与 mount(5) 一致；rootfs 和 proc 是虚拟的，单独补两行。 */
static u32 proc_mounts_(char *b)
{
    u32 o = pcat_(b, 0u, "rootfs / romfs rw 0 0\n");
#if C6OS_VFS
    {
        u32 i = 0u;

        while (i < (u32)OS_MAX_MOUNTS) {
            OsMount *m = &g_os_mounts[i];

            if (m->used != 0u) {
                if (m->dev[0] != '\0') {
                    o = pcat_(b, o, "/dev/");
                    o = pcat_(b, o, m->dev);
                } else if (os_str_eq(m->type, "ramfs") == 1) {
                    o = pcat_(b, o, "tmpfs");   /* 和 Linux 的 tmpfs 行同形 */
                } else {
                    o = pcat_(b, o, m->type);
                }
                b[o++] = ' ';
                o = pcat_(b, o, m->path);
                b[o++] = ' ';
                o = pcat_(b, o, m->type);
                o = pcat_(b, o, " rw 0 0\n");
            }
            i++;
        }
    }
#endif
    o = pcat_(b, o, "proc /proc proc rw 0 0\n");
    return o;
}

/* /proc/net/dev：busybox ifconfig 靠它列接口。前两行是 Linux 的固定表头，
 * 数据行是 "<name>: <16 个收发计数>"，计数直接来自 NetIf 的驱动统计。 */
static u32 proc_netdev_(char *b)
{
    static const char hdr[] =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|"
        "bytes    packets errs drop fifo colls carrier compressed\n";
#if C6OS_NET
    LosNetIf *nif = NetIfGet(0u);
#endif
    u32 o = pcat_(b, 0u, hdr);
    u32 i;

#if C6OS_NET
    if (nif != (LosNetIf *)0) {
        i = 0u;
        while (nif->name[i] != '\0' && i < 7u) {
            b[o++] = nif->name[i];
            i++;
        }
        b[o++] = ':';
        b[o++] = ' ';
        o = pnum_(b, o, (u64)nif->rx_packets * 60ull); b[o++] = ' ';
        o = pnum_(b, o, (u64)nif->rx_packets);         b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, (u64)nif->rx_dropped);          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, (u64)nif->tx_packets * 60ull);  b[o++] = ' ';
        o = pnum_(b, o, (u64)nif->tx_packets);          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, (u64)nif->tx_dropped);          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = ' ';
        o = pnum_(b, o, 0ull);                          b[o++] = '\n';
    }
#else
    (void)i;
#endif
    return o;
}

static u32 proc_pidstat_(char *b, u32 pid)
{
    /* /proc/PID/stat：busybox top 用"跳 N 个空格"的方式定位 utime/stime，
     * 短行会让它的扫描指针越过用户缓冲一路走到栈顶而 #PF。这里给足字段
     * （56 个，覆盖 proc(5) 全部 52 字段），编号与 proc(5) 严格对齐。 */
    u32 o = pnum_(b, 0u, pid);

    o = pcat_(b, o, " (busybox) S 0 0 0 0 -1 0 0 0 0 0 0 0 "
                    "10 5 0 0 1 0 20 0 1 1000 1 0 0 0 0 0 0 0 0 0 0 "
                    "0 0 0 0 0 0 0 0 15 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    return o;
}

static u32 proc_gen_(u32 id, u32 pid, char **out)
{
    char *b;

    if (id == 0u || id > LX_PROC_MAX) {
        return 0u;
    }
    b = proc_buf_(id);
    if (b == (char *)0) {
        return 0u;
    }
    switch (id) {
    case LX_PROC_meminfo:   g_procf[id].size = proc_meminfo_(b); break;
    case LX_PROC_uptime:    g_procf[id].size = proc_uptime_(b); break;
    case LX_PROC_cpuinfo:   g_procf[id].size = proc_cpuinfo_(b); break;
    case LX_PROC_stat:      g_procf[id].size = proc_stat_(b); break;
    case LX_PROC_cmdline:   g_procf[id].size = proc_cmdline_(b); break;
    case LX_PROC_status:    g_procf[id].size = proc_status_(b, pid); break;
    case LX_PROC_pidstat:   g_procf[id].size = proc_pidstat_(b, pid); break;
    case LX_PROC_pidstatus: g_procf[id].size = proc_status_(b, pid); break;
    case LX_PROC_pidcmdline:g_procf[id].size = proc_cmdline_(b); break;
    case LX_PROC_mounts:    g_procf[id].size = proc_mounts_(b); break;
    case LX_PROC_netdev:    g_procf[id].size = proc_netdev_(b); break;
    case LX_PROC_inotify_watches:
        g_procf[id].size = (u32)slen_("65536\n");
        for (u32 k = 0u; k < g_procf[id].size; k++) b[k] = "65536\n"[k];
        break;
    case LX_PROC_inotify_events:
        g_procf[id].size = (u32)slen_("16384\n");
        for (u32 k = 0u; k < g_procf[id].size; k++) b[k] = "16384\n"[k];
        break;
    case LX_PROC_self_maps: {
        LxProc *p = lx_cur_proc();
        if (p != (LxProc *)0) {
            g_procf[id].size = lx_vma_maps(p->as, b, PROC_BUF_MAX - 1u);
        }
        break;
    }
    case LX_PROC_inotify_inst:
        g_procf[id].size = (u32)slen_("128\n");
        for (u32 k = 0u; k < g_procf[id].size; k++) b[k] = "128\n"[k];
        break;
    default: return 0u;
    }
    *out = b;
    return g_procf[id].size;
}

/* 把 /proc/... 路径映射到 id；pid=0 表示"self"或全局文件。
 * 返回 0 = 不是可打开的 proc 文件。 */
static u32 proc_id_(const char *s, u32 *pid)
{
    *pid = 0u;
    if (seq_(s, "proc/meminfo") == 1)  return LX_PROC_meminfo;
    if (seq_(s, "proc/uptime") == 1)   return LX_PROC_uptime;
    if (seq_(s, "proc/cpuinfo") == 1)  return LX_PROC_cpuinfo;
    if (seq_(s, "proc/stat") == 1)     return LX_PROC_stat;
    if (seq_(s, "proc/mounts") == 1)   return LX_PROC_mounts;
    if (seq_(s, "proc/net/dev") == 1)  return LX_PROC_netdev;
    if (seq_(s, "proc/sys/fs/inotify/max_user_watches") == 1) return LX_PROC_inotify_watches;
    if (seq_(s, "proc/sys/fs/inotify/max_queued_events") == 1) return LX_PROC_inotify_events;
    if (seq_(s, "proc/sys/fs/inotify/max_user_instances") == 1) return LX_PROC_inotify_inst;
    if (seq_(s, "proc/self/maps") == 1) return LX_PROC_self_maps;
    if (seq_(s, "proc/self/status") == 1)   return LX_PROC_status;
    if (seq_(s, "proc/self/stat") == 1)     return LX_PROC_pidstat;
    if (seq_(s, "proc/self/cmdline") == 1)  return LX_PROC_cmdline;
    /* /proc/<pid>/{status,stat,cmdline}：pid 用简单的十进制解析 */
    if (s[0] == 'p' && s[1] == 'r' && s[2] == 'o' && s[3] == 'c' && s[4] == '/') {
        u32 p = 0u;
        const char *q = s + 5u;
        u32 any = 0u;

        while (*q >= '0' && *q <= '9') {
            p = p * 10u + (u32)(*q - '0');
            q++;
            any = 1u;
        }
        if (any != 0u && q[0] == '/') {
            if (seq_(q + 1u, "status") == 1)  { *pid = p; return LX_PROC_pidstatus; }
            if (seq_(q + 1u, "stat") == 1)    { *pid = p; return LX_PROC_pidstat; }
            if (seq_(q + 1u, "cmdline") == 1) { *pid = p; return LX_PROC_pidcmdline; }
        }
    }
    return 0u;
}

/* M58：/proc/<pid> 与 /proc/self 目录本身。node 编码成
 * LX_PROC_PIDDIR_BASE + pid，和 proc 文件 id（1..LX_PROC_mounts）区分开。
 * 没有它，busybox ps/top 列目录后 stat/open 子目录一律 ENOENT。 */
#define LX_PROC_PIDDIR_BASE 0x10000u
/* /proc/net 目录（里面只有 dev），和 pid 目录用不同 node 区段。 */
#define LX_PROC_NETDIR_BASE 0x20000u
/* /proc/self/fd（Crashpad 启动时 opendir 它来枚举自己的 fd）。 */
#define LX_PROC_FDDIR_BASE 0x30000u
/* /proc/self/task：Chromium sandbox 的 thread_helpers 必须 fstat 到它。 */
#define LX_PROC_TASKDIR_BASE 0x40000u

static u32 proc_piddir_(const char *s, u32 *pid)
{
    const char *q;
    u32 p = 0u;
    LxProc *cur;

    if (seq_(s, "proc/self") == 1) {
        cur = lx_cur_proc();
        *pid = (cur != (LxProc *)0) ? cur->pid : 1u;
        return 1u;
    }
    if (s[0] != 'p' || s[1] != 'r' || s[2] != 'o' || s[3] != 'c' || s[4] != '/') {
        return 0u;
    }
    q = s + 5u;
    while (*q >= '0' && *q <= '9') {
        p = p * 10u + (u32)(*q - '0');
        q++;
    }
    if (q == s + 5u || *q != '\0') {
        return 0u;
    }
    *pid = p;
    return 1u;
}

static u32 proc_fddir_(const char *s, u32 *pid)
{
    const char *q;
    u32 p = 0u;
    LxProc *cur;

    if (seq_(s, "proc/self/fd") == 1) {
        cur = lx_cur_proc();
        *pid = (cur != (LxProc *)0) ? cur->pid : 1u;
        return 1u;
    }
    if (s[0] != 'p' || s[1] != 'r' || s[2] != 'o' || s[3] != 'c' || s[4] != '/') {
        return 0u;
    }
    q = s + 5u;
    while (*q >= '0' && *q <= '9') {
        p = p * 10u + (u32)(*q - '0');
        q++;
    }
    if (q == s + 5u || q[0] != '/' || q[1] != 'f' || q[2] != 'd' || q[3] != '\0') {
        return 0u;
    }
    *pid = p;
    return 1u;
}

static u32 proc_taskdir_(const char *s, u32 *pid)
{
    LxProc *cur;
    u32 p = 0u;
    const char *q;

    if (seq_(s, "proc/self/task") == 1) {
        cur = lx_cur_proc();
        *pid = (cur != (LxProc *)0) ? cur->pid : 1u;
        return 1u;
    }
    if (s[0] != 'p' || s[1] != 'r' || s[2] != 'o' || s[3] != 'c' || s[4] != '/') {
        return 0u;
    }
    q = s + 5u;
    while (*q >= '0' && *q <= '9') {
        p = p * 10u + (u32)(*q - '0');
        q++;
    }
    if (q == s + 5u || q[0] != '/' || q[1] != 't' || q[2] != 'a' ||
        q[3] != 's' || q[4] != 'k' || q[5] != '\0') {
        return 0u;
    }
    *pid = p;
    return 1u;
}

static void proc_u32_(u32 v, char *out)
{
    char tmp[12];
    u32 n = 0u;
    u32 i;

    if (v == 0u) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v != 0u) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (i = 0u; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    out[n] = '\0';
}

static u64 fs_openat(u64 dirfd, u64 upath, u64 flags)
{
    char ap[FS_NAME] = {0};
    const char *s;
    int idx;
    u64 fd;

    {
        int rc = at_path_(dirfd, upath, ap, (u64)sizeof ap);

        if (rc != 0) {
            return (u64)0 - (u64)(-rc);
        }
    }
    s = ap;
    if (s[0] == '\0') {                            /* "/" 根目录 */
        fd = fd_alloc();
        if (fd == LX_FD_NONE) {
            return (u64)0 - 24;
        }
        fd_set(fd, LX_FD_ROOT, 1u, LX_FD_FILE, flags);
        return fd;
    }
    if (s[0] == 'p' && s[1] == 'r' && s[2] == 'o' && s[3] == 'c') {
        u32 pid = 0u;
        u32 id = proc_id_(s, &pid);
        char *buf;

        if (id != 0u && proc_gen_(id, pid, &buf) != 0u) {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, (u64)id, 0u, LX_FD_PROC, flags);
            g_fd[fd].pos = 0ull;
            return fd;
        }
        /* /proc 目录本身：列一组条目 */
        if (s[4] == '\0') {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, LX_FD_ROOT, 1u, LX_FD_PROC, flags);
            g_fd[fd].pos = 0ull;
            return fd;
        }
        /* /proc/self/task：Chromium sandbox 检查线程数，fstat 失败直接 FATAL。 */
        if (proc_taskdir_(s, &pid) != 0u) {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, LX_PROC_TASKDIR_BASE + (u64)pid, 1u, LX_FD_PROC, flags);
            g_fd[fd].pos = 0ull;
            return fd;
        }
        /* /proc/self/fd 目录：列当前进程 fd 编号（没有它 Crashpad 直接退出）。 */
        if (proc_fddir_(s, &pid) != 0u) {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, LX_PROC_FDDIR_BASE + (u64)pid, 1u, LX_FD_PROC, flags);
            g_fd[fd].pos = 0ull;
            return fd;
        }
        /* M58：/proc/<pid> 与 /proc/self 作为目录打开（ps/top 会 stat 它们） */
        if (proc_piddir_(s, &pid) != 0u) {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, LX_PROC_PIDDIR_BASE + (u64)pid, 1u, LX_FD_PROC, flags);
            g_fd[fd].pos = 0ull;
            return fd;
        }
        /* /proc/net：ifconfig 会先列这个目录再打开 dev */
        if (seq_(s, "proc/net") == 1) {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, LX_PROC_NETDIR_BASE, 1u, LX_FD_PROC, flags);
            g_fd[fd].pos = 0ull;
            return fd;
        }
        return (u64)0 - LX_ENOENT;
    }
    {
        u64 ptyn = pts_node_(s);

        if (ptyn != ~0ull) {
            u64 node = lx_pty_open_slave(ptyn);

            if ((i64)node < 0) { return node; }
            fd = fd_alloc();
            if (fd == LX_FD_NONE) { return (u64)0 - LX_EMFILE; }
            fd_set(fd, node, 0u, LX_FD_PTY, flags);
            g_fd[fd].pos = 1ull;                   /* pos=1 → slave（fd_set 会清零 pos） */
            return fd;
        }
        u64 dev = dev_node_(s);

        if (dev == LX_DEV_PTMX) {
            u64 node = lx_pty_open_master();

            if ((i64)node < 0) { return node; }
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                lx_pty_close(node, 1u);
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, node, 0u, LX_FD_PTY, flags); /* pos=0 → master */
            return fd;
        }
        if (dev != 0ull) {
            LxDev *d;
            LxOsFile *o;

            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            d = lx_dev_by_id(dev);
            o = lx_dev_open(d);                /* 每 open 一个引用对象 */
            if (o == (LxOsFile *)0) {
                lx_fdtab_release(&g_fd[fd]);
                return (u64)0 - LX_ENOMEM;
            }
            /* node 存 LxOsFile*，dev 存 LxDev id：close/fork/dup 靠这两个字段
             * 找到同一份打开实例并维护引用。 */
            fd_set(fd, (u64)o, 0u, LX_FD_DEV, flags);
            g_fd[fd].dev = (u32)dev;
            return fd;
        }
    }
#if C6OS_VFS
    if (vfs_owns_(s) != 0) {
        char vpath[LOS_FS_PATH_MAX] = {0};
        int vfd = 0;

        vpath[0] = '/';
        (void)os_str_cpy(vpath + 1, s, (u32)(LOS_FS_PATH_MAX - 1));
        if (vfs_is_dir_(vpath) != 0) {
            u64 slot = vfs_dir_slot_(vpath);

            if (slot == 0ull) {
                return (u64)0 - 24ull;
            }
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                g_dpath[(u32)(slot - LX_VDIR_BASE)][0] = '\0';
                return (u64)0 - 24ull;
            }
            fd_set(fd, slot, 1u, LX_FD_VFS, flags);
            return fd;
        }
        vfd = vfs_open_(vpath, flags);
        if (vfd < 0) {
            return vfs_err_(vfd);
        }
        fd = fd_alloc();
        if (fd == LX_FD_NONE) {
            (void)LOS_FsClose((INT32)vfd);
            return (u64)0 - 24ull;
        }
        if ((u32)vfd < (u32)OS_MAX_FILES) {
            (void)os_str_cpy(g_vpath[vfd], vpath, (u32)LOS_FS_PATH_MAX);
        }
        fd_set(fd, (u64)vfd, (u32)vfs_is_dir_(vpath), LX_FD_VFS, flags);
        return fd;
    }
#endif
    idx = fs_find(s);
    if (idx < 0) {
        return (u64)0 - LX_ENOENT;
    }
    fd = fd_alloc();
    if (fd == LX_FD_NONE) {
        return (u64)0 - 24;                      /* EMFILE */
    }
    fd_set(fd, (u64)idx, is_dir(s) ? 1u : 0u, LX_FD_FILE, flags);
    return fd;
}

u64 lx_fs_openat(u64 dirfd, u64 upath, u64 flags, u64 mode)
{
    (void)mode;
    return fs_openat(dirfd, upath, flags);
}

/* 从文件读，不动 fd 位置（pread64、以及 mmap 的文件内容拷贝都用它）。 */
static u64 pread_(u64 fd, u64 dst, u64 len, u64 off, u32 user)
{
    LxFile *f = fd_get(fd);
    u32 idx;
    u64 left;
    u64 n;
    const u8 *src;
    u64 i;

    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
    if (f->is_dir != 0u) {
        return (u64)0 - 21;                      /* EISDIR */
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        INT32 saved_pos = LOS_FsLseek((INT32)f->node, 0, (INT32)LOS_SEEK_CUR);
        int n;

        if (user != 0u && lx_user_ok(dst, len, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        if (saved_pos < 0 ||
            LOS_FsLseek((INT32)f->node, (UINT32)off, (INT32)LOS_SEEK_SET) < 0) {
            return (u64)0 - LX_EINVAL;
        }
        n = (int)LOS_FsRead((INT32)f->node, (VOID *)dst, (UINT32)len);
        (void)LOS_FsLseek((INT32)f->node, saved_pos, (INT32)LOS_SEEK_SET);
        return vfs_err_(n);
    }
#endif
    if (f->kind == LX_FD_PROC) {
        u32 id = (u32)f->node;
        char *b;
        u64 sz;

        if (f->is_dir != 0u || id == 0u || id > LX_PROC_MAX) {
            return (u64)0 - LX_EISDIR;
        }
        b = proc_buf_(id);
        sz = (u64)g_procf[id].size;
        if (off >= sz) {
            return 0ull;
        }
        {
            u64 n = (len < sz - off) ? len : sz - off;
            u64 i;

            if (user != 0u && lx_user_ok(dst, n, C6OS_PROT_W) == 0) {
                return (u64)0 - LX_EFAULT;
            }
            for (i = 0ull; i < n; i++) {
                *(u8 *)(dst + i) = (u8)b[off + i];
            }
            return n;
        }
    }
    if (f->kind == LX_FD_DEV) {
        LxOsFile *o = lx_dev_of(f->node);
        i64 rc;

        if (o == (LxOsFile *)0) {
            return (u64)0 - LX_EBADF;
        }
        if (user != 0u && lx_user_ok(dst, len, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        /* 空 evdev 队列返回 EAGAIN：阻塞设备在这里等，O_NONBLOCK / 内核读
         * （mmap 预拷等）原样透传。 */
        for (;;) {
            rc = lx_dev_read(o, (void *)dst, len);
            if (rc != -(i64)LX_EAGAIN || user == 0u ||
                (f->oflags & (u32)LX_O_NONBLOCK) != 0u) {
                return (u64)rc;
            }
            lx_proc_set_wait(LX_WAIT_PIPE);
            lx_proc_block();
            if (lx_proc_intr() != 0 && lx_proc_restart() == 0) {
                return (u64)0 - LX_EINTR;
            }
        }
    }
    if (f->node == LX_FD_CONSOLE) {
        /* M12：console 节点的读以前恒为 EOF。tty 打开之后它就是真终端——
         * 注意这里**不能**只在 fd 0 上判断：ash 启动时会把 stdin dup 成别的
         * fd 再用（`exec 3<&0` 那类），那些 fd 的 node 同样是 LX_FD_CONSOLE。
         * 只看 fd 号的话它们会拿到 EOF，ash 认为输入结束直接 exit_group(0)。 */
        if (lx_tty_enabled() != 0u) {
            return lx_tty_read(dst, len, lx_proc_yield);
        }
        return 0ull;                             /* 还没有键盘输入：EOF */
    }
    idx = (u32)f->node;
    if (off >= g_node[idx].size) {
        return 0ull;
    }
    left = g_node[idx].size - off;
    n = (len < left) ? len : left;
    if (n == 0ull) {
        return 0ull;
    }
    if (user != 0u && lx_user_ok(dst, n, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    src = node_data_(idx) + off;
    for (i = 0; i < n; i++) {
        *(u8 *)(dst + i) = src[i];
    }
    return n;
}

u64 lx_fs_pread(u64 fd, u64 ubuf, u64 len, u64 off)
{
    return pread_(fd, ubuf, len, off, 1u);
}

/* pwrite(2)（nr 18）：在指定偏移写，绝不改动 fd 的 pos。
 * 复用 lx_fs_write 的位置写入逻辑：临时把 pos 设为 off，写完恢复。 */
u64 lx_fs_pwrite(u64 fd, u64 ubuf, u64 len, u64 off)
{
    LxFile *f = fd_get(fd);
    u64 saved;
    u64 n;
#if C6OS_VFS
    INT32 vpos = 0;
    int have_vpos = 0;

    if (f != 0 && f->kind == LX_FD_VFS && f->node < LX_VDIR_BASE) {
        vpos = LOS_FsLseek((INT32)f->node, 0, (INT32)LOS_SEEK_CUR);
        have_vpos = 1;
    }
#endif
    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
    saved = f->pos;
    f->pos = off;
    n = lx_fs_write(fd, ubuf, len);
    f->pos = saved;
#if C6OS_VFS
    if (have_vpos != 0) {
        (void)LOS_FsLseek((INT32)f->node, vpos, (INT32)LOS_SEEK_SET);
    }
#endif
    return n;
}

/* 向量化位置 I/O：preadv(327)/pwritev(328)。
 * iovec 是 16B 的 {base@0, len@8}，cnt 上限与 readv/writev 一致（1024）。
 * 每段的位置由调用者传入的 off 累加推进，fd 的 pos 保持不变。 */
u64 lx_fs_preadv(u64 fd, u64 uiov, u64 cnt, u64 off)
{
    u64 total = 0ull;
    u64 i;

    if (cnt > 1024ull) {
        return (u64)0 - LX_EINVAL;
    }
    if (cnt != 0ull && lx_user_ok(uiov, cnt * 16ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    for (i = 0ull; i < cnt; i++) {
        u64 base = *(const u64 *)(uiov + i * 16ull);
        u64 len = *(const u64 *)(uiov + i * 16ull + 8ull);
        u64 r = lx_fs_pread(fd, base, len, off + total);

        if ((i64)r < 0) {
            return (total != 0ull) ? total : r;
        }
        total += r;
        if (r < len) {
            break;
        }
    }
    return total;
}

u64 lx_fs_pwritev(u64 fd, u64 uiov, u64 cnt, u64 off)
{
    u64 total = 0ull;
    u64 i;

    if (cnt > 1024ull) {
        return (u64)0 - LX_EINVAL;
    }
    if (cnt != 0ull && lx_user_ok(uiov, cnt * 16ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    for (i = 0ull; i < cnt; i++) {
        u64 base = *(const u64 *)(uiov + i * 16ull);
        u64 len = *(const u64 *)(uiov + i * 16ull + 8ull);
        u64 r = lx_fs_pwrite(fd, base, len, off + total);

        if ((i64)r < 0) {
            return (total != 0ull) ? total : r;
        }
        total += r;
        if (r < len) {
            break;
        }
    }
    return total;
}

/* 内核目标（dmap 里的物理页）：不查用户权限。文件映射用它把内容先灌进页框，
 * 再挂页表——避免"先挂页表再写用户 VA"踩到 TLB 别名。 */
u64 lx_fs_pread_k(u64 fd, u64 kdst, u64 len, u64 off)
{
    return pread_(fd, kdst, len, off, 0u);
}

u64 lx_fs_read(u64 fd, u64 ubuf, u64 len)
{
    LxFile *f = fd_get(fd);
    u64 n;

    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
    n = lx_fs_pread(fd, ubuf, len, f->pos);
    if ((i64)n > 0) {
        f->pos += n;
    }
    return n;
}

u64 lx_fs_write(u64 fd, u64 ubuf, u64 len)
{
    LxFile *f = fd_get(fd);

    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        int n;

        if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        if (LOS_FsLseek((INT32)f->node, (UINT32)f->pos, (INT32)LOS_SEEK_SET) < 0) {
            return (u64)0 - LX_EINVAL;
        }
        n = (int)LOS_FsWrite((INT32)f->node, (const VOID *)ubuf, (UINT32)len);
        if (n > 0) {
            f->pos += (u64)n;
        }
        return vfs_err_(n);
    }
#endif
    if (f->kind == LX_FD_DEV) {
        LxOsFile *o = lx_dev_of(f->node);

        if (o == (LxOsFile *)0) {
            return (u64)0 - LX_EBADF;
        }
        if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        return (u64)lx_dev_write(o, (const void *)ubuf, len);
    }
    if (f->kind == LX_FD_FILE && f->node < (u64)g_nodes &&
        g_node[(u32)f->node].writable != 0u) {
        u32 idx = (u32)f->node;
        u64 at = f->pos;
        u64 end;
        u64 k;

        if ((f->oflags & LX_O_ACCMODE) == LX_O_RDONLY) {
            return (u64)0 - LX_EBADF;
        }
        if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        if ((f->oflags & LX_O_APPEND) != 0u) {
            at = g_node[idx].size;
        }
        end = at + len;
        if (end < at) {
            return (u64)0 - LX_EFBIG;
        }
        if ((g_node[idx].seals & 0x0008u) != 0u) {       /* F_SEAL_WRITE */
            return (u64)0 - LX_EPERM;
        }
        if ((g_node[idx].seals & 0x0004u) != 0u &&
            end > g_node[idx].size) {                    /* F_SEAL_GROW */
            return (u64)0 - LX_EPERM;
        }
        if (node_ram_grow_(idx, end) != 0) {
            return (u64)0 - LX_ENOMEM;
        }
        for (k = 0ull; k < len; k++) {
            g_node[idx].ram_data[at + k] = *(const u8 *)(ubuf + k);
        }
        if (end > g_node[idx].size) {
            g_node[idx].size = end;
        }
        f->pos = end;
        return len;
    }
    return (u64)0 - LX_EBADF;
}

/* 内核缓冲写（copy_file_range 的中转页在内核堆，不能过用户范围检查）。
 * 支持 VFS 文件与 /dev，与 lx_fs_write 的分支保持一致。 */
u64 lx_fs_write_k(u64 fd, const u8 *src, u64 len)
{
    LxFile *f = fd_get(fd);

    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        int n;

        if (LOS_FsLseek((INT32)f->node, (UINT32)f->pos, (INT32)LOS_SEEK_SET) < 0) {
            return (u64)0 - LX_EINVAL;
        }
        n = (int)LOS_FsWrite((INT32)f->node, (const VOID *)src, (UINT32)len);
        if (n > 0) {
            f->pos += (u64)n;
        }
        return vfs_err_(n);
    }
#endif
    if (f->kind == LX_FD_DEV) {
        LxOsFile *o = lx_dev_of(f->node);

        if (o == (LxOsFile *)0) {
            return (u64)0 - LX_EBADF;
        }
        return (u64)lx_dev_write(o, (const void *)src, len);
    }
    return (u64)0 - LX_EBADF;
}

/* copy_file_range(2)（nr 326）：GNU cp 等用它做内核态文件拷贝。
 * offset 指针非 NULL 时从该偏移读/写并回写新偏移（且不改 fd 的 pos）；
 * 为 NULL 时用 fd 当前位置并推进。用 4KB 内核中转缓冲，跨 VFS/ramfs。 */
u64 lx_fs_copy_file_range(u64 fd_in, u64 uoff_in, u64 fd_out,
                          u64 uoff_out, u64 len, u64 flags)
{
    u8 *tmp;
    u64 in_off = 0ull, out_off = 0ull;
    u64 done = 0ull;
    u64 saved_out_pos;
    int have_in = (uoff_in != 0ull), have_out = (uoff_out != 0ull);
    LxFile *fi, *fo;

    if (flags != 0ull) {
        return (u64)0 - LX_EINVAL;
    }
    fi = fd_get(fd_in);
    fo = fd_get(fd_out);
    if (fi == 0 || fo == 0) {
        return (u64)0 - LX_EBADF;
    }
    saved_out_pos = fo->pos;
    if (have_in && lx_user_ok(uoff_in, 8ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    if (have_out && lx_user_ok(uoff_out, 8ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    if (have_in) { in_off = *(const u64 *)uoff_in; }
    else { in_off = fi->pos; }
    if (have_out) { out_off = *(const u64 *)uoff_out; }
    else { out_off = fo->pos; }
    tmp = (u8 *)port_kcalloc(1ull, 4096ull);
    if (tmp == (u8 *)0) {
        return (u64)0 - LX_ENOMEM;
    }
    while (done < len) {
        u64 chunk = (len - done < 4096ull) ? len - done : 4096ull;
        u64 nr = lx_fs_pread_k(fd_in, (u64)tmp, chunk, in_off);
        u64 nw;

        if ((i64)nr <= 0) { break; }
        /* fs_write_k_ 按 fd 当前 pos 写：显式 out_off 时先把 pos 对齐到它 */
        fo->pos = out_off;
        nw = lx_fs_write_k(fd_out, tmp, nr);
        if ((i64)nw <= 0) {
            if (done == 0ull) { done = nw; }
            break;
        }
        done += nw;
        in_off += nw;
        out_off += nw;
        fo->pos = out_off;
        if (nw < nr) { break; }
    }
    port_kfree((void *)tmp);
    /* 显式 offset 语义：调用者的 pos 不应被改动 */
    if (!have_in) { fi->pos = in_off; }
    else if (lx_user_ok(uoff_in, 8ull, C6OS_PROT_W) != 0) { *(u64 *)uoff_in = in_off; }
    if (!have_out) { fo->pos = out_off; }
    else {
        *(u64 *)uoff_out = out_off;
        fo->pos = saved_out_pos;
    }
    return done;
}

u64 lx_fs_fd_size(u64 fd)
{
    LxFile *f = fd_get(fd);

    if (f != 0 && f->kind == LX_FD_PROC && f->is_dir == 0u) {
        return (u64)g_procf[(u32)f->node].size;
    }
#if C6OS_VFS
    if (f != 0 && f->kind == LX_FD_VFS) {
        INT32 sz = 0;

        if (f->node >= LX_VDIR_BASE) {
            return 0ull;                         /* 目录：大小当 0 */
        }
        sz = LOS_FsLseek((INT32)f->node, 0, (INT32)LOS_SEEK_END);
        return (sz < 0) ? 0ull : (u64)sz;
    }
#endif
    if (f == 0 || f->node >= (u64)g_nodes) {
        return 0ull;
    }
    return g_node[(u32)f->node].size;
}

/* ---- 内核侧路径查询（给 ELF 加载器用） ---- */

u64 lx_fs_size_of(const char *path)
{
    int i = fs_find(strip_(path));

    return (i < 0) ? 0ull : g_node[(u32)i].size;
}

u64 lx_fs_pa_of(const char *path)
{
    int i = fs_find(strip_(path));

    return (i < 0) ? 0ull : g_fs_pa + g_node[(u32)i].off;
}

/* cd：路径可以是相对的（相对当前 cwd）。目录不存在或不是目录就报错。 */
u64 lx_fs_chdir(u64 upath)
{
    char path[FS_NAME] = {0};
    char ap[FS_NAME] = {0};
    u64 i;
    int idx;

    if (upath == 0ull || lx_user_ok(upath, 1ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    for (i = 0ull; i + 1ull < (u64)sizeof(path); i++) {
        u8 c;

        if (lx_user_ok(upath + i, 1ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        c = *(const u8 *)(upath + i);
        path[i] = (char)c;
        if (c == 0u) {
            break;
        }
    }
    if (path[0] == '\0') {
        return (u64)0 - LX_ENOENT;
    }
    if (path_(path, ap, (u64)sizeof ap) != 0) {
        return (u64)0 - LX_ENOENT;
    }
    if (ap[0] == '\0') {                     /* 根 */
        g_cwd[0] = '\0';
        return 0ull;
    }
    idx = fs_find(ap);
    if (idx < 0) {
#if C6OS_VFS
        /* 挂载点（/tmp 的 ramfs、/data 的 fatfs）不在 initramfs 的扁平表里。
         * stat/open/access 早就先查挂载表了，只有 chdir 还死盯 fs_find，
         * 于是 `cd /data` 报 "No such file or directory"。这里按同一张挂载表
         * 解析一次，是目录就接受（cwd 存相对路径，相对 open 自然落到挂载点下）。 */
        if (vfs_owns_(ap) != 0) {
            char vpath[LOS_FS_PATH_MAX] = {0};
            LosFsStat st = {0};
            u64 k = 0ull;

            vpath[0] = '/';
            while (k + 2ull < (u64)sizeof(vpath) && ap[k] != '\0') {
                vpath[k + 1ull] = ap[k];
                k++;
            }
            vpath[k + 1ull] = '\0';
            if (LOS_FsStat((const CHAR *)&vpath[0], &st) != 0) {
                return (u64)0 - LX_ENOENT;
            }
            if (st.type != (UINT32)LOS_FS_TYPE_DIR) {
                return (u64)0 - LX_ENOTDIR;
            }
            for (i = 0ull; i + 1ull < (u64)sizeof(g_cwd); i++) {
                g_cwd[i] = ap[i];
                if (ap[i] == '\0') {
                    break;
                }
            }
            g_cwd[sizeof(g_cwd) - 1u] = '\0';
            return 0ull;
        }
#endif
        return (u64)0 - LX_ENOENT;
    }
    if ((g_node[(u32)idx].mode & 0xF000u) != 0x4000u) {
        return (u64)0 - LX_ENOENT;
    }
    for (i = 0ull; i + 1ull < (u64)sizeof(g_cwd); i++) {
        g_cwd[i] = g_node[(u32)idx].name[i];
        if (g_cwd[i] == '\0') {
            break;
        }
    }
    g_cwd[sizeof(g_cwd) - 1u] = '\0';
    return 0ull;
}

const char *lx_fs_cwd(void)
{
    return (const char *)g_cwd;
}

/* 切进程时换 cwd（proc.c 在换 fd 表的同一个地方调） */
void lx_fs_set_cwd(char *p)
{
    g_cwd = (p != (char *)0) ? p : g_cwd_root;
}

/* 复制一个 fd 槽的“文件描述符引用”：dup2/fork 共享同一个后端句柄，
 * 每个副本各持一份引用，最后一个 close 才真正关。 */
void lx_fs_ref_ent(const LxFile *f)
{
    if (f == (const LxFile *)0 || f->kind != LX_FD_VFS) {
        return;
    }
#if C6OS_VFS
    if (f->node < LX_VDIR_BASE) {
        os_vfs_file_dup((int)f->node);
    }
#endif
}

/* fork 之后给新表里每个 VFS fd 加引用（子进程退出时只放自己那一份）。 */
void lx_fs_ref_fdtab(LxFdTab *t)
{
    u32 i;

    if (t == (LxFdTab *)0) {
        return;
    }
    for (i = 0u; i < t->cap; i++) {
        lx_fs_ref_ent(&t->ent[i]);
    }
}

/* VFS 文件的释放，按 **fd 槽** 而不是 fd 号：进程退出时 fd 表整个销毁
 * （linux/fdt.c 的 free hook 拿到的就是槽），没有"当前进程的 fd 号"可用。
 * close / dup2 覆盖 / 进程退出三条路径共用这一份回收逻辑，FAT_MAX_FILES
 * 句柄才不会随子进程退出泄漏（F 段跑到几十条就 "Out of memory"）。 */
void lx_fs_release_vfs(LxFile *f)
{
    if (f == (LxFile *)0 || f->kind != LX_FD_VFS) {
        return;
    }
#if C6OS_VFS
    if (f->node >= LX_VDIR_BASE) {
        u32 sl = (u32)(f->node - LX_VDIR_BASE);

        if (sl < (u32)LX_MAX_VDIR) {
            g_dpath[sl][0] = '\0';
        }
    } else {
        (void)LOS_FsClose((INT32)f->node);
        if (f->node < (u64)OS_MAX_FILES) {
            g_vpath[(u32)f->node][0] = '\0';
        }
    }
#endif
    f->node = LX_FD_NONE;
    f->pos = 0ull;
    f->is_dir = 0u;
    f->kind = LX_FD_FILE;
}

u64 lx_fs_close(u64 fd)
{
    LxFile *f = fd_get(fd);

    if (f == 0 || fd <= 2ull) {
        return (u64)0 - LX_EBADF;
    }
    if (f->kind == LX_FD_VFS) {
        lx_fs_release_vfs(f);
        return 0ull;
    }
    if (f->kind == LX_FD_DEV) {
        lx_dev_release(lx_dev_of(f->node));
        f->node = LX_FD_NONE;
        f->pos = 0ull;
        f->is_dir = 0u;
        f->kind = LX_FD_FILE;
        f->dev = 0u;
        return 0ull;
    }
    f->node = LX_FD_NONE;
    f->pos = 0ull;
    f->is_dir = 0u;
    f->kind = LX_FD_FILE;
    f->dev = 0u;
    return 0ull;
}

u64 lx_fs_lseek(u64 fd, u64 off, u64 whence)
{
    LxFile *f = fd_get(fd);
    u64 base;
    u64 np;

    if (f == 0 || f->node == LX_FD_CONSOLE) {
        return (u64)0 - LX_EBADF;
    }
    /* 管道/fifo 不可定位：Linux 要求 lseek 返回 ESPIPE。以前把管道 fd 的
     * node（小整数管道 id）当 g_node[] 下标，返回伪造的 success，busybox/
     * musl 的 cat 探测 stdin 时据此走错路径，动态 exec 的管道输入静默丢失。 */
    if (f->kind == LX_FD_PIPE_R || f->kind == LX_FD_PIPE_W) {
        return (u64)0 - LX_ESPIPE;
    }
    if (f->kind == LX_FD_DEV) {
        return (u64)0 - LX_ESPIPE;         /* 字符设备不可定位（Linux 同） */
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        INT32 np;
        i64 base = 0;
        i64 target;

        if (f->node >= LX_VDIR_BASE) {
            if (whence != 0ull) {
                return (u64)0 - LX_EINVAL;
            }
            f->pos = off;
            return off;
        }
        /* LxFile.pos 是权威逻辑游标：pread/pwrite/copy_file_range 等位置
         * I/O 刻意不动底层 LxOsFile.pos，若这里拿底层 pos 作 SEEK_CUR 基准
         * 就会读回过期值。统一在 Linux 层算目标，再用 SEEK_SET 同步后端。 */
        if (whence == 0ull) {
            base = 0;
        } else if (whence == 1ull) {
            base = (i64)f->pos;
        } else if (whence == 2ull) {
            base = (i64)lx_fs_fd_size(fd);
        } else {
            return (u64)0 - LX_EINVAL;
        }
        target = base + (i64)off;
        if (target < 0) {
            return (u64)0 - LX_EINVAL;
        }
        np = LOS_FsLseek((INT32)f->node, (INT32)target, (INT32)LOS_SEEK_SET);
        if (np < 0) {
            return vfs_err_(np);
        }
        f->pos = (u64)(u32)np;
        return (u64)(u32)np;
    }
#endif
    if (f->node == LX_FD_ROOT) {
        return 0ull;
    }
    if (whence == 0ull) {
        base = 0ull;
    } else if (whence == 1ull) {
        base = f->pos;
    } else if (whence == 2ull) {
        base = g_node[(u32)f->node].size;
    } else {
        return (u64)0 - LX_EINVAL;
    }
    np = base + off;
    if (np > g_node[(u32)f->node].size) {
        np = g_node[(u32)f->node].size;
    }
    f->pos = np;
    return np;
}

u64 lx_fs_fstat(u64 fd, u64 ustat)
{
    LxFile *f = fd_get(fd);
    u32 idx;

    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        u64 sz = lx_fs_fd_size(fd);
        u32 mode = (f->is_dir != 0u) ? 0040755u : 0100644u;

        if (fill_stat(ustat, (u32)f->node + 2u, sz, mode) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
#endif
    if (f->kind == LX_FD_PROC) {
        u32 mode = (f->is_dir != 0u) ? 0040755u : 0100644u;
        u64 sz = 0ull;

        /* 目录 fd 的 node 是 LX_FD_ROOT 哨兵，不是 proc id —— 别拿它索引表。 */
        if (f->is_dir == 0u && (u32)f->node <= LX_PROC_MAX) {
            sz = (u64)g_procf[(u32)f->node].size;
        }
        if (fill_stat(ustat, 0u, sz, mode) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    if (f->node == LX_FD_CONSOLE) {
        if (fill_stat(ustat, 0u, 0ull, 0020620u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    if (f->node == LX_FD_ROOT) {
        if (fill_stat(ustat, 0u, 0ull, 0040755u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    /* 管道 fd 的 node 是小整数 id，不是 initramfs 节点号。必须当作 FIFO
     * 上报，否则 busybox/musl 的 cat 会 fstat(0) 看到普通文件 size=0 而
     * 直接不读 —— 外部命令管道（如 `echo x | cat`）静默丢数据。 */
    if (f->kind == LX_FD_PIPE_R || f->kind == LX_FD_PIPE_W) {
        if (fill_stat(ustat, 0u, 0ull, 0010600u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    /* G2：设备 fd 的 node 是 LxOsFile 指针，绝不能拿去索引 g_node[]。 */
    if (f->kind == LX_FD_DEV) {
        if (fill_stat(ustat, 0u, 0ull, 0020600u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    idx = (u32)f->node;
    if (fill_stat(ustat, idx, g_node[idx].size, g_node[idx].mode) != 0) {
        return (u64)0 - LX_EFAULT;
    }
    return 0ull;
}

u64 lx_fs_statat(u64 dirfd, u64 upath, u64 ustat, u64 flags)
{
    char upath_buf[FS_NAME];
    char ap[FS_NAME] = {0};
    const char *s;
    int idx;

    /* AT_EMPTY_PATH 要先看原始 path 串是否为空，不能先经 at_path_ 规范
     * （空串解析成 cwd，就分不出"真的是空"还是"."）。 */
    {
        int rc = get_user_path_(upath, upath_buf, (u64)sizeof upath_buf);

        if (rc != 0) {
            return (u64)0 - (u64)(-rc);
        }
    }
    if (upath_buf[0] == '\0') {
        if ((flags & LX_AT_EMPTY_PATH) != 0ull && (i64)dirfd >= 0) {
            return lx_fs_fstat(dirfd, ustat);
        }
        if (fill_stat(ustat, 0u, 0ull, 0040755u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    {
        int rc = at_path_(dirfd, upath, ap, (u64)sizeof ap);

        if (rc != 0) {
            return (u64)0 - (u64)(-rc);
        }
    }
    s = ap;
    if (s[0] == '\0') {                    /* "/" 根目录（AT_EMPTY_PATH 已在上面处理） */
        if (fill_stat(ustat, 0u, 0ull, 0040755u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
    if (s[0] == 'p' && s[1] == 'r' && s[2] == 'o' && s[3] == 'c') {
        u32 pid = 0u;
        u32 id = proc_id_(s, &pid);
        char *b;

        if (s[4] == '\0' || proc_piddir_(s, &pid) != 0u ||
            proc_taskdir_(s, &pid) != 0u || proc_fddir_(s, &pid) != 0u ||
            seq_(s, "proc/net") == 1) {
            return (fill_stat(ustat, 0u, 0ull, 0040755u) != 0) ?
                   (u64)0 - LX_EFAULT : 0ull;
        }
        if (id != 0u) {
            u64 sz = (u64)proc_gen_(id, pid, &b);

            return (fill_stat(ustat, 0u, sz, 0100644u) != 0) ?
                   (u64)0 - LX_EFAULT : 0ull;
        }
        return (u64)0 - LX_ENOENT;
    }
#if C6OS_VFS
    /* B8：挂载点（/tmp 的 ramfs、/data 的 fatfs）必须能被 stat 看见。
     * 以前只查 initramfs 的扁平表，挂载点下的文件一律 ENOENT。 */
    if (vfs_owns_(s) != 0) {
        LosFsStat st = {0};
        char vpath[LOS_FS_PATH_MAX] = {0};
        u32 mode = 0100644u;

        vpath[0] = '/';
        (void)os_str_cpy(vpath + 1, s, (u32)(LOS_FS_PATH_MAX - 1));
        if (LOS_FsStat((const CHAR *)vpath, &st) != 0) {
            return (u64)0 - LX_ENOENT;
        }
        if (st.type == (UINT32)LOS_FS_TYPE_DIR) {
            mode = 0040755u;
        }
        return (fill_stat(ustat, 1u, (u64)st.size, mode) != 0) ?
               (u64)0 - LX_EFAULT : 0ull;
    }
#endif
    idx = fs_find(s);
    if (idx < 0) {
        return (u64)0 - LX_ENOENT;
    }
    if (fill_stat(ustat, (u32)idx, g_node[(u32)idx].size, g_node[(u32)idx].mode) != 0) {
        return (u64)0 - LX_EFAULT;
    }
    return 0ull;
}

u64 lx_fs_access(u64 upath, u64 mode)
{
    char ap[FS_NAME] = {0};
    const char *s;
    int rc;

    (void)mode;
    rc = at_path_(LX_AT_FDCWD, upath, ap, (u64)sizeof ap);
    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
    s = ap;
    if (s[0] == '\0') {
        return 0ull;                       /* 根目录 */
    }
#if C6OS_VFS
    if (vfs_owns_(s) != 0) {
        LosFsStat st = {0};
        char vpath[LOS_FS_PATH_MAX] = {0};

        vpath[0] = '/';
        (void)os_str_cpy(vpath + 1, s, (u32)(LOS_FS_PATH_MAX - 1));
        return (LOS_FsStat((const CHAR *)vpath, &st) == 0) ? 0ull
                                                            : (u64)0 - LX_ENOENT;
    }
#endif
    if (fs_find(s) >= 0) {
        return 0ull;
    }
    return (u64)0 - LX_ENOENT;
}

/* M54：faccessat(dirfd, path, mode, flags) —— 与 access 同语义但支持 dirfd。
 * flags 只认 AT_EACCESS(=0x200)/AT_SYMLINK_NOFOLLOW(=0x100)，我们无有效
 * uid/gid 与符号链接概念，接受并忽略；路径解析复用 at_abspath_。 */
u64 lx_fs_faccessat(u64 dirfd, u64 upath, u64 mode, u64 flags)
{
    char ap[FS_NAME] = {0};
    int rc;

    (void)mode;
    (void)flags;
    rc = at_path_(dirfd, upath, ap, (u64)sizeof ap);
    if (rc != 0) { return (u64)0 - (u64)(-rc); }
    if (ap[0] == '\0') { return 0ull; }
#if C6OS_VFS
    {
    char ab[LOS_FS_PATH_MAX] = {0};
    LosFsStat st = {0};

    ab[0] = '/';
    (void)os_str_cpy(ab + 1, ap, (u32)(LOS_FS_PATH_MAX - 1));
    if (vfs_owns_(ap) != 0) {
        return (LOS_FsStat((const CHAR *)ab, &st) == 0) ? 0ull
                                                        : (u64)0 - LX_ENOENT;
    }
    }
#endif
    return (fs_find(ap) >= 0) ? 0ull : (u64)0 - LX_ENOENT;
}

/* M54：fchdir(fd) —— 把 cwd 改到已打开目录 fd。用 dirfd_base_ 取该 fd 的
 * 路径后写回 cwd；非目录 fd 报 ENOTDIR。 */
u64 lx_fs_fchdir(u64 fd)
{
    char base[FS_NAME] = {0};
    int rc;

    rc = dirfd_base_(fd, base, (u64)sizeof base);
    if (rc != 0) { return (u64)0 - (u64)(-rc); }
    {
        u64 n = slen_(base);
        if (n >= (u64)FS_NAME) { return (u64)0 - LX_ENAMETOOLONG; }
        for (u64 i = 0u; i <= n; i++) { g_cwd[i] = base[i]; }
    }
    lx_fs_set_cwd(g_cwd);
    return 0ull;
}

/* ---- M15-1b：写类 syscall ------------------------------------------------
 * 一律转成绝对路径后交给内核 VFS（LOS_Fs*）。内核 errno 与 Linux errno 数值
 * 一致（都是 POSIX），直接透传即可。initramfs 只读：相关操作返回 EROFS。
 * C6OS_VFS=0 的 port（L2–L8 载荷）这些操作全部返回 EROFS，行为确定。 */
static u64 vfs_errno_(int rc)
{
    return (rc < 0) ? ((u64)0 - (u64)(0 - rc)) : 0ull;
}

u64 lx_fs_mkdirat(u64 dirfd, u64 upath, u64 mode)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(dirfd, upath, abspath, (u64)sizeof abspath);

    (void)mode;
    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
#if C6OS_VFS
    return vfs_errno_(LOS_FsMkdir((const CHAR *)abspath));
#else
    (void)abspath;
    return (u64)0 - LX_EROFS;
#endif
}

u64 lx_fs_rmdir(u64 upath)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(LX_AT_FDCWD, upath, abspath, (u64)sizeof abspath);

    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
#if C6OS_VFS
    return vfs_errno_(LOS_FsRmdir((const CHAR *)abspath));
#else
    (void)abspath;
    return (u64)0 - LX_EROFS;
#endif
}

u64 lx_fs_unlinkat(u64 dirfd, u64 upath, u64 flags)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(dirfd, upath, abspath, (u64)sizeof abspath);

    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
    if ((flags & 0x200ull /* AT_REMOVEDIR */) != 0ull) {
#if C6OS_VFS
        return vfs_errno_(LOS_FsRmdir((const CHAR *)abspath));
#else
        return (u64)0 - LX_EROFS;
#endif
    }
#if C6OS_VFS
    return vfs_errno_(LOS_FsUnlink((const CHAR *)abspath));
#else
    return (u64)0 - LX_EROFS;
#endif
}

u64 lx_fs_renameat(u64 odirfd, u64 oldpath, u64 ndirfd, u64 newpath)
{
    char from[LOS_FS_PATH_MAX] = {0};
    char to[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(odirfd, oldpath, from, (u64)sizeof from);

    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
    rc = at_abspath_(ndirfd, newpath, to, (u64)sizeof to);
    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
#if C6OS_VFS
    return vfs_errno_(LOS_FsRename((const CHAR *)from, (const CHAR *)to));
#else
    (void)from; (void)to;
    return (u64)0 - LX_EROFS;
#endif
}

u64 lx_fs_truncate(u64 upath, u64 len)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(LX_AT_FDCWD, upath, abspath, (u64)sizeof abspath);

    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
#if C6OS_VFS
    return vfs_errno_(LOS_FsTruncate((const CHAR *)abspath, (UINT32)len));
#else
    (void)abspath; (void)len;
    return (u64)0 - LX_EROFS;
#endif
}

u64 lx_fs_ftruncate(u64 fd, u64 len)
{
    LxFile *f = fd_get(fd);
    char abspath[LOS_FS_PATH_MAX] = {0};

    if (f == (LxFile *)0 || fd <= 2ull) {
        return (u64)0 - LX_EBADF;
    }
    if (f->kind == LX_FD_FILE && f->node < (u64)g_nodes &&
        g_node[(u32)f->node].writable != 0u) {
        u32 idx = (u32)f->node;

        if ((g_node[idx].seals & 0x0002u) != 0u && len < g_node[idx].size) {
            return (u64)0 - LX_EPERM;                  /* F_SEAL_SHRINK */
        }
        if ((g_node[idx].seals & 0x0004u) != 0u && len > g_node[idx].size) {
            return (u64)0 - LX_EPERM;                  /* F_SEAL_GROW */
        }
        if (node_ram_grow_(idx, len) != 0) {
            return (u64)0 - LX_ENOMEM;
        }
        if (len > g_node[idx].size) {
            u64 k;
            for (k = g_node[idx].size; k < len; k++) {
                g_node[idx].ram_data[k] = 0u;
            }
        }
        g_node[idx].size = len;
        return 0ull;
    }
#if C6OS_VFS
    if (f->kind != LX_FD_VFS || f->node >= LX_VDIR_BASE ||
        f->node >= (u64)OS_MAX_FILES || g_vpath[(u32)f->node][0] == '\0') {
        return (u64)0 - LX_EINVAL;         /* initramfs 只读 / 管道不可截断 */
    }
    (void)os_str_cpy(abspath, g_vpath[(u32)f->node], (u32)sizeof abspath);
    /* POSIX：ftruncate 不改文件偏移。LxFile.pos 是权威逻辑游标，这里绝不能
     * 动它；truncate_refresh_ 会按需要夹住底层 LxOsFile.pos。 */
    return vfs_errno_(LOS_FsTruncate((const CHAR *)abspath, (UINT32)len));
#else
    (void)f; (void)len; (void)abspath;
    return (u64)0 - LX_EROFS;
#endif
}

/* fallocate(2)（nr 285）：本内核 VFS（FAT）不支持真正的块预分配，
 * 语义按"文件大小"近似：
 *   - mode=0                 → 把文件扩展到 off+len（等价 ftruncate 到更大）
 *   - FALLOC_FL_KEEP_SIZE    → 预分配但不改大小：本文件系统无预分配概念，
 *                              若 off+len 已不超过当前大小则直接成功；否则
 *                              仍视为成功（后续写入自然扩展），不改 pos。
 *   - FALLOC_FL_PUNCH_HOLE   → EOPNOTSUPP（FAT 无稀疏）
 * 其余 flag 组合 → EINVAL。返回的"成功"与 POSIX 对齐：成功不改变 fd pos。 */
u64 lx_fs_fallocate(u64 fd, u64 mode, u64 off, u64 len)
{
    LxFile *f = fd_get(fd);
    u64 end;

    if (f == (LxFile *)0) {
        return (u64)0 - LX_EBADF;
    }
    if (len == 0ull) {
        return (u64)0 - LX_EINVAL;
    }
    end = off + len;
    if (end < off) {                       /* 64 位溢出 */
        return (u64)0 - LX_EINVAL;
    }
    if ((mode & LX_FALLOC_FL_PUNCH_HOLE) != 0u) {
        return (u64)0 - LX_EOPNOTSUPP;
    }
    if ((mode & ~(u64)LX_FALLOC_FL_KEEP_SIZE) != 0u) {
        return (u64)0 - LX_EINVAL;
    }
    if ((mode & LX_FALLOC_FL_KEEP_SIZE) != 0u) {
        return 0ull;                       /* 无预分配：语义等价 no-op */
    }
    return lx_fs_ftruncate(fd, end);       /* mode=0：扩展文件大小 */
}

static u64 chmod_path_(u64 dirfd, u64 upath, u64 mode)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(dirfd, upath, abspath, (u64)sizeof abspath);

    (void)mode;                            /* 单用户：mode 只有 ro 语义，无权限校验 */
    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
    {
        int idx = fs_find(abspath + 1);

        if (idx >= 0) {
            return 0ull;                   /* initramfs 存在：假装改成功（只读） */
        }
    }
#if C6OS_VFS
    {
        LosFsStat st = {0};
        return (LOS_FsStat((const CHAR *)abspath, &st) == 0) ? 0ull
                                                              : (u64)0 - LX_ENOENT;
    }
#else
    return (u64)0 - LX_ENOENT;
#endif
}

u64 lx_fs_chmod(u64 upath, u64 mode)
{
    return chmod_path_(LX_AT_FDCWD, upath, mode);
}

u64 lx_fs_chmod_pathat(u64 dirfd, u64 upath, u64 mode)
{
    return chmod_path_(dirfd, upath, mode);
}

/* M57：chown 族。单用户内核无有效 uid/gid（fill_stat 恒报 0），接受并忽略
 * uid/gid，只做存在性校验（与 chmod_path_ 同构）。nofollow=1 用不跟随链接
 * 的 fs_find_raw_（lchown / AT_SYMLINK_NOFOLLOW）。 */
static u64 chown_path_(u64 dirfd, u64 upath, u64 uid, u64 gid, u32 nofollow)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(dirfd, upath, abspath, (u64)sizeof abspath);

    (void)uid; (void)gid;                  /* 单用户：无所有权校验 */
    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
    {
        int idx = nofollow ? fs_find_raw(abspath + 1) : fs_find(abspath + 1);

        if (idx >= 0) {
            return 0ull;                   /* 存在：假装改成功 */
        }
    }
#if C6OS_VFS
    {
        LosFsStat st = {0};
        return (LOS_FsStat((const CHAR *)abspath, &st) == 0) ? 0ull
                                                              : (u64)0 - LX_ENOENT;
    }
#else
    return (u64)0 - LX_ENOENT;
#endif
}

u64 lx_fs_chownat(u64 dirfd, u64 upath, u64 uid, u64 gid, u32 nofollow)
{
    return chown_path_(dirfd, upath, uid, gid, nofollow);
}

/* statfs(2)：x86-64 struct statfs 共 120 字节（实测 offsets:
 * type 0/bsize 8/blocks 16/bfree 24/bavail 32/files 40/ffree 48/
 * fsid 56/namelen 64/frsize 72/flags 80/f_spare 88，末尾 32B 保留）。
 * busybox df 读 f_bsize/f_blocks/f_bfree/f_bavail，其余留 0 即可。 */
static u64 statfs_put_(u64 ubuf, u32 bsize, u32 blocks, u32 bfree)
{
    u8 st[120];
    u64 i;

    if (lx_user_ok(ubuf, 120ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    for (i = 0ull; i < 120ull; i++) {
        st[i] = 0u;
    }
    put64(st + 0u, 0x858458f6ull);            /* f_type = RAMFS_MAGIC */
    put64(st + 8u, (u64)bsize);               /* f_bsize */
    put64(st + 16u, (u64)blocks);             /* f_blocks */
    put64(st + 24u, (u64)bfree);              /* f_bfree */
    put64(st + 32u, (u64)bfree);              /* f_bavail */
    put64(st + 64u, 255ull);                  /* f_namelen */
    put64(st + 72u, (u64)bsize);              /* f_frsize */
    for (i = 0ull; i < 120ull; i++) {
        *(u8 *)(ubuf + i) = st[i];
    }
    return 0ull;
}

#if C6OS_VFS
/* 绝对路径（带前导 '/'）→ 对应挂载点的后端统计。找不到挂载点返回 -1。 */
static int statfs_mount_(const char *abspath, u32 *bsize, u32 *blocks,
                         u32 *bfree)
{
    LosFsStatFs fs;
    int rc;

    fs.bsize = 0u;
    fs.blocks = 0u;
    fs.bfree = 0u;
    fs.bavail = 0u;
    fs.files = 0u;
    fs.ffree = 0u;
    rc = LOS_FsStatFs((const CHAR *)abspath, &fs);
    if (rc != 0) {
        return -1;
    }
    *bsize = fs.bsize;
    *blocks = fs.blocks;
    *bfree = fs.bfree;
    return 0;
}
#endif

/* statfs 以前对任何路径都报同一组常量（256MB），df 里 /data 是 8MB 的盘也
 * 显示 256MB。现在挂载点走后端统计（ramfs 报 arena、fatfs 报簇数），/proc
 * 报 0；initramfs 根仍用兜底常量。 */
u64 lx_fs_statfs(u64 upath, u64 ubuf)
{
    u32 bsize = 4096u;
    u32 blocks = 65536u;
    u32 bfree = 32768u;

    if (lx_user_ok(ubuf, 120ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
#if C6OS_VFS
    {
        char ap[FS_NAME] = {0};
        int rc = at_path_(LX_AT_FDCWD, upath, ap, (u64)sizeof ap);

        if (rc != 0) {
            return (u64)0 - (u64)(-rc);
        }
        if (ap[0] != '\0') {
            if (vfs_owns_(ap) != 0) {
                char ab[LOS_FS_PATH_MAX] = {0};
                u64 k = 0ull;

                ab[0] = '/';
                while (k + 2ull < (u64)sizeof(ab) && ap[k] != '\0') {
                    ab[k + 1ull] = ap[k];
                    k++;
                }
                ab[k + 1ull] = '\0';
                if (statfs_mount_(ab, &bsize, &blocks, &bfree) != 0) {
                    bsize = 4096u;
                    blocks = 0u;
                    bfree = 0u;
                }
            } else if (seq_(ap, "proc") == 1 ||
                       os_str_prefix(ap, "proc/") == 1) {
                bsize = 4096u;
                blocks = 0u;
                bfree = 0u;
            }
        }
    }
#else
    (void)upath;
#endif
    return statfs_put_(ubuf, bsize, blocks, bfree);
}

/* fstatfs(fd)：第一个参数是 fd 而不是用户路径。以前和 statfs 共用同一个
 * 忽略路径的实现，能跑纯属巧合；statfs 开始解析路径后必须拆开。 */
u64 lx_fs_fstatfs(u64 fd, u64 ubuf)
{
    u32 bsize = 4096u;
    u32 blocks = 65536u;
    u32 bfree = 32768u;

    if (lx_user_ok(ubuf, 120ull, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
#if C6OS_VFS
    {
        LxFile *f = fd_get(fd);

        if (f == 0) {
            return (u64)0 - LX_EBADF;
        }
        if (f->kind == LX_FD_VFS) {
            const char *p = (const char *)0;

            if (f->node >= LX_VDIR_BASE) {
                u32 slot = (u32)(f->node - LX_VDIR_BASE);

                if (slot < (u32)LX_MAX_VDIR) {
                    p = (const char *)&g_dpath[slot][0];
                }
            } else if (f->node < (u64)OS_MAX_FILES) {
                p = (const char *)&g_vpath[f->node][0];
            }
            if (p == (const char *)0 || p[0] != '/' ||
                statfs_mount_(p, &bsize, &blocks, &bfree) != 0) {
                bsize = 4096u;
                blocks = 0u;
                bfree = 0u;
            }
        } else if (f->kind == LX_FD_PROC) {
            bsize = 4096u;
            blocks = 0u;
            bfree = 0u;
        }
    }
#else
    (void)fd;
#endif
    return statfs_put_(ubuf, bsize, blocks, bfree);
}

/* utimensat(2)：内核 VFS 的 mtime 是预留字段（ramfs/romfs 恒 0），
 * 没有地方存。但 `touch`/`cp -p` 会调它，不能 ENOSYS。语义取"校验路径存在、
 * 时间戳接受但忽略"：path 有效且 times 可读就成功，空 times(=NULL) 表示当前时间
 * （同样忽略）。AT_FDCWD 与 *at 的 dirfd 都按常规解析。 */
u64 lx_fs_utimensat(u64 dirfd, u64 upath, u64 times)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(dirfd, upath, abspath, (u64)sizeof abspath);

    if (rc != 0) {
        return (u64)0 - (u64)(-rc);
    }
    if (times != 0ull && lx_user_ok(times, 32ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    {
        int idx = fs_find(abspath + 1);

        if (idx >= 0) {
            return 0ull;                   /* initramfs 中的路径 */
        }
    }
#if C6OS_VFS
    {
        LosFsStat st = {0};
        return (LOS_FsStat((const CHAR *)abspath, &st) == 0) ? 0ull
                                                              : (u64)0 - LX_ENOENT;
    }
#else
    return (u64)0 - LX_ENOENT;
#endif
}

#if C6OS_VFS
/* /tmp 目录的 getdents64：pos 0/1 是 . 和 ..，之后按 LOS_FsReaddir 的顺序。 */
static u64 getdents_vfs_(LxFile *f, u64 ubuf, u64 len)
{
    u8 rec[512];
    u64 used = 0ull;
    const char *dir = (f->node >= LX_VDIR_BASE)
                          ? g_dpath[(u32)(f->node - LX_VDIR_BASE)]
                          : g_vpath[f->node];

    for (;;) {
        const char *nm = "";
        u32 ino = 1u;
        u32 type = 4u;
        u64 nl = 0ull;
        u64 rl = 0ull;
        u64 i = 0ull;

        if (f->pos == 0ull) {
            nm = ".";
            ino = 1u;
            type = 4u;
        } else if (f->pos == 1ull) {
            nm = "..";
            ino = 1u;
            type = 4u;
        } else {
            LosFsDirent de = {0};

            de.name[0] = '\0';
            de.type = 0u;
            de.size = 0u;
            if (LOS_FsReaddir((const CHAR *)dir, (UINT32)(f->pos - 2ull), &de) != 0) {
                return used;
            }
            nm = (const char *)de.name;
            ino = (u32)(f->pos - 1ull);
            type = (de.type == (UINT32)LOS_FS_TYPE_DIR) ? 4u : 8u;
        }
        nl = slen_(nm);
        rl = (19ull + nl + 1ull + 7ull) & ~7ull;
        if (used + rl > len) {
            if (used == 0ull) {
                return (u64)0 - LX_EINVAL;
            }
            return used;
        }
        for (i = 0; i < rl; i++) {
            rec[i] = 0;
        }
        put64(rec + 0u, (u64)ino);
        put64(rec + 8u, f->pos + 1ull);
        rec[16] = (u8)(rl & 0xffull);
        rec[17] = (u8)((rl >> 8) & 0xffull);
        rec[18] = (u8)type;
        for (i = 0; i < nl; i++) {
            rec[19ull + i] = (u8)nm[i];
        }
        if (lx_user_ok(ubuf + used, rl, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        for (i = 0; i < rl; i++) {
            *(u8 *)(ubuf + used + i) = rec[i];
        }
        used += rl;
        f->pos++;
    }
}
#endif

/* /proc 目录的 getdents64：列出最小集条目（busybox ps/free 只需能列目录）。 */
/* M58：按 pos 算出 /proc 目录的第 pos 个条目名与 d_type。
 * 根 /proc：固定虚拟文件 + self + 活进程 pid（全是 DIR 的 pid 项）。
 * /proc/<pid>：status/stat/cmdline 三个 REG 文件。
 * dtype=0 表示列表结束。 */
static void proc_dent_(const LxFile *f, u64 pos, char *nm, u32 *dtype)
{
    static const char *const root_names[] = {
        "meminfo", "uptime", "cpuinfo", "stat", "mounts", "net", "self"
    };
    static const char *const pid_names[] = { "status", "stat", "cmdline" };
    u32 pids[64];
    u32 n;
    u32 i;

    if (pos == 0ull) {
        nm[0] = '.'; nm[1] = '\0'; *dtype = 4u; return;
    }
    if (pos == 1ull) {
        nm[0] = '.'; nm[1] = '.'; nm[2] = '\0'; *dtype = 4u; return;
    }
    pos -= 2ull;

    if (f->node == LX_FD_ROOT) {
        n = (u32)(sizeof root_names / sizeof root_names[0]);
        if (pos < (u64)n) {
            const char *src = root_names[pos];

            for (i = 0u; src[i] != '\0'; i++) { nm[i] = src[i]; }
            nm[i] = '\0';
            *dtype = (seq_(src, "self") == 1 || seq_(src, "net") == 1) ? 4u : 8u;
            return;
        }
        pos -= (u64)n;
        n = lx_proc_list(pids, 64u);
        if (pos < (u64)n) {
            proc_u32_(pids[pos], nm);
            *dtype = 4u;                    /* pid 项是目录 */
            return;
        }
        *dtype = 0u;
        return;
    }

    /* /proc/self/task：最小实现只列组长 tid。 */
    if (f->node >= LX_PROC_TASKDIR_BASE && f->node < LX_PROC_TASKDIR_BASE + 0x10000u) {
        if (pos == 0ull) {
            proc_u32_((u32)(f->node - LX_PROC_TASKDIR_BASE), nm);
            *dtype = 4u;
        } else {
            *dtype = 0u;
        }
        return;
    }

    /* /proc/self/fd：按当前 fd 表枚举还活着的槽位。 */
    if (f->node >= LX_PROC_FDDIR_BASE && f->node < LX_PROC_FDDIR_BASE + 0x10000u) {
        LxProc *cur = lx_cur_proc();
        u64 seen = 0ull;
        u32 i;

        if (cur == (LxProc *)0 || cur->fd == (LxFdTab *)0) {
            *dtype = 0u;
            return;
        }
        for (i = 0u; i < cur->fd->cap; i++) {
            if (cur->fd->ent[i].node == LX_FD_NONE) {
                continue;
            }
            if (seen == pos) {
                proc_u32_(i, nm);
                *dtype = 8u;
                return;
            }
            seen++;
        }
        *dtype = 0u;
        return;
    }

    /* /proc/net 目录：只有 dev 一个虚拟文件 */
    if (f->node == LX_PROC_NETDIR_BASE) {
        if (pos == 0ull) {
            nm[0] = 'd'; nm[1] = 'e'; nm[2] = 'v'; nm[3] = '\0';
            *dtype = 8u;
        } else {
            *dtype = 0u;
        }
        return;
    }

    n = (u32)(sizeof pid_names / sizeof pid_names[0]);
    if (pos < (u64)n) {
        const char *src = pid_names[pos];

        for (i = 0u; src[i] != '\0'; i++) { nm[i] = src[i]; }
        nm[i] = '\0';
        *dtype = 8u;
        return;
    }
    *dtype = 0u;
}

static u64 getdents_proc_(LxFile *f, u64 ubuf, u64 len)
{
    u64 used = 0ull;

    while (f->pos < 4096ull) {
        char nm[32];
        u32 dtype = 0u;
        u64 nl;
        u64 rl;
        u8 rec[64];
        u64 i;

        proc_dent_(f, f->pos, nm, &dtype);
        if (dtype == 0u) {
            break;                          /* 条目到头 */
        }
        nl = 0ull;
        while (nm[nl] != '\0') { nl++; }
        rl = (19ull + nl + 1ull + 7ull) & ~7ull;
        if (used + rl > len) {
            break;
        }
        for (i = 0ull; i < 64ull; i++) {
            rec[i] = 0u;
        }
        *(u64 *)(rec + 0ull) = 1ull;        /* d_ino */
        *(u64 *)(rec + 8ull) = f->pos + 1ull;  /* d_off */
        rec[16] = (u8)(rl & 0xffull);       /* d_reclen */
        rec[17] = (u8)((rl >> 8) & 0xffull);
        rec[18] = (u8)dtype;                /* d_type: DIR(4)/REG(8) */
        for (i = 0ull; i < nl; i++) {
            rec[19ull + i] = (u8)nm[i];
        }
        if (lx_user_ok(ubuf + used, rl, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        for (i = 0ull; i < rl; i++) {
            *(u8 *)(ubuf + used + i) = rec[i];
        }
        used += rl;
        f->pos++;
    }
    return used;
}

/* getdents64：Linux 的 dirent64 布局，名字以 NUL 结尾，记录 8 字节对齐。 */
u64 lx_fs_getdents64(u64 fd, u64 ubuf, u64 len)
{
    LxFile *f = fd_get(fd);
    u8 rec[512];
    u64 used = 0;
    u32 idx;
    const char *dir;

    if (f == 0) {
        return (u64)0 - LX_EBADF;
    }
    if (f->is_dir == 0u) {
        return (u64)0 - 20;                      /* ENOTDIR */
    }
    if (f->kind == LX_FD_PROC) {
        return getdents_proc_(f, ubuf, len);
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        return getdents_vfs_(f, ubuf, len);
    }
#endif
    dir = (f->node == LX_FD_ROOT) ? "" : fs_node_name((u32)f->node);
    for (;;) {
        const char *nm;
        u32 ino;
        u32 type;
        u64 nl;
        u64 rl;
        u64 i;

        /* pos 是"第几个条目"的游标：0/1 固定是 . 和 ..，之后是表里顺序 */
        if (f->pos == 0ull) {
            nm = ".";
            ino = 1u;
            type = 4u;
        } else if (f->pos == 1ull) {
            nm = "..";
            ino = 1u;
            type = 4u;
        } else {
            u64 want = f->pos - 2ull;
            u64 seen = 0ull;
            u32 k;
            int found = 0;

            ino = 0u;
            type = 0u;
            nm = "";
            for (k = 0; k < g_nodes; k++) {
                if (in_dir(g_node[k].name, dir) == 0) {
                    continue;
                }
                if (seen == want) {
                    nm = g_node[k].name + ((dir[0] == '\0') ? 0ull : slen_(dir) + 1ull);
                    ino = k + 1u;
                    type = ((g_node[k].mode & 0040000u) != 0u) ? 4u : 8u;
                    found = 1;
                    break;
                }
                seen++;
            }
            if (found == 0) {
                return used;
            }
        }
        nl = slen_(nm);
        rl = (19ull + nl + 1ull + 7ull) & ~7ull;
        if (used + rl > len) {
            if (used == 0ull) {
                return (u64)0 - LX_EINVAL;
            }
            return used;
        }
        for (i = 0; i < rl; i++) {
            rec[i] = 0;
        }
        put64(rec + 0u, (u64)ino);
        put64(rec + 8u, f->pos + 1ull);
        rec[16] = (u8)(rl & 0xffull);
        rec[17] = (u8)((rl >> 8) & 0xffull);
        rec[18] = (u8)type;
        for (i = 0; i < nl; i++) {
            rec[19ull + i] = (u8)nm[i];
        }
        if (lx_user_ok(ubuf + used, rl, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        for (i = 0; i < rl; i++) {
            *(u8 *)(ubuf + used + i) = rec[i];
        }
        used += rl;
        f->pos++;
    }
}

/* M56：readlink/readlinkat —— 读取符号链接目标（不跟随）。
 * initramfs 的符号链接目标存在 cpio 数据区（mode=S_IFLNK，数据是目标字符串），
 * 直接用 fs_find_raw_（不跟随）查表后拷贝目标。filename 不补 NUL（Linux 语义），
 * 返回写入的字节数；bufsiz 太小则截断到 bufsiz。 */
u64 lx_fs_readlinkat(u64 dirfd, u64 upath, u64 ubuf, u64 bufsiz)
{
    char ap[FS_NAME];
    int i;
    u64 n, m, k;
    const u8 *src;

    if (at_path_(dirfd, upath, ap, (u64)sizeof ap) != 0) {
        return (u64)0 - LX_ENAMETOOLONG;
    }
    i = fs_find_raw(ap);
    if (i < 0) {
        return (u64)0 - LX_ENOENT;
    }
    m = (u64)g_node[(u32)i].mode;
    if ((m & 0170000ull) != 0120000ull) {
        return (u64)0 - LX_EINVAL;
    }
    n = g_node[(u32)i].size;
    if (n > bufsiz) { n = bufsiz; }
    if (n == 0ull) { return 0ull; }
    if (lx_user_ok(ubuf, n, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    src = node_data_((u32)i);
    for (k = 0ull; k < n; k++) {
        *(u8 *)(ubuf + k) = src[k];
    }
    return n;
}

/* M57：symlink(target, linkpath) / symlinkat。
 * 目标字符串原样存入运行时节点（S_IFLNK），之后 fs_find 跟随、readlinkat
 * 读取都经 node_data_，因此新建的链接立刻对全路径解析可见。 */
u64 lx_fs_symlinkat(u64 oldpath, u64 newdirfd, u64 newpath)
{
    char target[LOS_FS_PATH_MAX] = {0};
    char ap[FS_NAME] = {0};
    int rc;

    rc = get_user_path_(oldpath, target, (u64)sizeof target);
    if (rc != 0) { return (u64)0 - (u64)(-rc); }
    if (at_path_(newdirfd, newpath, ap, (u64)sizeof ap) != 0) {
        return (u64)0 - LX_ENAMETOOLONG;
    }
    if (node_find_rw_(ap) >= 0 || fs_find_raw(ap) >= 0) {
        return (u64)0 - LX_EEXIST;
    }
    {
        u64 n = 0ull;
        while (target[n] != '\0' && n < (u64)sizeof target) { n++; }
        if (node_add_(ap, 0120777u, (const u8 *)target, n) < 0) {
            return (u64)0 - LX_ENOMEM;
        }
    }
    return 0ull;
}

/* M57：link(oldpath, newpath) / linkat。硬链接 = 新增一个指向同一份数据的
 * 目录项。cpio 节点共享 off（数据仍在固件映像），运行时节点共享 ram_data；
 * 这里统一用 ram_data 复制一份（资源不受限），保证 unlink 单个名字不影响
 * 其它名字。nofollow 时 oldpath 用 fs_find_raw。 */
u64 lx_fs_linkat(u64 olddirfd, u64 oldpath, u64 newdirfd, u64 newpath)
{
    char old[FS_NAME] = {0};
    char ap[FS_NAME] = {0};
    int idx;

    if (at_path_(olddirfd, oldpath, old, (u64)sizeof old) != 0) {
        return (u64)0 - LX_ENAMETOOLONG;
    }
    idx = fs_find(old);                 /* 跟随符号链接，链接到最终目标 */
    if (idx < 0) { return (u64)0 - LX_ENOENT; }
    if (at_path_(newdirfd, newpath, ap, (u64)sizeof ap) != 0) {
        return (u64)0 - LX_ENAMETOOLONG;
    }
    if (node_find_rw_(ap) >= 0 || fs_find_raw(ap) >= 0) {
        return (u64)0 - LX_EEXIST;
    }
    if (node_add_(ap, g_node[(u32)idx].mode,
                  node_data_((u32)idx), g_node[(u32)idx].size) < 0) {
        return (u64)0 - LX_ENOMEM;
    }
    return 0ull;
}

/* M57：mknod(path, mode, dev) / mknodat。
 * 本内核无块/字符设备后端（无 LOS_FsMknod），FIFO 也走独立管道对象而非
 * 文件系统节点。这里对"特殊文件"请求给出诚实的 EOPNOTSUPP；普通文件
 * （S_IFREG）等价于 open(O_CREAT|O_EXCL)，交给 VFS 后端创建。 */
u64 lx_fs_mknodat(u64 dirfd, u64 upath, u64 mode, u64 dev)
{
    char abspath[LOS_FS_PATH_MAX] = {0};
    int rc = at_abspath_(dirfd, upath, abspath, (u64)sizeof abspath);

    (void)dev;
    if (rc != 0) { return (u64)0 - (u64)(-rc); }
    if ((mode & 0170000ull) != 0100000ull &&   /* 非 S_IFREG */
        (mode & 0170000ull) != 0ull) {         /* 且显式给了类型 */
        return (u64)0 - LX_EOPNOTSUPP;
    }
#if C6OS_VFS
    {
        int fd = LOS_FsOpen((const CHAR *)abspath,
                            (UINT32)(LOS_O_WRONLY | LOS_O_CREAT | LOS_O_EXCL));
        if (fd < 0) { return (u64)0 - (u64)(-fd); }
        LOS_FsClose(fd);
        return 0ull;
    }
#else
    (void)abspath;
    return (u64)0 - LX_EROFS;
#endif
}
