/* c6LOS 的 lwIP 端口层（平台钩子 + netif 粘合）
 *
 * 位置说明：这个文件属于**将来的**"IP 栈"阶段（docs/wifi-plan.md §207），
 * 现在只保证「能编译、能归档」，还没有接进 c6os.elf —— 因为 WiFi 关联本身
 * 还没在真板上跑通，先接进来只会给一个无法验证的镜像增加风险。
 *
 * 编译期约定：
 *   - 本文件用 -mabi=windowed 编译（和闭源 WiFi 库一致），因为 linkoutput()
 *     要调 libnet80211 的 esp_wifi_internal_tx()（windowed ABI）。
 *   - 内核侧（call0）不能直接调本文件的函数，必须走 port/s3/w2c.S 的正规壳。
 *
 * lwIP 侧要我们提供的东西只有四样（NO_SYS=1 时）：
 *   1) sys_now()            —— 毫秒时基（timeouts 用）
 *   2) netif->linkoutput    —— 发一帧 802.3
 *   3) RX 入口              —— 收到一帧 802.3 后丢给 netif->input()
 *   4) LWIP_PLATFORM_DIAG/ASSERT/RAND（见 arch/cc.h）
 *
 * 已核实（§207）：IDF 的 S3 闭源库确实导出这三个 L2 钩子 ——
 *   esp_wifi_internal_tx            libnet80211.a
 *   esp_wifi_internal_reg_rxcb      libnet80211.a
 *   esp_wifi_internal_free_rx_buffer libpp.a
 * 也就是说：**L2 由我们用，ARP/IP/TCP/UDP/DHCP/DNS 全归 lwIP**，
 * 不需要 esp_netif / FreeRTOS / esp_event 那一整套。
 */
#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/err.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

/* ---- 平台钩子 ---------------------------------------------------------- */

void c6lwip_diag(const char *fmt, ...)
{
    (void)fmt;      /* 诊断输出先留空：等接进镜像时改用内核的 kprintf */
}

void c6lwip_assert(const char *msg)
{
    (void)msg;
    for (;;) { }    /* 断言就是停机；真接进来时改成 panic */
}

unsigned int c6lwip_rand(void)
{
    static unsigned int s = 0x2545F491u;

    s = s * 1103515245u + 12345u;
    return s >> 8;
}

/* 毫秒时基：lwIP 的 timeouts/重传全靠它。由内核的 tick 换算。
 * （s3_w2c_tick_get / s3_w2c_tick_hz 是 port/s3/w2c.S 提供的 windowed 壳） */
extern unsigned int s3_w2c_tick_get(void);
extern unsigned int s3_w2c_tick_hz(void);

u32_t sys_now(void)
{
    unsigned int hz = s3_w2c_tick_hz();
    unsigned long long t = s3_w2c_tick_get();

    if (hz == 0u) {
        return (u32_t)t;
    }
    return (u32_t)((t * 1000ull) / (unsigned long long)hz);
}

/* ---- L2 粘合 ----------------------------------------------------------- */

/* 闭源库里的函数（windowed ABI），故意不引 esp_wifi 头文件：
 * 我们的构建里没有 IDF 的 include 树。原型照 esp_wifi.h 抄。 */
#define C6_WIFI_IF_STA 0
extern int  esp_wifi_internal_tx(int iface, void *buffer, unsigned short len);
extern int  esp_wifi_internal_reg_rxcb(int iface, void *cb);
extern void esp_wifi_internal_free_rx_buffer(void *eb);

static err_t c6_lwip_linkoutput(struct netif *nif, struct pbuf *p)
{
    (void)nif;
    if (p == 0) {
        return ERR_ARG;
    }
    /* LWIP_NETIF_TX_SINGLE_PBUF=1 保证了这里 p 是单块连续 buffer */
    if (esp_wifi_internal_tx(C6_WIFI_IF_STA, p->payload, (unsigned short)p->tot_len) != 0) {
        return ERR_IF;
    }
    return ERR_OK;
}

static err_t c6_lwip_netif_init_cb(struct netif *nif)
{
    nif->name[0] = 's';
    nif->name[1] = 't';
    nif->output     = etharp_output;      /* IP 包 → 找 MAC → linkoutput */
    nif->linkoutput = c6_lwip_linkoutput; /* 真正发一帧 802.3 */
    nif->mtu        = 1500;
    nif->hwaddr_len = 6;
    nif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

/* 收到一帧（由注册给 esp_wifi_internal_reg_rxcb 的回调转进来）。
 * buffer 是完整的 802.3 帧（从目的 MAC 开始），eb 是库自己的 buffer 句柄，
 * 处理完必须用 esp_wifi_internal_free_rx_buffer() 还回去。 */
static int c6_lwip_rx(void *buffer, unsigned short len, void *eb)
{
    struct netif *nif = netif_default;
    struct pbuf *p;

    if (nif != 0 && buffer != 0 && len != 0u) {
        p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
        if (p != 0) {
            pbuf_take(p, buffer, len);
            if (nif->input(p, nif) != ERR_OK) {
                pbuf_free(p);
            }
        }
    }
    if (eb != 0) {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    return 0;
}

/* 供 .c6 侧（经 w2c 壳）调用：起 lwIP、装 netif、注册 RX 回调。
 * mac 必须是真实 MAC（见 §203：假 MAC 会让 DHCP 身份不合法）。 */
int c6_lwip_start(const unsigned char *mac)
{
    static struct netif s_netif;
    struct netif *n;
    int i;

    lwip_init();
    n = netif_add(&s_netif, 0, 0, 0, 0, c6_lwip_netif_init_cb, ethernet_input);
    if (n == 0) {
        return -1;
    }
    netif_set_default(n);
    for (i = 0; i < 6; i++) {
        n->hwaddr[i] = mac[i];
    }
    /* DHCP 拿地址；拿不到就退回静态（本函数返回后由调用方等待/超时） */
    netif_set_up(n);
    dhcp_start(n);
    (void)esp_wifi_internal_reg_rxcb(C6_WIFI_IF_STA, (void *)c6_lwip_rx);
    return 0;
}

/* 主循环里定期调（NO_SYS=1 时 lwIP 不自己跑） */
void c6_lwip_poll(void)
{
    sys_check_timeouts();
}

/* ---- 等 DHCP 拿到地址（§215）------------------------------------------
 * WiFi 事件系统我们没接，所以"拿到 IP 没有"直接看 netif 的地址：DHCP 是 lwIP
 * 自己跑的，只要把 sys_check_timeouts() 转起来 + 定期让出 CPU，它就会自己完成。
 * 返回 1 = 拿到地址（非 0 且不是 0.0.0.0）。 */
extern unsigned int s3_w2c_task_delay(unsigned int ticks);
extern int ets_printf(const char *fmt, ...);

int c6_lwip_wait_ip(int ms)
{
    int waited = 0;

    while (waited < ms) {
        sys_check_timeouts();
        if (netif_default != 0) {
            const ip4_addr_t *a = netif_ip4_addr(netif_default);

            if (a != 0 && a->addr != 0u) {
                return 1;
            }
        }
        s3_w2c_task_delay(1u);
        waited = waited + 10;
    }
    return 0;
}

/* 打印当前地址（调试用） */
void c6_lwip_print_ip(void)
{
    if (netif_default == 0) {
        ets_printf("[lwip] netif 不存在\n");
        return;
    }
    {
        const ip4_addr_t *a = netif_ip4_addr(netif_default);
        const ip4_addr_t *m = netif_ip4_netmask(netif_default);
        const ip4_addr_t *g = netif_ip4_gw(netif_default);
        const u8_t *ab = (const u8_t *)a;
        const u8_t *mb = (const u8_t *)m;
        const u8_t *gb = (const u8_t *)g;

        ets_printf("[lwip] ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
                   ab[0], ab[1], ab[2], ab[3], mb[0], mb[1], mb[2], mb[3],
                   gb[0], gb[1], gb[2], gb[3]);
    }
}
