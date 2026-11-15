/* c6LOS 的 lwIP 配置（ESP32-S3 端口，不依赖 sdkconfig.h / FreeRTOS）
 *
 * 为什么要有这个文件（docs/wifi-plan.md §207）：
 *   IDF 的 `components/lwip/port/include/lwipopts.h` 第一行就 `#include "sdkconfig.h"`，
 *   我们这套构建是 `-ffreestanding` 的手工构建，没有 sdkconfig.h，也没有 IDK 的
 *   Kconfig 生成流程。lwIP 本身对平台的要求其实很小（`lwipopts.h` + `arch/cc.h`），
 *   所以自己写一份。
 *
 * 当前阶段：**NO_SYS=1**（单线程 raw API）。
 *   - 优点：完全不需要 sys_arch（信号量/邮箱/线程），要接的只有
 *     ① netif 的 linkoutput（发一帧）② RX 回调（收一帧）③ sys_check_timeouts 的时基。
 *   - 局限：没有 BSD socket。而 `/home/winger/c6/std/net/tcp.c6` 是 POSIX 薄封装
 *     （socket/connect/poll/getaddrinfo），所以**最终目标是 NO_SYS=0 + 自己的 sys_arch**
 *     （我们内核已经有 sem/mutex/queue/task/tick/event，正好是 sys_arch 要的那几个原语）。
 *     到时候把下面 NO_SYS 改成 0、打开 LWIP_SOCKET/LWIP_NETCONN 即可，其余配置不动。
 *
 * 内存：ESP32-S3 的 DRAM 很紧（ORIGIN 0x3FCA0000 LENGTH 0x4F000，见 port/s3/link.ld），
 * 所以优先 MEM_LIBC_MALLOC + MEMP_MEM_MALLOC，让 lwIP 直接用我们内核的堆，
 * 而不是自己再开一大块静态池。等真上板量过余量再回来调。
 */
#ifndef C6_LWIPOPTS_H
#define C6_LWIPOPTS_H

#define NO_SYS                          0   /* 0 = 有自己的 tcpip 线程 + BSD socket（见 sys_arch.c）*/
#define SYS_LIGHTWEIGHT_PROT            0

/* ---- 协议栈 ---- */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_IGMP                       0
#define LWIP_DHCP                       1   /* 拿 IP：家用路由器默认只给 DHCP */
#define LWIP_DNS                        1   /* qq.com 要解析 */
#define LWIP_DNS_SECURE                 0
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0   /* 地址冲突检测：可选，先关 */
#define LWIP_DHCP_DOES_ACD_CHECK        0   /* 关掉就必须显式写 0，否则 init.c 直接 #error */

/* ---- API ---- */
#define LWIP_SOCKET                     1   /* c6 标准库 std/net/tcp.c6 是 POSIX 薄封装 */
#define LWIP_NETCONN                    1
#define MEMP_NUM_NETCONN                4
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  1
#define LWIP_TCP_KEEPALIVE              0
#define LWIP_NETIF_API                  0

/* ---- 内存 ---- */
#define MEM_LIBC_MALLOC                 1
#define MEMP_MEM_MALLOC                 1
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        0   /* 用 libc 的 malloc 时无意义 */
/* DRAM 只剩约 10KB（§210），lwIP 的动态内存和闭源 WiFi 库共用 16KB 的
 * g_wifi_heap_mem。取一次 https://qq.com 的页面不需要大窗口，先按小配置来：
 *   MSS 536（经典保守值，不会分片）、窗口/发送缓冲各 4KB、TCP PCB 上限 4。
 * 等真机上量过 peak 再决定要不要放大。 */
#define TCP_WND                         (4 * 1024)
#define TCP_MSS                         536
#define TCP_SND_BUF                     (4 * 1024)
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_PBUF                   16
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1520

/* ---- 杂项 ---- */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_STATS                      0
#define LWIP_DEBUG                      0
#define LWIP_PROVIDE_ERRNO              1
#define LWIP_COMPAT_SOCKETS             0
#define LWIP_NETIF_TX_SINGLE_PBUF       1   /* WiFi 驱动要连续的一帧 */

#endif /* C6_LWIPOPTS_H */
