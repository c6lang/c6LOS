#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"

/* 只读 initramfs（标准 newc cpio，和 Linux 的 initramfs 同格式）+ fd 表 +
 * 文件类系统调用。
 *
 * 为什么是 cpio/newc 而不是自造格式：内核侧解析器只有 60 行，而工具链那边
 * 任何 Linux 都能生成（`find | cpio -o -H newc`），L4 的 ld.so/libc.so 直接
 * 从宿主拷进来即可。目录语义只做只读 + 扁平表，L5 的写路径留给 tmpfs。 */

#define FS_MAX_FILES 64u
#define FS_NAME      64u

typedef struct {
    char name[FS_NAME];
    u64  off;                              /* 数据在 initramfs 物理映像内的偏移 */
    u64  size;
    u32  mode;
} LxNode;

static LxNode g_node[FS_MAX_FILES];
static u32    g_nodes;
static LxFile g_fdt0[C6OS_LINUX_MAX_FD];
static LxFile *g_fd = g_fdt0;
static u64    g_fs_pa;


#if C6OS_VFS
#include "los/los_fs.h"
#include "c6os/vfs.h"

/* M9-4：/tmp 及其子路径交给内核 VFS（挂的是 ramfs）。
 * g_vpath 按 **VFS fd** 索引 —— VFS fd 全局唯一，所以多个进程同时列
 * 各自 /tmp 子目录也不会串。 */
static char g_vpath[OS_MAX_FILES][LOS_FS_PATH_MAX];

/* 目录 fd 不经过 LOS_FsOpen（ramfs 不让 open 目录），只记路径；
 * node 落在 LX_VDIR_BASE.. 这一段，和真实 VFS fd（0..31）不会撞。 */
#define LX_VDIR_BASE 0x100ull
#define LX_MAX_VDIR  8u

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
#endif

static u64 slen_(const char *s)
{
    u64 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
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
#endif
                return 0;                              /* TRAILER!!! */
            }
            if (g_nodes < FS_MAX_FILES) {
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

static int fs_find(const char *path)
{
    u32 i;

    for (i = 0; i < g_nodes; i++) {
        if (seq_(g_node[i].name, path)) {
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
    if (fd >= (u64)C6OS_LINUX_MAX_FD) {
        return 0;
    }
    if (g_fd[fd].node == LX_FD_NONE) {
        return 0;
    }
    return &g_fd[fd];
}

/* L5：fd 表跟着进程走。单进程的 L2/L3/L4 目标继续用默认表 g_fdt0。 */
void lx_fs_set_fdtab(LxFile *t)
{
    g_fd = (t != (LxFile *)0) ? t : g_fdt0;
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

static u64 fd_alloc(void)
{
    u64 i;

    for (i = 3ull; i < (u64)C6OS_LINUX_MAX_FD; i++) {
        if (g_fd[i].node == LX_FD_NONE) {
            return i;
        }
    }
    return LX_FD_NONE;
}

void lx_fs_fd_reset(void)
{
    u64 i;

    for (i = 0; i < (u64)C6OS_LINUX_MAX_FD; i++) {
        g_fd[i].node = LX_FD_NONE;
        g_fd[i].pos = 0ull;
        g_fd[i].is_dir = 0u;
        g_fd[i].kind = LX_FD_FILE;
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

/* ---- 系统调用实现 ---- */

static u64 fs_openat(u64 dirfd, u64 upath, u64 flags)
{
    char path[FS_NAME];
    const char *s;
    int idx;
    u64 fd;

    (void)dirfd;
    (void)flags;
    if (lx_user_ok(upath, 1ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    {
        u64 i = 0;

        for (;;) {
            u8 c;

            if (i + 1ull >= FS_NAME) {
                return (u64)0 - LX_EINVAL;
            }
            if (lx_user_ok(upath + i, 1ull, C6OS_PROT_R) == 0) {
                return (u64)0 - LX_EFAULT;
            }
            c = *(const u8 *)(upath + i);
            path[i] = (char)c;
            if (c == 0u) {
                break;
            }
            i++;
        }
    }
    s = strip_(path);
    if (s[0] == '\0') {                            /* "/" 根目录 */
        fd = fd_alloc();
        if (fd == LX_FD_NONE) {
            return (u64)0 - 24;
        }
        g_fd[fd].node = LX_FD_ROOT;
        g_fd[fd].pos = 0ull;
        g_fd[fd].is_dir = 1u;
        g_fd[fd].kind = LX_FD_FILE;
        return fd;
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
            g_fd[fd].node = slot;
            g_fd[fd].pos = 0ull;
            g_fd[fd].is_dir = 1u;
            g_fd[fd].kind = LX_FD_VFS;
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
        g_fd[fd].node = (u64)vfd;
        g_fd[fd].pos = 0ull;
        g_fd[fd].is_dir = (u32)vfs_is_dir_(vpath);
        g_fd[fd].kind = LX_FD_VFS;
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
    g_fd[fd].node = (u64)idx;
    g_fd[fd].pos = 0ull;
    g_fd[fd].is_dir = is_dir(s) ? 1u : 0u;
    g_fd[fd].kind = LX_FD_FILE;
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
    u64 pa;
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
        int n;

        if (user != 0u && lx_user_ok(dst, len, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        if (LOS_FsLseek((INT32)f->node, (UINT32)off, (INT32)LOS_SEEK_SET) < 0) {
            return (u64)0 - LX_EINVAL;
        }
        n = (int)LOS_FsRead((INT32)f->node, (VOID *)dst, (UINT32)len);
        return vfs_err_(n);
    }
#endif
    if (f->node == LX_FD_CONSOLE) {
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
    pa = g_fs_pa + g_node[idx].off + off;
    src = (const u8 *)port_phys_map(pa);
    for (i = 0; i < n; i++) {
        *(u8 *)(dst + i) = src[i];
    }
    return n;
}

u64 lx_fs_pread(u64 fd, u64 ubuf, u64 len, u64 off)
{
    return pread_(fd, ubuf, len, off, 1u);
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
    return (u64)0 - LX_EBADF;
}

u64 lx_fs_fd_size(u64 fd)
{
    LxFile *f = fd_get(fd);

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

u64 lx_fs_close(u64 fd)
{
    LxFile *f = fd_get(fd);

    if (f == 0 || fd <= 2ull) {
        return (u64)0 - LX_EBADF;
    }
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
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
    }
#endif
    f->node = LX_FD_NONE;
    f->pos = 0ull;
    f->is_dir = 0u;
    f->kind = LX_FD_FILE;
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
#if C6OS_VFS
    if (f->kind == LX_FD_VFS) {
        INT32 wh = (INT32)LOS_SEEK_SET;
        INT32 np;

        if (whence == 1ull) {
            wh = (INT32)LOS_SEEK_CUR;
        } else if (whence == 2ull) {
            wh = (INT32)LOS_SEEK_END;
        } else if (whence != 0ull) {
            return (u64)0 - LX_EINVAL;
        }
        if (f->node >= LX_VDIR_BASE) {
            if (whence != 0ull) {
                return (u64)0 - LX_EINVAL;
            }
            f->pos = off;
            return off;
        }
        np = LOS_FsLseek((INT32)f->node, (INT32)off, wh);
        if (np < 0) {
            return vfs_err_(np);
        }
        f->pos = (u64)(u32)np;                   /* 游标是 fd 的，必须同步 */
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
    idx = (u32)f->node;
    if (fill_stat(ustat, idx, g_node[idx].size, g_node[idx].mode) != 0) {
        return (u64)0 - LX_EFAULT;
    }
    return 0ull;
}

u64 lx_fs_statat(u64 dirfd, u64 upath, u64 ustat, u64 flags)
{
    char path[FS_NAME];
    const char *s;
    int idx;
    u64 i;

    (void)dirfd;
    if (lx_user_ok(upath, 1ull, C6OS_PROT_R) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    for (i = 0;; i++) {
        u8 c;

        if (i + 1ull >= FS_NAME) {
            return (u64)0 - LX_EINVAL;
        }
        if (lx_user_ok(upath + i, 1ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        c = *(const u8 *)(upath + i);
        path[i] = (char)c;
        if (c == 0u) {
            break;
        }
    }
    s = strip_(path);
    if (s[0] == '\0') {
        /* AT_EMPTY_PATH：path 为空时按 dirfd 取文件自己的 stat。
         * glibc 的 fstat 就走这条路；不认它的话所有文件都会拿到根目录那
         * 一套 dev/ino，动态链接器据此判定"这个库已经加载过"，
         * 于是第二个库整个被跳过（busybox 需要 libresolv + libc 时必踩）。 */
        if ((flags & LX_AT_EMPTY_PATH) != 0ull && (i64)dirfd >= 0) {
            return lx_fs_fstat(dirfd, ustat);
        }
        if (fill_stat(ustat, 0u, 0ull, 0040755u) != 0) {
            return (u64)0 - LX_EFAULT;
        }
        return 0ull;
    }
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
    (void)mode;
    {
        char path[FS_NAME];
        const char *s;
        u64 i;

        if (lx_user_ok(upath, 1ull, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        for (i = 0;; i++) {
            u8 c;

            if (i + 1ull >= FS_NAME) {
                return (u64)0 - LX_EINVAL;
            }
            if (lx_user_ok(upath + i, 1ull, C6OS_PROT_R) == 0) {
                return (u64)0 - LX_EFAULT;
            }
            c = *(const u8 *)(upath + i);
            path[i] = (char)c;
            if (c == 0u) {
                break;
            }
        }
        s = strip_(path);
        if (s[0] == '\0' || fs_find(s) >= 0) {
            return 0ull;
        }
    }
    return (u64)0 - LX_ENOENT;
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
