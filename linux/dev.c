/* G2：设备对象模型（docs/linux-abi-gui-chrome.md §3.1）。
 *
 * 这里只做三件事：设备注册表（LxDev）、每次 open 的引用对象（LxOsFile）、
 * 以及 /dev 下四个老字符设备的简单 ops。真正有状态的块在 linux/fb.c 与
 * linux/evdev.c，它们通过 lx_dev_register() 把 ops 挂进来。
 *
 * 为什么注册表是定长：设备是内核里的固定硬件面（fb/input 各一两个），
 * 不像 fd/VMA 那样随用户程序增长。LX_DEV_MAX 只是保护性上限。 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"
#include "c6os/linux_dev.h"

static LxDev g_dev[LX_DEV_MAX];
static u32 g_ndev;
static u32 g_inited;
static u32 g_seed = 0x13579bdfu;

static int same_(const char *a, const char *b)
{
    u32 i = 0u;

    if (a == (const char *)0 || b == (const char *)0) {
        return 0;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

/* ---- /dev/null、/dev/zero、/dev/full、/dev/{u,}random --------------------
 * 以前这些路径在 linux/fs.c 里按 node 哨兵硬编码；现在走同一套 ops。
 * 语义保持不变：null/full 读 EOF，zero 读零页，random 读 LCG，full 写 ENOSPC。 */
static i64 z_read_(struct LxOsFile *f, void *buf, u64 n)
{
    u8 *p = (u8 *)buf;
    u64 i;

    (void)f;
    for (i = 0ull; i < n; i++) {
        p[i] = 0u;
    }
    return (i64)n;
}

static i64 eof_read_(struct LxOsFile *f, void *buf, u64 n)
{
    (void)f;
    (void)buf;
    (void)n;
    return 0;
}

static i64 rand_read_(struct LxOsFile *f, void *buf, u64 n)
{
    u8 *p = (u8 *)buf;
    u64 i;

    (void)f;
    for (i = 0ull; i < n; i++) {
        g_seed = g_seed * 1103515245u + 12345u;
        p[i] = (u8)((g_seed >> 16) & 0xffu);
    }
    return (i64)n;
}

static i64 ok_write_(struct LxOsFile *f, const void *buf, u64 n)
{
    (void)f;
    (void)buf;
    return (i64)n;
}

static i64 full_write_(struct LxOsFile *f, const void *buf, u64 n)
{
    (void)f;
    (void)buf;
    (void)n;
    return -(i64)LX_ENOSPC;
}

static const OsFileOps g_null_ops = {
    eof_read_, ok_write_, (u16 (*)(LxOsFile *, u16))0,
    (i64 (*)(LxOsFile *, u32, u64))0, (i64 (*)(LxOsFile *, struct LxVma *))0,
    (void (*)(LxOsFile *))0,
};

static const OsFileOps g_zero_ops = {
    z_read_, ok_write_, (u16 (*)(LxOsFile *, u16))0,
    (i64 (*)(LxOsFile *, u32, u64))0, (i64 (*)(LxOsFile *, struct LxVma *))0,
    (void (*)(LxOsFile *))0,
};

static const OsFileOps g_full_ops = {
    eof_read_, full_write_, (u16 (*)(LxOsFile *, u16))0,
    (i64 (*)(LxOsFile *, u32, u64))0, (i64 (*)(LxOsFile *, struct LxVma *))0,
    (void (*)(LxOsFile *))0,
};

static const OsFileOps g_rand_ops = {
    rand_read_, ok_write_, (u16 (*)(LxOsFile *, u16))0,
    (i64 (*)(LxOsFile *, u32, u64))0, (i64 (*)(LxOsFile *, struct LxVma *))0,
    (void (*)(LxOsFile *))0,
};

void lx_dev_init(void)
{
    if (g_inited != 0u) {
        return;
    }
    g_inited = 1u;
    (void)lx_dev_register("dev/null", &g_null_ops, (void *)0);
    (void)lx_dev_register("dev/zero", &g_zero_ops, (void *)0);
    (void)lx_dev_register("dev/full", &g_full_ops, (void *)0);
    (void)lx_dev_register("dev/random", &g_rand_ops, (void *)0);
    (void)lx_dev_register("dev/urandom", &g_rand_ops, (void *)0);
    lx_fb_init();
    lx_evdev_init();
}

LxDev *lx_dev_register(const char *name, const OsFileOps *ops, void *priv)
{
    LxDev *d;

    if (g_ndev >= LX_DEV_MAX || name == (const char *)0) {
        return (LxDev *)0;
    }
    d = &g_dev[g_ndev];
    d->name = name;
    d->ops = ops;
    d->priv = priv;
    d->refs = 0u;
    g_ndev++;
    d->id = g_ndev;                     /* id 从 1 开始，0 = 不是设备 */
    return d;
}

LxDev *lx_dev_by_id(u64 id)
{
    if (id == 0ull || id > (u64)g_ndev) {
        return (LxDev *)0;
    }
    return &g_dev[id - 1ull];
}

LxDev *lx_dev_find(const char *path)
{
    u32 i;

    if (path == (const char *)0) {
        return (LxDev *)0;
    }
    lx_dev_init();
    for (i = 0u; i < g_ndev; i++) {
        if (same_(g_dev[i].name, path) != 0) {
            return &g_dev[i];
        }
    }
    return (LxDev *)0;
}

LxOsFile *lx_dev_open(LxDev *d)
{
    LxOsFile *o;

    if (d == (LxDev *)0 || d->ops == (const OsFileOps *)0) {
        return (LxOsFile *)0;
    }
    o = (LxOsFile *)port_kcalloc(1ull, (u64)sizeof(LxOsFile));
    if (o == (LxOsFile *)0) {
        return (LxOsFile *)0;
    }
    o->ops = d->ops;
    o->dev = d;
    o->fh = d->priv;
    o->refs = 1u;
    d->refs++;
    return o;
}

LxOsFile *lx_dev_of(u64 node)
{
    return (LxOsFile *)node;
}

void lx_dev_ref(LxOsFile *f)
{
    if (f != (LxOsFile *)0) {
        f->refs++;
    }
}

void lx_dev_release(LxOsFile *f)
{
    if (f == (LxOsFile *)0) {
        return;
    }
    if (f->refs > 1u) {
        f->refs--;
        return;
    }
    if (f->ops != (const OsFileOps *)0 &&
        f->ops->close != (void (*)(LxOsFile *))0) {
        f->ops->close(f);
    }
    if (f->dev != (LxDev *)0 && f->dev->refs > 0u) {
        f->dev->refs--;
    }
    port_kfree((void *)f);
}

i64 lx_dev_read(LxOsFile *f, void *buf, u64 n)
{
    if (f == (LxOsFile *)0 || f->ops == (const OsFileOps *)0 ||
        f->ops->read == (i64 (*)(LxOsFile *, void *, u64))0) {
        return -(i64)LX_EINVAL;
    }
    return f->ops->read(f, buf, n);
}

i64 lx_dev_write(LxOsFile *f, const void *buf, u64 n)
{
    if (f == (LxOsFile *)0 || f->ops == (const OsFileOps *)0 ||
        f->ops->write == (i64 (*)(LxOsFile *, const void *, u64))0) {
        return -(i64)LX_EINVAL;
    }
    return f->ops->write(f, buf, n);
}

u16 lx_dev_poll(LxOsFile *f, u16 events)
{
    if (f == (LxOsFile *)0 || f->ops == (const OsFileOps *)0 ||
        f->ops->poll == (u16 (*)(LxOsFile *, u16))0) {
        return 0u;
    }
    return f->ops->poll(f, events);
}

i64 lx_dev_ioctl(LxOsFile *f, u32 req, u64 arg)
{
    if (f == (LxOsFile *)0 || f->ops == (const OsFileOps *)0 ||
        f->ops->ioctl == (i64 (*)(LxOsFile *, u32, u64))0) {
        return -(i64)LX_ENOTTY;
    }
    return f->ops->ioctl(f, req, arg);
}

i64 lx_dev_mmap(LxOsFile *f, struct LxVma *vma)
{
    if (f == (LxOsFile *)0 || f->ops == (const OsFileOps *)0 ||
        f->ops->mmap == (i64 (*)(LxOsFile *, struct LxVma *))0) {
        return -(i64)LX_ENODEV;
    }
    return f->ops->mmap(f, vma);
}