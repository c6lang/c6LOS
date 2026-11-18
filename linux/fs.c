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

/* 全量 Alpine minirootfs 有 518 个 cpio 成员（目录 + 文件 + 符号链接），
 * 原来的 64 只够放一个小镜像；1024 给发行版根文件系统留够余量。
 * LxNode 88 字节，1024 项 = 88KB，全部落在 .bss 里（不占载荷）。 */
#define FS_MAX_FILES 1024u
#define FS_NAME      96u
/* 符号链接最多跟几层。Alpine 的 /etc/ssl1.1/certs -> /etc/ssl/certs 与
 * /etc/ssl/certs 之间互相指向，必须有上限，否则解析会转圈。 */
#define FS_LINK_MAX  8u

typedef struct {
    char name[FS_NAME];
    u64  off;                              /* 数据在 initramfs 物理映像内的偏移 */
    u64  size;
    u32  mode;
} LxNode;

static LxNode g_node[FS_MAX_FILES];
static u32    g_nodes;
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

/* 纯查表，不跟符号链接 */
static int fs_find_raw(const char *path)
{
    u32 i;

    for (i = 0; i < g_nodes; i++) {
        if (seq_(g_node[i].name, path)) {
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
        src = (const u8 *)port_phys_map(g_fs_pa + g_node[(u32)i].off);
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

/* M15-2：把 /dev/xxx 映射到设备节点号（node 字段的特殊值）。
 * 返回 0 表示不是设备路径。设备走 LX_FD_DEV，不占 VFS 的文件槽。 */
static u64 dev_node_(const char *s)
{
    if (seq_(s, "dev/null") == 1) {
        return LX_DEV_NULL;
    }
    if (seq_(s, "dev/zero") == 1) {
        return LX_DEV_ZERO;
    }
    if (seq_(s, "dev/full") == 1) {
        return LX_DEV_FULL;
    }
    if (seq_(s, "dev/random") == 1 || seq_(s, "dev/urandom") == 1) {
        return LX_DEV_URANDOM;
    }
    return 0ull;
}

/* ---- M17-1：procfs 最小集 -------------------------------------------------
 * busybox ps/top/free/df 读的是 /proc/{meminfo,stat,uptime,cpuinfo,
 * self/status,self/stat,self/cmdline,PID/...}。我们用一个"虚拟文件"层：
 * 每个 proc 文件对应一个 id，内容在首次访问时一次性生成到内核堆缓冲，
 * 之后 open/read/pread/stat/getdents 都走同一套 fd 机制（kind=LX_FD_PROC）。
 * 内容常驻（条目数固定、总量 < 4KB），不需要 fd close hook，也不占 VFS 文件槽。 */
#define PROC_BUF_MAX 4096u

typedef struct {
    u32 id;
    u32 size;
    char *buf;
} LxProcFile;

static LxProcFile g_procf[LX_PROC_mounts + 1u];

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

    o = pcat_(b, o, "MemTotal:       24576 kB\nMemFree:        12288 kB\n"
                    "MemAvailable:   16384 kB\nBuffers:            0 kB\n"
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
                    "cpu MHz\t\t: 1000.000\nflags\t\t: fpu tsc\n"
                    "processor\t: 1\nvendor_id\t: c6os\n"
                    "model name\t: c6os x86-64 Linux ABI\n"
                    "cpu MHz\t\t: 1000.000\nflags\t\t: fpu tsc\n");
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

/* /proc/mounts：busybox df -h 用它读已挂载文件系统。给一行根 romfs + tmpfs，
 * 字段对齐 mount(5) 的 6 列格式。 */
static u32 proc_mounts_(char *b)
{
    u32 o = 0u;

    o = pcat_(b, o, "rootfs / romfs rw 0 0\n"
                    "tmpfs /tmp tmpfs rw 0 0\n"
                    "proc /proc proc rw 0 0\n");
    return o;
}

static u32 proc_pidstat_(char *b, u32 pid)
{
    /* /proc/PID/stat: busybox ps 读第 2 字段(comm)和第 4 字段(ppid)、
     * 第 14/15 字段(utime/stime)算 CPU。给个最简合法行。 */
    u32 o = pnum_(b, 0u, pid);

    o = pcat_(b, o, " (busybox) S 0 0 0 0 -1 0 0 0 0 0 0 0 "
                    "10 5 0 0 20 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    return o;
}

static u32 proc_gen_(u32 id, u32 pid, char **out)
{
    char *b;

    if (id == 0u || id > LX_PROC_mounts) {
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
        return (u64)0 - LX_ENOENT;
    }
    {
        u64 dev = dev_node_(s);

        if (dev != 0ull) {
            fd = fd_alloc();
            if (fd == LX_FD_NONE) {
                return (u64)0 - LX_EMFILE;
            }
            fd_set(fd, dev, 0u, LX_FD_DEV, flags);
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
/* M15-2：/dev/* 设备的读。node 是 LX_DEV_* 特殊值。
 * user!=0 时目标在用户空间要先校验可写；user==0 是内核缓冲（pread_k）。 */
static u32 g_dev_seed = 0x13579bdfu;

static u64 dev_read_(u64 node, u64 dst, u64 len, u32 user)
{
    u64 i;

    if (user != 0u && lx_user_ok(dst, len, C6OS_PROT_W) == 0) {
        return (u64)0 - LX_EFAULT;
    }
    if (node == LX_DEV_ZERO) {
        for (i = 0ull; i < len; i++) {
            *(u8 *)(dst + i) = 0u;
        }
        return len;
    }
    if (node == LX_DEV_URANDOM) {
        for (i = 0ull; i < len; i++) {
            g_dev_seed = g_dev_seed * 1103515245u + 12345u;
            *(u8 *)(dst + i) = (u8)((g_dev_seed >> 16) & 0xffu);
        }
        return len;
    }
    /* /dev/null、/dev/full 读都是 EOF */
    return 0ull;
}

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
    if (f->kind == LX_FD_PROC) {
        u32 id = (u32)f->node;
        char *b;
        u64 sz;

        if (f->is_dir != 0u || id == 0u || id > LX_PROC_mounts) {
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
        return dev_read_(f->node, dst, len, user);
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
    if (f->kind == LX_FD_DEV) {
        if (lx_user_ok(ubuf, len, C6OS_PROT_R) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        f->pos += len;
        if (f->node == LX_DEV_FULL) {
            return (u64)0 - LX_ENOSPC;             /* /dev/full：写永远失败 */
        }
        return len;                                /* /dev/null 等：吞掉 */
    }
    return (u64)0 - LX_EBADF;
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
    /* 管道/fifo 不可定位：Linux 要求 lseek 返回 ESPIPE。以前把管道 fd 的
     * node（小整数管道 id）当 g_node[] 下标，返回伪造的 success，busybox/
     * musl 的 cat 探测 stdin 时据此走错路径，动态 exec 的管道输入静默丢失。 */
    if (f->kind == LX_FD_PIPE_R || f->kind == LX_FD_PIPE_W) {
        return (u64)0 - LX_ESPIPE;
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
    if (f->kind == LX_FD_PROC) {
        u32 mode = (f->is_dir != 0u) ? 0040755u : 0100644u;
        u64 sz = 0ull;

        /* 目录 fd 的 node 是 LX_FD_ROOT 哨兵，不是 proc id —— 别拿它索引表。 */
        if (f->is_dir == 0u && (u32)f->node <= LX_PROC_mounts) {
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

        if (s[4] == '\0') {
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
#if C6OS_VFS
    if (f->kind != LX_FD_VFS || f->node >= LX_VDIR_BASE ||
        f->node >= (u64)OS_MAX_FILES || g_vpath[(u32)f->node][0] == '\0') {
        return (u64)0 - LX_EINVAL;         /* initramfs 只读 / 管道不可截断 */
    }
    (void)os_str_cpy(abspath, g_vpath[(u32)f->node], (u32)sizeof abspath);
    f->pos = 0ull;
    return vfs_errno_(LOS_FsTruncate((const CHAR *)abspath, (UINT32)len));
#else
    (void)f; (void)len; (void)abspath;
    return (u64)0 - LX_EROFS;
#endif
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

/* statfs(2)：x86-64 struct statfs 共 120 字节（实测 offsets:
 * type 0/bsize 8/blocks 16/bfree 24/bavail 32/files 40/ffree 48/
 * fsid 56/namelen 64/frsize 72/flags 80/f_spare 88，末尾 32B 保留）。
 * busybox df 读 f_bsize/f_blocks/f_bfree/f_bavail，其余留 0 即可。
 * 报一组合理常量：4KB 块、65536 块（256MB）、空闲一半 —— df 有数显、不除零。 */
u64 lx_fs_statfs(u64 upath, u64 ubuf)
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
    put64(st + 8u, 4096ull);                  /* f_bsize */
    put64(st + 16u, 65536ull);                /* f_blocks */
    put64(st + 24u, 32768ull);                /* f_bfree */
    put64(st + 32u, 32768ull);                /* f_bavail */
    put64(st + 64u, 255ull);                  /* f_namelen */
    put64(st + 72u, 4096ull);                 /* f_frsize */
    (void)upath;
    for (i = 0ull; i < 120ull; i++) {
        *(u8 *)(ubuf + i) = st[i];
    }
    return 0ull;
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
static u64 getdents_proc_(LxFile *f, u64 ubuf, u64 len)
{
    static const char *const names[] = {
        "meminfo", "uptime", "cpuinfo", "stat", "mounts",
        "self", "1", "2", "3", "4"
    };
    u64 used = 0ull;

    while (f->pos < 2ull + (u64)(sizeof names / sizeof names[0])) {
        const char *nm;
        u64 nl;
        u64 rl;
        u8 rec[512];

        if (f->pos == 0ull) {
            nm = ".";
        } else if (f->pos == 1ull) {
            nm = "..";
        } else {
            nm = names[f->pos - 2ull];
        }
        nl = 0ull;
        while (nm[nl] != '\0') { nl++; }
        /* 与 initramfs 版一致：19 字节头 + 名字 + NUL，再 8 字节对齐。 */
        rl = (19ull + nl + 1ull + 7ull) & ~7ull;
        if (used + rl > len) {
            break;
        }
        for (u64 i = 0ull; i < 512ull; i++) {
            rec[i] = 0u;
        }
        *(u64 *)(rec + 0ull) = 1ull;        /* d_ino */
        *(u64 *)(rec + 8ull) = f->pos + 1ull;  /* d_off */
        rec[16] = (u8)(rl & 0xffull);       /* d_reclen */
        rec[17] = (u8)((rl >> 8) & 0xffull);
        rec[18] = (u8)((f->pos <= 1ull || seq_(nm, "self") == 1)
                       ? 4u : 8u);         /* d_type: DIR/REG */
        for (u64 i = 0ull; i < nl; i++) {
            rec[19ull + i] = (u8)nm[i];
        }
        if (lx_user_ok(ubuf + used, rl, C6OS_PROT_W) == 0) {
            return (u64)0 - LX_EFAULT;
        }
        for (u64 i = 0ull; i < rl; i++) {
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
