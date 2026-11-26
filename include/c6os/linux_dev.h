#ifndef C6OS_LINUX_DEV_H
#define C6OS_LINUX_DEV_H

#include "c6os/types.h"

/* G2：设备层对象模型（见 docs/linux-abi-gui-chrome.md §3.1）。
 *
 * LxDev 是设备注册表里的**设备**：名字、ops、私有数据、打开引用数。
 * OsFile 是一次 open 产生的**打开实例**：ops/dev 在 open 时固化，fh 指向
 * 设备的私有数据（fb0 的扫描缓冲、evdev 的队列），refs 供 dup/fork 共享。
 *
 * 约定（与 linux/ 其它层一致）：
 *   - 跨边界只用 u32/u64/i64，不传结构体；
 *   - ops 返回负 errno；-EAGAIN 在阻塞设备上由调用方（linux/fs.c）转成
 *     lx_proc_block() + 重试，这样设备实现不需要认识进程模型。
 *
 * 生产内核可以 -DC6OS_TEST_HOOKS=0 关掉 C6EVIOCINJECT；测试镜像保持默认开。 */
#ifndef C6OS_TEST_HOOKS
#define C6OS_TEST_HOOKS 1
#endif

struct LxVma;
struct OsFile;

typedef struct OsFileOps {
    i64 (*read)(struct OsFile *f, void *buf, u64 n);
    i64 (*write)(struct OsFile *f, const void *buf, u64 n);
    u16 (*poll)(struct OsFile *f, u16 events);
    i64 (*ioctl)(struct OsFile *f, u32 req, u64 arg);
    i64 (*mmap)(struct OsFile *f, struct LxVma *vma);   /* ABI 私有，见 §3.2 */
    void (*close)(struct OsFile *f);
} OsFileOps;

typedef struct LxDev {
    const char *name;          /* "dev/fb0" / "dev/input/event0" */
    const OsFileOps *ops;
    void *priv;
    u32 refs;
    u32 id;                    /* 1..LX_DEV_MAX，0 表示无效 */
} LxDev;

typedef struct OsFile {
    const OsFileOps *ops;
    LxDev *dev;
    void *fh;                  /* 设备私有打开状态（通常 = dev->priv） */
    u64 pos;                   /* 需要定位语义的设备用 */
    u32 refs;
    u32 pad;
} OsFile;

#define LX_DEV_MAX 16u

/* 设备节点在第一次 open 时惰性初始化（不依赖 initramfs 是否存在）。 */
void    lx_dev_init(void);
LxDev  *lx_dev_register(const char *name, const OsFileOps *ops, void *priv);
/* 路径不带前导 '/'（linux/fs.c 已归一化），例如 "dev/fb0"。 */
LxDev  *lx_dev_find(const char *path);
LxDev  *lx_dev_by_id(u64 id);

/* fd 槽的 node 字段存的就是 OsFile*（LX_FD_DEV 专用）。 */
OsFile *lx_dev_open(LxDev *d);
OsFile *lx_dev_of(u64 node);
void    lx_dev_ref(OsFile *f);
void    lx_dev_release(OsFile *f);

i64     lx_dev_read(OsFile *f, void *buf, u64 n);
i64     lx_dev_write(OsFile *f, const void *buf, u64 n);
u16     lx_dev_poll(OsFile *f, u16 events);
i64     lx_dev_ioctl(OsFile *f, u32 req, u64 arg);
i64     lx_dev_mmap(OsFile *f, struct LxVma *vma);

/* 各设备自己的注册入口；由 lx_dev_init 调用。 */
void lx_fb_init(void);
void lx_evdev_init(void);
/* 真输入驱动（i8042）向 event0 注入事件；无设备构建里由 irq.c 提供弱符号。 */
void lx_evdev_push(u16 type, u16 code, i32 value);

#endif