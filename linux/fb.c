/* G2：/dev/fb0 —— 内存扫描缓冲（docs/linux-abi-gui-chrome.md §3.2）。
 *
 * 本里程碑不做 VBE/i8042 真硬件（那是 G2b），先把 ABI 与 mmap 语义钉死：
 *   - 扫描缓冲是 PMM 分配的**物理连续页**（1280x800 XRGB8888 = 4,096,000 B
 *     = 正好 1000 页）；驱动长期持有每页的 1 份引用；
 *   - 用户 mmap 时逐页 port_phys_ref，munmap/进程退出由 port_as_unmap 的
 *     port_phys_put 抵消，所以用户映射消失后扫描缓冲仍在（不会把 fb 还进 PMM）；
 *   - 这里**不能**用 C6OS_PROT_DEV/PTE_DEV：那是给 x86 VBE 的 MMIO 用的；
 *     DRAM 帧缓冲是普通 PMM 页，必须走引用计数。
 *
 * struct 布局按 x86-64 Linux（fb_var_screeninfo 160 B / fb_fix_screeninfo 80 B），
 * 下面有编译期断言，改动字段时构建会先炸而不是让用户态读错。 */
#include "c6os/types.h"
#include "c6os/port.h"
#include "c6os/linux_abi.h"
#include "c6os/linux_vma.h"
#include "c6os/linux_dev.h"

#define FB_W        1280u
#define FB_H        800u
#define FB_BPP      32u
#define FB_LINE     (FB_W * 4u)
#define FB_SIZE     (FB_LINE * FB_H)       /* 4,096,000 = 1000 * 4096 */
#define FB_PAGES    (FB_SIZE / 4096u)

#define FBIOGET_VSCREENINFO 0x4600u
#define FBIOPUT_VSCREENINFO 0x4601u
#define FBIOGET_FSCREENINFO 0x4602u
#define FBIOBLANK           0x4611u
#define C6FBIOFLUSH         0x40104610u    /* _IOW('F', 0x10, struct c6_fb_rect) */

#define FB_VISUAL_TRUECOLOR 2u

typedef struct {
    u32 offset;
    u32 length;
    u32 msb_right;
} FbBitfield;

typedef struct {                           /* 160 B */
    u32 xres;
    u32 yres;
    u32 xres_virtual;
    u32 yres_virtual;
    u32 xoffset;
    u32 yoffset;
    u32 bits_per_pixel;
    u32 grayscale;
    FbBitfield red;
    FbBitfield green;
    FbBitfield blue;
    FbBitfield transp;
    u32 nonstd;
    u32 activate;
    u32 height;
    u32 width;
    u32 accel_flags;
    u32 pixclock;
    u32 left_margin;
    u32 right_margin;
    u32 upper_margin;
    u32 lower_margin;
    u32 hsync_len;
    u32 vsync_len;
    u32 sync;
    u32 vmode;
    u32 rotate;
    u32 colorspace;
    u32 reserved[4];
} FbVarScreeninfo;

typedef struct {                           /* 80 B */
    char id[16];
    u64 smem_start;
    u32 smem_len;
    u32 type;
    u32 type_aux;
    u32 visual;
    u16 xpanstep;
    u16 ypanstep;
    u16 ywrapstep;
    u32 line_length;
    u64 mmio_start;
    u32 mmio_len;
    u32 accel;
    u32 capabilities;
    u16 reserved[2];
} FbFixScreeninfo;

typedef struct {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
} FbRect;

_Static_assert(sizeof(FbVarScreeninfo) == 160, "fb_var_screeninfo ABI");
_Static_assert(sizeof(FbFixScreeninfo) == 80, "fb_fix_screeninfo ABI");
_Static_assert(sizeof(FbRect) == 16, "c6_fb_rect ABI");
_Static_assert(FB_SIZE == FB_PAGES * 4096u, "fb size must be whole pages");

typedef struct {
    FbVarScreeninfo var;
    FbFixScreeninfo fix;
    u64 pa;
    u64 size;
    u8 *kmap;                  /* 直接映射窗口里的内核视图 */
} LxFb;

static LxFb g_fb;

static void bytes_zero_(u8 *p, u64 n)
{
    u64 i;

    for (i = 0ull; i < n; i++) {
        p[i] = 0u;
    }
}

static void id_copy_(char *id, const char *s)
{
    u32 i = 0u;

    while (i < 15u && s[i] != '\0') {
        id[i] = s[i];
        i++;
    }
    id[i] = '\0';
}

static i64 fb_read_(struct OsFile *f, void *buf, u64 n)
{
    (void)f;
    (void)buf;
    (void)n;
    return -(i64)LX_EINVAL;                 /* 帧缓冲没有 read(2) 语义 */
}

static i64 fb_write_(struct OsFile *f, const void *buf, u64 n)
{
    (void)f;
    (void)buf;
    (void)n;
    return -(i64)LX_EINVAL;                 /* 像素走 mmap；写路径留给 G3 */
}

static u16 fb_poll_(struct OsFile *f, u16 events)
{
    (void)f;
    (void)events;
    return (u16)LX_POLLOUT;                 /* 帧缓冲永远可写 */
}

static i64 fb_ioctl_(struct OsFile *f, u32 req, u64 arg)
{
    LxFb *fb = (f != (struct OsFile *)0) ? (LxFb *)f->fh : (LxFb *)0;

    if (fb == (LxFb *)0) {
        return -(i64)LX_ENODEV;
    }
    if (req == FBIOGET_VSCREENINFO) {
        if (lx_user_ok(arg, (u64)sizeof(FbVarScreeninfo), C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        *(FbVarScreeninfo *)arg = fb->var;
        return 0;
    }
    if (req == FBIOGET_FSCREENINFO) {
        if (lx_user_ok(arg, (u64)sizeof(FbFixScreeninfo), C6OS_PROT_W) == 0) {
            return -(i64)LX_EFAULT;
        }
        *(FbFixScreeninfo *)arg = fb->fix;
        return 0;
    }
    if (req == FBIOPUT_VSCREENINFO) {
        FbVarScreeninfo nv;

        if (lx_user_ok(arg, (u64)sizeof(FbVarScreeninfo), C6OS_PROT_R) == 0) {
            return -(i64)LX_EFAULT;
        }
        nv = *(const FbVarScreeninfo *)arg;
        /* 只接受同分辨率/同 bpp；xoffset/yoffset 也必须还是 0（单一缓冲）。 */
        if (nv.xres != fb->var.xres || nv.yres != fb->var.yres ||
            nv.bits_per_pixel != fb->var.bits_per_pixel ||
            nv.xres_virtual < nv.xres || nv.yres_virtual < nv.yres ||
            nv.xres_virtual > fb->var.xres_virtual ||
            nv.yres_virtual > fb->var.yres_virtual ||
            nv.xoffset != 0u || nv.yoffset != 0u) {
            return -(i64)LX_EINVAL;
        }
        fb->var = nv;
        return 0;
    }
    if (req == FBIOBLANK) {
        /* FB_BLANK_UNBLANK/NORMAL/POWERDOWN 都接受：控制台/合成器会试这几个。 */
        return 0;
    }
    if (req == C6FBIOFLUSH) {
        FbRect r;

        if (lx_user_ok(arg, (u64)sizeof(FbRect), C6OS_PROT_R) == 0) {
            return -(i64)LX_EFAULT;
        }
        r = *(const FbRect *)arg;
        if (r.x >= FB_W || r.y >= FB_H ||
            r.w > FB_W - r.x || r.h > FB_H - r.y) {
            return -(i64)LX_EINVAL;
        }
        /* 内存帧缓冲没有 GDMA/面板要 blit；S3 驱动在这里调 s3_lcd_blit_rect。 */
        return 0;
    }
    return -(i64)LX_ENOTTY;
}

static i64 fb_mmap_(struct OsFile *f, struct LxVma *vma)
{
    LxFb *fb = (f != (struct OsFile *)0) ? (LxFb *)f->fh : (LxFb *)0;
    u64 as;
    u64 len;
    u64 pages;
    u64 i;
    u64 j;

    if (fb == (LxFb *)0 || fb->pa == 0ull) {
        return -(i64)LX_ENODEV;
    }
    if (vma == (struct LxVma *)0 || vma->file_off != 0ull) {
        return -(i64)LX_EINVAL;
    }
    len = vma->end - vma->start;
    if (len == 0ull || (len & 4095ull) != 0ull || len > fb->size) {
        return -(i64)LX_EINVAL;
    }
    as = lx_as_get();
    if (as == 0ull) {
        return -(i64)LX_EINVAL;
    }
    pages = len >> 12;
    /* 先给每页加一份"用户映射"引用：port_as_unmap/AS 销毁时的 put 正好抵消。
     * 驱动自己那一份引用在 lx_fb_init 里由 alloc_pages 建立，永不归还。 */
    for (i = 0ull; i < pages; i++) {
        port_phys_ref(fb->pa + (i << 12));
    }
    for (i = 0ull; i < pages; i++) {
        if (port_as_map(as, vma->start + (i << 12),
                        fb->pa + (i << 12), 4096ull, vma->prot) != 0) {
            for (j = 0ull; j < i; j++) {
                (void)port_as_unmap(as, vma->start + (j << 12), 4096ull);
            }
            for (j = i; j < pages; j++) {
                port_phys_put(fb->pa + (j << 12));
            }
            return -(i64)LX_ENOMEM;
        }
    }
    return 0;
}

static const OsFileOps g_fb_ops = {
    fb_read_,
    fb_write_,
    fb_poll_,
    fb_ioctl_,
    fb_mmap_,
    (void (*)(struct OsFile *))0,
};

void lx_fb_init(void)
{
    u64 pa;
    u32 i;

    if (g_fb.pa != 0ull) {
        return;                            /* 已初始化（幂等） */
    }
    pa = port_phys_alloc_pages((u64)FB_PAGES);
    if (pa == 0ull) {
        port_puts("fb: FAIL 扫描缓冲分配失败（需要 4MB 连续物理页）\n");
        return;                            /* 不注册 → /dev/fb0 open 返回 ENOENT */
    }
    g_fb.pa = pa;
    g_fb.size = (u64)FB_SIZE;
    g_fb.kmap = (u8 *)port_phys_map(pa);
    /* 分配器不清零：第一次映射不能把上一个进程的残留当像素暴露给用户。 */
    bytes_zero_(g_fb.kmap, (u64)FB_SIZE);

    bytes_zero_((u8 *)&g_fb.var, (u64)sizeof(g_fb.var));
    g_fb.var.xres = FB_W;
    g_fb.var.yres = FB_H;
    g_fb.var.xres_virtual = FB_W;
    g_fb.var.yres_virtual = FB_H;
    g_fb.var.bits_per_pixel = FB_BPP;
    g_fb.var.grayscale = 0u;
    g_fb.var.red.offset = 16u;
    g_fb.var.red.length = 8u;
    g_fb.var.green.offset = 8u;
    g_fb.var.green.length = 8u;
    g_fb.var.blue.offset = 0u;
    g_fb.var.blue.length = 8u;
    g_fb.var.transp.offset = 24u;
    g_fb.var.transp.length = 8u;
    g_fb.var.activate = 0u;                 /* FB_ACTIVATE_NOW */
    g_fb.var.width = FB_W;
    g_fb.var.height = FB_H;
    g_fb.var.vmode = 0u;                    /* FB_VMODE_NONINTERLACED */
    g_fb.var.colorspace = 0u;

    bytes_zero_((u8 *)&g_fb.fix, (u64)sizeof(g_fb.fix));
    id_copy_(g_fb.fix.id, "c6os-fb0");
    g_fb.fix.smem_start = pa;
    g_fb.fix.smem_len = (u32)FB_SIZE;
    g_fb.fix.type = 0u;                     /* FB_TYPE_PACKED_PIXELS */
    g_fb.fix.type_aux = 0u;
    g_fb.fix.visual = FB_VISUAL_TRUECOLOR;
    g_fb.fix.xpanstep = 0u;
    g_fb.fix.ypanstep = 0u;
    g_fb.fix.ywrapstep = 0u;
    g_fb.fix.line_length = FB_LINE;
    g_fb.fix.mmio_start = 0ull;
    g_fb.fix.mmio_len = 0u;
    g_fb.fix.accel = 0u;                    /* FB_ACCEL_NONE */
    g_fb.fix.capabilities = 0u;

    for (i = 0u; i < 4u; i++) {
        g_fb.var.reserved[i] = 0u;
    }
    (void)lx_dev_register("dev/fb0", &g_fb_ops, (void *)&g_fb);
    port_puts("fb: /dev/fb0 1280x800 XRGB8888（4MB 物理连续扫描缓冲）\n");
}