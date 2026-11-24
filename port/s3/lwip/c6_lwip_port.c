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
#include "lwip/tcpip.h"
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

/* §848: 扫描 rec[0] 是最强邻居，RC seed 把它的 BSSID 灌进 cnx 节点 →
 * 连邻居 → WPA 密码不符 → 无 DHCP 回应（TX=5/RX=0）。
 * §846 wrap get_ap_records 破坏库 BSS 链表；§847 wrap scan_start 破坏扫描
 * （--wrap 拦截库内部所有调用点）。改为 wrap cnx_add_rc：只在 seed 的固定
 * slot 地址上把 BSSID(+4..+9) 改成目标，库自身调用（别的指针）原样透传。
 * wifi_glue.c 保持字节不变（任何改动都破坏脆弱的同步扫描）。 */
#ifdef S3_TARGET_BSSID
extern void __real_cnx_add_rc(void *bss);
void __wrap_cnx_add_rc(void *bss)
{
    if ((unsigned long)bss == 0x3fcd6b48ul) {
        unsigned char *b = (unsigned char *)bss;
        b[4] = 0xdcu; b[5] = 0xd8u; b[6] = 0x7cu;
        b[7] = 0x5au; b[8] = 0x7au; b[9] = 0x46u;
    }
    __real_cnx_add_rc(bss);
}
#endif

/* §852: 诊断 mgmt 帧发送——auth/assoc 帧到底有没有尝试发出去。
 * send_mgmt(buf,subtype,extra): a11=subtype(176=auth,32=assoc_req,160=disassoc)
 * 复用 g_lwip_stage[2]=总调用/[3]= subtype 位图（不新增 BSS，scan 对布局极敏感）。
 * [2]/[3] 平时只被 sys_thread_new 写，而 tcpip 线程没起，运行时恒为 0。 */
extern volatile unsigned int g_lwip_stage[8];

/* §859: 软件 wrap 计数管理帧 RX 路径（比 JTAG 硬件断点可靠）。
 * sta_recv_mgmt 是窗口化库函数；wrap 透传全部 6 个寄存器参数。
 * 复用 g_lwip_stage[4] 高 16 位计数（低 16 位送 send_mgmt 返回），避免新增 BSS
 * （新增 BSS 会破坏脆弱的同步扫描布局，见 §860）。 */
extern volatile unsigned int g_lwip_stage[8];
extern void *__real_sta_recv_mgmt(void *, void *, void *, void *, void *, void *);
void *__wrap_sta_recv_mgmt(void *a, void *b, void *c, void *d, void *e, void *f)
{
    g_lwip_stage[4] += 0x10000u;   /* 高 16 位 = sta_recv_mgmt 调用数 */
    return __real_sta_recv_mgmt(a, b, c, d, e, f);
}

extern void *__real_ieee80211_send_mgmt(void *buf, int subtype, int extra);
void *__wrap_ieee80211_send_mgmt(void *buf, int subtype, int extra)
{
    g_lwip_stage[2]++;                       /* mgmt 帧发送总次数 */
    if (subtype == 176 || subtype == 0xb0) {
        /* §863: AUTH 构造瞬间记录 g_chm home/current，判定是否被 chm_is_at_home_channel
         * 延迟入队。打包进 stage[2] 高 16 位：bit16..23=home, bit24..31=current。 */
        unsigned int chmp = *(volatile unsigned int *)0x3fcef850u;
        unsigned int home = chmp ? *(volatile unsigned char *)(chmp + 80u) : 0xffu;
        unsigned int cur  = chmp ? *(volatile unsigned char *)(chmp + 82u) : 0xffu;
        g_lwip_stage[2] |= (home << 16) | (cur << 24);
        if (chmp && home != cur) {
            /* §864: 就在 AUTH 构造前把 home 设成 current，让紧随其后的
             * chm_is_at_home_channel() 返回真 → 帧直接 ic_tx_pkt 发出而不入延迟队列。 */
            *(volatile unsigned char *)(chmp + 80u) = (unsigned char)cur;
        }
        g_lwip_stage[3] |= 0x1u;  /* auth */
    }
    else if (subtype == 32 || subtype == 0x20) { g_lwip_stage[3] |= 0x2u; } /* assoc_req */
    else if (subtype == 160 || subtype == 0xa0) { g_lwip_stage[3] |= 0x4u; } /* disassoc */
    {
        void *ret = __real_ieee80211_send_mgmt(buf, subtype, extra);
        g_lwip_stage[4] = (g_lwip_stage[4] & 0xffff0000u) | ((unsigned int)(unsigned long)ret & 0xffffu);   /* 低16=send_mgmt 返回；高16=sta_recv_mgmt 计数 */
        return ret;
    }
}

static err_t c6_lwip_linkoutput(struct netif *nif, struct pbuf *p)
{
    (void)nif;
    g_lwip_stage[5]++;            /* linkoutput 调用次数（DHCP Discover 等） */
    if (p == 0) {
        return ERR_ARG;
    }
    /* LWIP_NETIF_TX_SINGLE_PBUF=1 保证了这里 p 是单块连续 buffer */
    {
        int tr = esp_wifi_internal_tx(C6_WIFI_IF_STA, p->payload, (unsigned short)p->tot_len);
        g_lwip_stage[7] = (unsigned int)tr;   /* §849: 最近一次 TX 返回码；0=成功 */
        if (tr != 0) {
            return ERR_IF;
        }
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
int c6_lwip_rx(void *buffer, unsigned short len, void *eb)
{
    struct netif *nif = netif_default;
    struct pbuf *p;

    g_lwip_stage[6]++;            /* RX 回调被库调用次数 */
    if (nif != 0 && buffer != 0 && len != 0u) {
        p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
        if (p != 0) {
            pbuf_take(p, buffer, len);
            if (tcpip_input(p, nif) != ERR_OK) {
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
extern int s3_ets_printf(const char *fmt, ...);
/* 阶段信标：DRAM 全局（RTC 慢速内存实测会被库/PHY 写坏，改用 DRAM，
 * 崩了以后 JTAG 直接 x/8xw g_lwip_stage 就能看走到哪一步）。 */
volatile unsigned int g_lwip_stage[8] = {0};
#define LWSTAGE(v) (g_lwip_stage[0] = (unsigned int)(v))
#define LWSTAGE2(i, v) (g_lwip_stage[i] = (unsigned int)(v))

int c6_lwip_start(const unsigned char *mac)
{
    static struct netif s_netif;
    struct netif *n;
    int i;

    LWSTAGE(0x201);
    /* §959 ★ 必须是 tcpip_init，不能是 lwip_init。
     *
     * lwipopts.h 里 NO_SYS=0、LWIP_SOCKET/LWIP_NETCONN 都开着，BSD socket 全走
     * netconn → tcpip_send_msg_wait_sem → tcpip_mbox，而 tcpip_mbox 只有
     * tcpip_init() 才会创建。原来这里调 lwip_init()，而整个镜像里**没有任何
     * 一处**调 tcpip_init —— 反汇编确认 `sys_thread_new` 的唯一调用点就在
     * `tcpip_init` 体内（0x420855a7），所以 tcpip 线程从来没起来过。
     * 后果：lwip_socket()/connect()/getaddrinfo() 没有线程可等，DHCP 也没人
     * 驱动。验收要的 DHCP 地址、TLS、HTTP 全都过不去。
     * tcpip_init 内部会自己调 lwip_init 并 sys_thread_new("tcpip")。 */
    tcpip_init(0, 0);
    LWSTAGE(0x202);
    /* NO_SYS=0：收包必须交给 tcpip 线程处理（`tcpip_input` 会把它塞进
     * tcpip 的邮箱）。之前这里传的是 `ethernet_input`，等于在 WiFi 的 RX
     * 回调上下文里**直接**跑 lwIP 的协议栈 —— 和 tcpip 线程并发访问同一批
     * 全局状态，轻则丢包乱序，重则把 pcb 链表踩坏（§220）。 */
    n = netif_add(&s_netif, 0, 0, 0, 0, c6_lwip_netif_init_cb, tcpip_input);
    LWSTAGE(0x203);
    if (n == 0) {
        return -1;
    }
    netif_set_default(n);
    for (i = 0; i < 6; i++) {
        n->hwaddr[i] = mac[i];
    }
    /* DHCP 拿地址；拿不到就退回静态（本函数返回后由调用方等待/超时） */
    netif_set_up(n);
    /* §843: dhcp_start() 先检查 netif_is_link_up()；只 set_up 不够，
     * link 位没置的话它进 INIT 状态等 dhcp_network_changed()，而 NO_SYS=0
     * 又没有 tcpip 线程驱动 → Discover 永远不发。这里直接置 link up。 */
    netif_set_link_up(n);
    LWSTAGE(0x204);
    /* DHCP 特意**推迟到关联成功之后**再启动（§253）：现在立刻 dhcp_start 会在
     * 发第一包时经过 esp_wifi_internal_tx → 库里的发送路径，而这时还没关联 AP，
     * 真板实测会卡在 dhcp_start 里出不来。 */
    LWSTAGE(0x205);
    /* 注册 RX 回调也推迟到关联之后（§260）：现在调
     * esp_wifi_internal_reg_rxcb 会在真板上直接崩——PC 跳进栈地址、a0=0。
     * 这一步同样依赖 WiFi 驱动已经 start 起来，早调就是踩未初始化的内部状态。 */
    LWSTAGE(0x206);
    return 0;
}

/* §960: dhcp_start 必须在 tcpip 线程里执行 —— 见 c6_lwip_start_dhcp 的说明。 */
static void c6_dhcp_start_cb(void *arg)
{
    s3_ets_printf("[lwip] dhcbcb\n");
    (void)dhcp_start((struct netif *)arg);
}

/* 关联成功之后再启动 DHCP（§253）。 */
int c6_lwip_start_dhcp(void)
{
    s3_ets_printf("[lwip] sd in\n");
    LWSTAGE(0x211);
    LWSTAGE2(1, 0x2111u);
    LWSTAGE(0x212);
    (void)esp_wifi_internal_reg_rxcb(C6_WIFI_IF_STA, (void *)c6_lwip_rx);
    LWSTAGE(0x213);
    if (netif_default == 0) {
        s3_ets_printf("[lwip] nonetif\n");
        return -1;
    }
    s3_ets_printf("[lwip] sd ok\n");
    /* §960: §959 把 tcpip 线程真正拉起来之后，dhcp_start 就不能再从这个
     * （WiFi 主任务）上下文直接调了：它要 udp_new/udp_bind/udp_connect 并挂
     * dhcp_fine_tmr，而 tcpip 线程此刻正在跑 sys_timeouts —— 两边并发改同一份
     * dhcp 状态。tcpip_callback() 是**阻塞版**（tcpip.h:88 的宏
     * `block != 0 ? tcpip_callback : tcpip_try_callback` 印证），
     * 回调在 tcpip 线程里执行完才返回。 */
    /* §1501 ★ 改用**非阻塞版**。`tcpip_callback` 会阻塞等 tcpip 线程执行完，
     * 而 tcpip 线程（prio 2）只能在我们让出时运行，我们又要等它回来 ——
     * 真板表现就是 `[wifi] 已关联` 之后**再无任何输出**。
     * `tcpip_try_callback` 只投递消息、立刻返回，tcpip 线程下次拿到 CPU
     * 时自然会执行它。 */
    if (tcpip_try_callback(c6_dhcp_start_cb, (void *)netif_default) != ERR_OK) {
        s3_ets_printf("[lwip] postfail\n");
    }
    LWSTAGE(0x212);
    return 0;
}

/* §959: NO_SYS=0 下超时由 tcpip 线程自己驱动，**不能再从别的线程**调
 * sys_check_timeouts() —— 那会和 tcpip 线程并发改同一份 timer 链表。
 * 留这个空函数只是为了不动调用点。 */
void c6_lwip_poll(void)
{
}

/* ---- 等 DHCP 拿到地址（§215）------------------------------------------
 * WiFi 事件系统我们没接，所以"拿到 IP 没有"直接看 netif 的地址：DHCP 由
 * tcpip 线程驱动（§959），这里只定期让出 CPU 并检查地址。
 * 返回 1 = 拿到地址（非 0 且不是 0.0.0.0）。 */
extern unsigned int s3_w2c_task_delay(unsigned int ticks);

int c6_lwip_wait_ip(int ms)
{
    int waited = 0;

    while (waited < ms) {
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
    LWSTAGE2(1, 0x2111u);
    if (netif_default == 0) {
        s3_ets_printf("[lwip] netif 不存在\n");
        return;
    }
    {
        const ip4_addr_t *a = netif_ip4_addr(netif_default);
        const ip4_addr_t *m = netif_ip4_netmask(netif_default);
        const ip4_addr_t *g = netif_ip4_gw(netif_default);
        const u8_t *ab = (const u8_t *)a;
        const u8_t *mb = (const u8_t *)m;
        const u8_t *gb = (const u8_t *)g;

        s3_ets_printf("[lwip] ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
                   ab[0], ab[1], ab[2], ab[3], mb[0], mb[1], mb[2], mb[3],
                   gb[0], gb[1], gb[2], gb[3]);
    }
}
