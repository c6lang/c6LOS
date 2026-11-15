/* c6_https_demo：板级 HTTPS 演示（windowed C）
 *
 * 位置说明（docs/wifi-plan.md §214）：
 *   TLS 1.2 客户端是用 .c6 写的（net/tls12_hs.c6 等），编成 **windowed** 静态库
 *   `libtls.a`；lwIP 也是 windowed（§210）。这个文件是它们的**调用者**，同样按
 *   windowed 编译，所以整条链上没有 ABI 混用，只有内核侧（call0）通过 w2c 壳
 *   间接参与（本文件不碰内核）。
 *
 * 流程：WiFi bringup 成功 → lwIP 起来（DHCP 拿到地址）→ 本函数
 *   getaddrinfo("qq.com") → connect(:443) → TLS 1.2 握手 → HTTP GET /
 *   → 打印状态行和页面开头。
 *
 * 失败时把阶段号打出来（1=已发 ClientHello，2=ServerHello，11=Certificate，
 * 12=ServerKeyExchange，20=等服务端 Finished，21=握手完成）。
 */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/errno.h"

/* ---- libtls.a 里 c6 模块导出的符号（名字就是 c6 的模块修饰名）---- */
extern int  net__tls12_hs__tls12_state_size(void);
extern int  net__tls12_hs__tls12_set_buffers(void *rec, int rec_cap, void *enc, int enc_cap,
                                            void *out, int out_cap, void *hsbuf, int hsbuf_cap);
extern int  net__tls12_hs__tls12_start(void *st, int fd, const char *host,
                                       unsigned long long seed);
extern int  net__tls12_hs__tls12_handshake(void *st);
extern int  net__tls12_hs__tls12_write_app(void *st, const void *p, int n);
extern int  net__tls12_hs__tls12_read_app(void *st, void *p, int cap);
extern int  net__tls12_hs__tls12_stage(void *st);
extern int  net__tls12_hs__tls12_trace;

/* 本地小工具：不引 strstr（freestanding 镜像里没有），只做一次子串查找 */
static const char *find_str(const char *hay, const char *needle)
{
    const char *h = hay;

    while (*h != 0) {
        const char *p = h;
        const char *n = needle;

        while (*n != 0 && *p == *n) {
            p++;
            n++;
        }
        if (*n == 0) {
            return h;
        }
        h++;
    }
    return 0;
}

/* 板级串口打印（windowed 侧由 wifi_glue.c 提供） */
extern int  printf(const char *fmt, ...);
extern int  snprintf(char *out, unsigned int cap, const char *fmt, ...);

/* TLS 状态和页缓冲都从堆上要（板子 DRAM 只剩 ~10KB，§210）：
 * 板子上 malloc = osi_malloc（64KB 的 OSI 堆），宿主上是 libc 的 malloc。 */
extern void *malloc(unsigned int n);
static unsigned char *s_tls_state = 0;
static unsigned char *s_tls_rec = 0;
static unsigned char *s_tls_enc = 0;
static unsigned char *s_tls_out = 0;
static unsigned char *s_tls_hs = 0;

/* 一次 HTTPS GET。返回读到的字节数，<0 表示失败。 */
static int https_get(const char *host, const char *path, char *out, int cap)
{
    struct addrinfo hints;
    struct addrinfo *res = 0;
    struct sockaddr_storage ss;
    int fd = -1;
    int n = 0;
    int total = 0;
    int rounds = 0;
    char req[256] = { 0 };

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (lwip_getaddrinfo(host, "443", &hints, &res) != 0 || res == 0) {
        printf("[https] DNS 解析 %s 失败\n", host);
        return -1;
    }
    fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        printf("[https] socket() 失败\n");
        lwip_freeaddrinfo(res);
        return -1;
    }
    memcpy(&ss, res->ai_addr, res->ai_addrlen);
    if (lwip_connect(fd, (struct sockaddr *)res->ai_addr, res->ai_addrlen) != 0) {
        printf("[https] connect(%s:443) 失败\n", host);
        lwip_close(fd);
        lwip_freeaddrinfo(res);
        return -1;
    }
    lwip_freeaddrinfo(res);
    (void)ss;

    if (s_tls_rec == 0) {
        s_tls_rec = (unsigned char *)malloc(6400);
        s_tls_enc = (unsigned char *)malloc(2048);
        s_tls_out = (unsigned char *)malloc(1024);
        s_tls_hs = (unsigned char *)malloc(6400);
        if (s_tls_rec == 0 || s_tls_enc == 0 || s_tls_out == 0 || s_tls_hs == 0) {
            printf("[https] TLS 缓冲申请失败\n");
            lwip_close(fd);
            return -1;
        }
        (void)net__tls12_hs__tls12_set_buffers(s_tls_rec, 6400, s_tls_enc, 2048,
                                               s_tls_out, 1024, s_tls_hs, 6400);
    }
    if (s_tls_state == 0) {
        s_tls_state = (unsigned char *)malloc((unsigned int)net__tls12_hs__tls12_state_size());
        if (s_tls_rec == 0) {
        s_tls_rec = (unsigned char *)malloc(6400);
        s_tls_enc = (unsigned char *)malloc(2048);
        s_tls_out = (unsigned char *)malloc(1024);
        s_tls_hs = (unsigned char *)malloc(6400);
        if (s_tls_rec == 0 || s_tls_enc == 0 || s_tls_out == 0 || s_tls_hs == 0) {
            printf("[https] TLS 缓冲申请失败\n");
            lwip_close(fd);
            return -1;
        }
        (void)net__tls12_hs__tls12_set_buffers(s_tls_rec, 6400, s_tls_enc, 2048,
                                               s_tls_out, 1024, s_tls_hs, 6400);
    }
    if (s_tls_state == 0) {
            printf("[https] TLS 状态申请失败（%d 字节）\n",
                   net__tls12_hs__tls12_state_size());
            lwip_close(fd);
            return -1;
        }
    }
    if (!net__tls12_hs__tls12_start(s_tls_state, fd, host, 0x1122334455667788ull)) {
        printf("[https] ClientHello 发送失败\n");
        lwip_close(fd);
        return -1;
    }
    if (!net__tls12_hs__tls12_handshake(s_tls_state)) {
        printf("[https] TLS 握手失败，阶段=%d\n", net__tls12_hs__tls12_stage(s_tls_state));
        lwip_close(fd);
        return -1;
    }
    printf("[https] TLS 握手完成（阶段=%d）\n", net__tls12_hs__tls12_stage(s_tls_state));

    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: c6los\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (net__tls12_hs__tls12_write_app(s_tls_state, req, (int)strlen(req)) <= 0) {
        printf("[https] 发送请求失败\n");
        lwip_close(fd);
        return -1;
    }
    for (rounds = 0; rounds < 32; rounds++) {
        n = net__tls12_hs__tls12_read_app(s_tls_state, out + total,
                                          cap - total > 2048 ? 2048 : cap - total);
        if (n == -2 || n < 0) {
            break;
        }
        if (n == 0) {
            continue;
        }
        total = total + n;
        if (total >= cap - 2048) {
            break;
        }
    }
    lwip_close(fd);
    return total;
}

/* 供 win_island.c 的 S3_WITH_HTTPS 分支调用；返回 0 表示成功拿到页面数据 */
int c6_https_demo(void)
{
    char *page = (char *)malloc(4096);

    net__tls12_hs__tls12_trace = 1;
    if (page == 0) {
        printf("[https] 页缓冲申请失败\n");
        return -1;
    }
    memset(page, 0, 4096);
    printf("[https] === 开始取 https://qq.com/ ===\n");
    if (https_get("qq.com", "/", page, 4096) <= 0) {
        printf("[https] 失败\n");
        return -1;
    }
    printf("[https] ---- 响应开头 ----\n");
    if (strlen(page) > 700) {
        page[700] = 0;          /* wp_vfmt 不保证支持 %.700s，先手工截断 */
    }
    printf("%s\n", page);
    printf("[https] ---- 响应结尾 ----\n");
    if (find_str(page, "HTTP/1.1") != 0) {
        printf("[https] 成功：拿到了 HTTP 响应\n");
        return 0;
    }
    return -1;
}
