/* Xingzhi Cube 1.54TFT WiFi 的三个按键（引脚来自上游 config.h）：
 *   BOOT=GPIO0  VOLUME_UP=GPIO40  VOLUME_DOWN=GPIO39
 * 全部是"按下接地"的接法（内部上拉）。
 *
 * 只做轮询 + 去抖：每 tick 采一次（100Hz），连续 3 次（30ms）一致才认账，
 * 松开同理。对外给 s3_btn_pressed(id) / s3_btn_take_event(id)（边沿）。
 */
#include "c6os/types.h"
#include "c6os/port.h"

#define GPIO_BASE      0x60004000u
#define GPIO_IN        (*(volatile u32 *)(GPIO_BASE + 0x3Cu))
#define IO_MUX(p)      (0x60009000u + 0x04u + 4u * (p))

#define BTN_N   3u
#define BTN_BOOT 0u
#define BTN_UP   1u
#define BTN_DOWN 2u

static const u32 g_btn_pin[BTN_N] = {0u, 40u, 39u};

static u32 g_btn_down[BTN_N];     /* 去抖后的状态：1=按下 */
static u32 g_btn_cnt[BTN_N];
static u32 g_btn_event[BTN_N];    /* 1=有一次"按下"边沿没取走 */

static int btn_raw(u32 i)
{
    return ((GPIO_IN >> g_btn_pin[i]) & 1u) == 0u;   /* 低电平=按下 */
}

void s3_btn_init(void)
{
    u32 i = 0;

    while (i < BTN_N) {
        volatile u32 *mux = (volatile u32 *)IO_MUX(g_btn_pin[i]);
        u32 v = *mux;
        v = (v & ~(7u << 12)) | (1u << 12);   /* MCU_SEL=1 = GPIO */
        v = v | (1u << 9);                    /* FUN_IE  输入使能 */
        v = v | (1u << 8);                    /* FUN_WPU 上拉 */
        v = v & ~(1u << 7);                   /* 关下拉 */
        *mux = v;
        g_btn_down[i] = 0u;
        g_btn_cnt[i] = 0u;
        g_btn_event[i] = 0u;
        i++;
    }
}

/* 每 1 tick（10ms）调一次 */
void s3_btn_poll(void)
{
    u32 i = 0;

    while (i < BTN_N) {
        int raw = btn_raw(i);
        if ((u32)raw != g_btn_down[i]) {
            g_btn_cnt[i]++;
            if (g_btn_cnt[i] >= 3u) {          /* 30ms 稳定 */
                g_btn_down[i] = (u32)raw;
                g_btn_cnt[i] = 0u;
                if (raw != 0) { g_btn_event[i] = 1u; }   /* 只对"按下"记边沿 */
            }
        } else {
            g_btn_cnt[i] = 0u;
        }
        i++;
    }
}

u32 s3_btn_pressed(u32 i)
{
    return (i < BTN_N) ? g_btn_down[i] : 0u;
}

u32 s3_btn_take_event(u32 i)
{
    u32 v = 0;

    if (i >= BTN_N) { return 0u; }
    v = g_btn_event[i];
    g_btn_event[i] = 0u;
    return v;
}

u32 s3_btn_count(void)
{
    return BTN_N;
}
