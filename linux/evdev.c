/* G2：/dev/input/event0 —— 最小 evdev（docs/linux-abi-gui-chrome.md §3.3）。
 *
 *   - input_event 是 x86-64 Linux 的 24 B 布局：timeval(16) + type/code(4) +
 *     value(4)，一个字节都不能差；
 *   - read() 按整事件返回（len < 24 报 EINVAL）；空队列返回 -EAGAIN，由
 *     linux/fs.c 的阻塞循环决定是 lx_proc_block 还是把 EAGAIN 透给用户；
 *   - poll() 有事件报 POLLIN；
 *   - C6EVIOCINJECT 是 TDD 私有注入通道（x86 的 i8042 真路径是 G2b），
 *     生产内核用 -DC6OS_TEST_HOOKS=0 关掉。
 *
 * 队列只有一个：event0 是整机唯一的虚拟输入设备，多 open 共享同一个
 * 硬件的语义（Linux evdev 每 client 一份队列，等真驱动时再拆）。 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_proc.h"
#include "c6os/linux_dev.h"

#define EV_QMAX      64u
#define EV_SIZE      24u

#define EV_SYN       0u
#define EV_KEY       1u
#define EV_REL       2u
#define EV_ABS       3u

#define SYN_REPORT   0u

#define KEY_ENTER    28u
#define KEY_A        30u
#define KEY_Z        44u
#define KEY_SPACE    57u

#define REL_X        0u
#define REL_Y        1u
#define ABS_X        0u
#define ABS_Y        1u

#define EV_VERSION   0x010001u

#define EVIOCGVERSION 0x80044501u
#define EVIOCGID      0x80084502u
#define EVIOCGNAME   0x80404506u          /* 实际按请求里的 size 字段变长 */
#define C6EVIOCINJECT 0x401845f0u         /* _IOW('E', 0xf0, struct input_event) */

#define FB_W 1280
#define FB_H 800

/* x86-64 Linux 的 timeval 是两个 64 位；下面 padding 让结构体正好 24 B。 */
typedef struct {
    u64 sec;
    u64 usec;
    u16 type;
    u16 code;
    i32 value;
} LxInputEvent;

typedef struct {
    u16 bustype;
    u16 vendor;
    u16 product;
    u16 version;
} LxInputId;

typedef struct {
    i32 value;
    i32 minimum;
    i32 maximum;
    i32 fuzz;
    i32 flat;
    i32 resolution;
} LxInputAbsinfo;

typedef struct {
    LxInputEvent q[EV_QMAX];
    u32 head;
    u32 tail;
    u32 count;
} LxEvdev;

static LxEvdev g_ev;

_Static_assert(sizeof(LxInputEvent) == EV_SIZE, "input_event ABI");
_Static_assert(sizeof(LxInputId) == 8, "input_id ABI");
_Static_assert(sizeof(LxInputAbsinfo) == 24, "input_absinfo ABI");

static void ev_zero_(u8 *p, u64 n)
{
    u64 i;

    for (i = 0ull; i < n; i++) {
        p[i] = 0u;
    }
}

static void ev_copy_out_(u8 *dst, const LxInputEvent *src)
{
    u32 i;

    for (i = 0u; i < EV_SIZE; i++) {
        dst[i] = ((const u8 *)src)[i];
    }
}

static void ev_copy_in_(LxInputEvent *dst, const u8 *src)
{
    u32 i;

    for (i = 0u; i < EV_SIZE; i++) {
        ((u8 *)dst)[i] = src[i];
    }
}

static i64 ev_read_(struct OsFile *f, void *buf, u64 len)
{
    LxEvdev *e = (f != (struct OsFile *)0) ? (LxEvdev *)f->fh : (LxEvdev *)0;
    u64 take;
    u64 max;
    u64 i;

    if (e == (LxEvdev *)0) {
        return -(i64)LX_ENODEV;
    }
    if (len < EV_SIZE) {
        return -(i64)LX_EINVAL;
    }
    if (e->count == 0u) {
        return -(i64)LX_EAGAIN;             /* fs.c 决定 block 还是透传 */
    }
    max = len / EV_SIZE;
    take = (max < (u64)e->count) ? max : (u64)e->count;
    for (i = 0ull; i < take; i++) {
        u32 at = e->head;
        u8 *out = (u8 *)buf + i * EV_SIZE;

        ev_copy_out_(out, &e->q[at]);
        e->head = (e->head + 1u) % EV_QMAX;
        e->count--;
    }
    return (i64)(take * EV_SIZE);
}

static i64 ev_write_(struct OsFile *f, const void *buf, u64 len)
{
    (void)f;
    (void)buf;
    (void)len;
    return -(i64)LX_EINVAL;                 /* 用户不能伪造输入事件 */
}

static u16 ev_poll_(struct OsFile *f, u16 events)
{
    LxEvdev *e = (f != (struct OsFile *)0) ? (LxEvdev *)f->fh : (LxEvdev *)0;

    (void)events;
    if (e != (LxEvdev *)0 && e->count != 0u) {
        return (u16)LX_POLLIN;
    }
    return 0u;
}

void lx_evdev_push(u16 type, u16 code, i32 value)
{
    LxInputEvent ev;

    if (g_ev.count >= EV_QMAX) {
        return;
    }
    ev.sec = port_wallclock_sec();
    ev.usec = (port_cycles() / 1000ull) % 1000000ull;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    g_ev.q[g_ev.tail] = ev;
    g_ev.tail = (g_ev.tail + 1u) % EV_QMAX;
    g_ev.count++;
    lx_proc_wake_all();
}

static i64 ev_inject_(LxEvdev *e, u64 uarg)
{
    LxInputEvent ev;
    u32 i;

    if (lx_user_ok(uarg, (u64)EV_SIZE, C6OS_PROT_R) == 0) {
        return -(i64)LX_EFAULT;
    }
    ev_copy_in_(&ev, (const u8 *)uarg);
    if (ev.type > EV_ABS) {
        return -(i64)LX_EINVAL;
    }
    if (ev.sec == 0ull && ev.usec == 0ull) {
        ev.sec = port_wallclock_sec();
        ev.usec = (port_cycles() / 1000ull) % 1000000ull;
    }
    if (e->count >= EV_QMAX) {
        return -(i64)LX_ENOSPC;
    }
    e->q[e->tail] = ev;
    e->tail = (e->tail + 1u) % EV_QMAX;
    e->count++;
    lx_proc_wake_all();
    return 0;
}

static u32 ev_name_(char *out, u32 cap)
{
    const char *s = "c6os virtual keyboard";
    u32 n = 0u;
    u32 i = 0u;

    while (s[n] != '\0') {
        n++;
    }
    if (cap == 0u) {
        return 0u;
    }
    while (i < n && i + 1u < cap) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
    return i + 1u;
}

static void ev_bits_(u32 ev, u8 *bits, u32 n)
{
    ev_zero_(bits, (u64)n);
    if (ev == EV_SYN) {
        bits[0] = (u8)((1u << EV_SYN) | (1u << EV_KEY) |
                       (1u << EV_REL) | (1u << EV_ABS));
    } else if (ev == EV_KEY) {
        bits[KEY_ENTER / 8u] |= (u8)(1u << (KEY_ENTER % 8u));
        bits[KEY_A / 8u] |= (u8)(1u << (KEY_A % 8u));
        bits[KEY_Z / 8u] |= (u8)(1u << (KEY_Z % 8u));
        bits[KEY_SPACE / 8u] |= (u8)(1u << (KEY_SPACE % 8u));
    } else if (ev == EV_REL) {
        bits[REL_X / 8u] |= (u8)(1u << (REL_X % 8u));
        bits[REL_Y / 8u] |= (u8)(1u << (REL_Y % 8u));
    } else if (ev == EV_ABS) {
        bits[ABS_X / 8u] |= (u8)(1u << (ABS_X % 8u));
        bits[ABS_Y / 8u] |= (u8)(1u << (ABS_Y % 8u));
    }
}

static i64 ev_ioctl_(struct OsFile *f, u32 req, u64 arg)
{
    LxEvdev *e = (f != (struct OsFile *)0) ? (LxEvdev *)f->fh : (LxEvdev *)0;
    u32 nr;
    u32 len;
    u32 i;

    if (e == (LxEvdev *)0) {
        return -(i64)LX_ENODEV;
    }
#if C6OS_TEST_HOOKS
    if (req == C6EVIOCINJECT) {
        return ev_inject_(e, arg);
    }
#endif
    if (req == EVIOCGVERSION) {
        u32 ver = EV_VERSION;

        if (lx_user_ok(arg, 4ull, C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        for (i = 0u; i < 4u; i++) {
            ((u8 *)arg)[i] = (u8)((ver >> (i * 8u)) & 0xffu);
        }
        return 0;
    }
    if (req == EVIOCGID) {
        LxInputId id;

        if (lx_user_ok(arg, 8ull, C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        id.bustype = 0x0019u;               /* BUS_VIRTUAL */
        id.vendor = 0x0001u;
        id.product = 0xc6f0u;
        id.version = 0x0001u;
        for (i = 0u; i < 8u; i++) {
            ((u8 *)arg)[i] = ((const u8 *)&id)[i];
        }
        return 0;
    }

    nr = req & 0xffu;
    len = (req >> 16) & 0x3fffu;
    if (len == 0u) {
        return -(i64)LX_EINVAL;
    }
    if (nr == 0x06u) {                      /* EVIOCGNAME(len) */
        u32 wrote;

        if (lx_user_ok(arg, (u64)len, C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        wrote = ev_name_((char *)arg, len);
        if (wrote == 0u) {
            return -(i64)LX_EINVAL;
        }
        return (i64)wrote;
    }
    if (nr >= 0x20u && nr <= 0x3fu) {       /* EVIOCGBIT(ev,len) */
        u8 bits[64];
        u32 ev = nr - 0x20u;
        u32 n = (len < (u32)sizeof(bits)) ? len : (u32)sizeof(bits);

        if (lx_user_ok(arg, (u64)n, C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        ev_bits_(ev, bits, n);
        for (i = 0u; i < n; i++) {
            ((u8 *)arg)[i] = bits[i];
        }
        return 0;
    }
    if (nr >= 0x40u && nr <= 0x5fu) {       /* EVIOCGABS(abs) */
        u32 abs = nr - 0x40u;
        LxInputAbsinfo ai;

        if (len < (u32)sizeof(LxInputAbsinfo) ||
            lx_user_ok(arg, (u64)sizeof(LxInputAbsinfo), C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        ev_zero_((u8 *)&ai, (u64)sizeof(ai));
        if (abs == ABS_X) {
            ai.value = FB_W / 2;
            ai.minimum = 0;
            ai.maximum = FB_W - 1;
        } else if (abs == ABS_Y) {
            ai.value = FB_H / 2;
            ai.minimum = 0;
            ai.maximum = FB_H - 1;
        } else {
            return -(i64)LX_EINVAL;
        }
        for (i = 0u; i < (u32)sizeof(ai); i++) {
            ((u8 *)arg)[i] = ((const u8 *)&ai)[i];
        }
        return 0;
    }
    return -(i64)LX_ENOTTY;
}

static const OsFileOps g_ev_ops = {
    ev_read_,
    ev_write_,
    ev_poll_,
    ev_ioctl_,
    (i64 (*)(struct OsFile *, struct LxVma *))0,
    (void (*)(struct OsFile *))0,
};

void lx_evdev_init(void)
{
    (void)lx_dev_register("dev/input/event0", &g_ev_ops, (void *)&g_ev);
    port_puts("evdev: /dev/input/event0（c6os virtual keyboard，24B input_event）\n");
}