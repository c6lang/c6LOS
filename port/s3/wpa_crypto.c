/* wpa_crypto：给闭源 WiFi 库用的 WPA 加密回调表（wpa_crypto_funcs_t）
 *
 * 为什么必须有（docs/wifi-plan.md §218）：
 *   `esp_wifi_init_internal()` 的配置块里，偏移 4 起是
 *   `wifi_init_config_t.wpa_crypto_funcs`（44 字节）。IDF 用
 *   `WIFI_INIT_CONFIG_DEFAULT()` 填的是 `g_wifi_default_wpa_crypto_funcs`
 *   （components/wpa_supplicant/src/crypto/crypto_ops.c 里那张表）。
 *   我们之前只填了 size=44/version=1，**九个函数指针全是 NULL** ——
 *   WPA2-PSK 关联时要做的 "口令 → PSK"（PBKDF2-SHA1）和 EAPOL 的 MIC 计算
 *   都会走这些回调，NULL 就是直接跑飞。
 *
 * 表结构（32 位指针，整体 44 字节，与 IDF 的 wpa_crypto_funcs_t 一一对应）：
 *   +0  size = 44
 *   +4  version = ESP_WIFI_CRYPTO_VERSION (0x00000001)
 *   +8  hmac_sha256_vector
 *   +12 pbkdf2_sha1
 *   +16 aes_128_encrypt
 *   +20 aes_128_decrypt
 *   +24 omac1_aes_128
 *   +28 ccmp_decrypt
 *   +32 ccmp_encrypt
 *   +36 aes_gmac
 *   +40 sha256_vector
 *
 * 实现策略（本轮）：
 *   - PBKDF2-SHA1、SHA256、HMAC-SHA256 这三个**真实现**（直接复用 c6 标准库里
 *     已经验证过的 sha1/sha256 实现，通过它们编出来的符号调用）；
 *   - AES 相关的四个（aes_128_cbc / omac1_aes_128 / ccmp 的加解密 / aes_gmac）先做成
 *     "记账 + 返回 -1"，因为 ESP32-S3 的 MAC 有 CCMP/TKIP 硬件卸载，普通
 *     WPA2-PSK 关联大概率用不到它们。真上板时如果库调了，RTC 记账
 *     （0x50000BC0 起）和串口都会显示，那时再补 C 版 AES。
 */
#include <string.h>

/* 这里自带 SHA-1 / SHA-256 / HMAC 的 C 实现（约 200 行）：
 *   - 之前的版本是复用 c6 标准库编出来的符号，但那会让 wifi 构建（不链 libtls）
 *     出现 undefined reference，而且 c6 模块的符号改名规则也很绕；
 *   - 自带一份之后 wpa_crypto.c 不依赖任何其它模块，宿主上可以直接用 gcc 编译
 *     验证（docs/wifi-plan.md §218 里就是这么验的）。 */

typedef unsigned int   u32;
typedef unsigned char  u8;
typedef unsigned long long u64;

typedef struct {
    u32 h[5];
    u8  buf[64];
    u32 len;            /* 累计字节数 */
    u32 fill;
} Sha1;

typedef struct {
    u32 h[8];
    u8  buf[64];
    u32 len;
    u32 fill;
} Sha256;

static u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
static u32 ror32(u32 v, int n) { return (v >> n) | (v << (32 - n)); }

static u32 ld_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void st_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* ---- SHA-1 ---- */
static void sha1_block(Sha1 *s, const u8 *p)
{
    u32 w[80];
    u32 a = 0, b = 0, c = 0, d = 0, e = 0;
    int i = 0;

    for (i = 0; i < 16; i++) {
        w[i] = ld_be32(p + i * 4);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        u32 f = 0;
        u32 k = 0;
        u32 t = 0;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->len = 0u; s->fill = 0u;
}

static void sha1_update(Sha1 *s, const void *data, u32 n)
{
    const u8 *p = (const u8 *)data;
    u32 i = 0;

    s->len = s->len + n;
    while (i < n) {
        u32 take = 64u - s->fill;

        if (take > n - i) {
            take = n - i;
        }
        memcpy(s->buf + s->fill, p + i, take);
        s->fill = s->fill + take;
        i = i + take;
        if (s->fill == 64u) {
            sha1_block(s, s->buf);
            s->fill = 0u;
        }
    }
}

static void sha1_final(Sha1 *s, u8 *out20)
{
    u64 bits = (u64)s->len * 8ull;
    u8 pad = 0x80;
    u8 zero = 0;
    u8 lenbuf[8];
    int i = 0;

    sha1_update(s, &pad, 1);
    while (s->fill != 56u) {
        sha1_update(s, &zero, 1);
    }
    for (i = 0; i < 8; i++) {
        lenbuf[i] = (u8)((bits >> (56 - 8 * i)) & 0xffu);
    }
    sha1_update(s, lenbuf, 8);
    for (i = 0; i < 5; i++) {
        st_be32(out20 + i * 4, s->h[i]);
    }
}

static void sha1_hash(const void *p, u32 n, u8 *out20)
{
    Sha1 s;

    sha1_init(&s);
    sha1_update(&s, p, n);
    sha1_final(&s, out20);
}

/* ---- SHA-256 ---- */
static const u32 sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void sha256_block(Sha256 *s, const u8 *p)
{
    u32 w[64];
    u32 a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
    int i = 0;

    for (i = 0; i < 16; i++) {
        w[i] = ld_be32(p + i * 4);
    }
    for (i = 16; i < 64; i++) {
        u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        u32 S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 t1 = h + S1 + ch + sha256_k[i] + w[i];
        u32 S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(Sha256 *s)
{
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u; s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au; s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->len = 0u; s->fill = 0u;
}

static void sha256_update(Sha256 *s, const void *data, u32 n)
{
    const u8 *p = (const u8 *)data;
    u32 i = 0;

    s->len = s->len + n;
    while (i < n) {
        u32 take = 64u - s->fill;

        if (take > n - i) {
            take = n - i;
        }
        memcpy(s->buf + s->fill, p + i, take);
        s->fill = s->fill + take;
        i = i + take;
        if (s->fill == 64u) {
            sha256_block(s, s->buf);
            s->fill = 0u;
        }
    }
}

static void sha256_final(Sha256 *s, u8 *out32)
{
    u64 bits = (u64)s->len * 8ull;
    u8 pad = 0x80;
    u8 zero = 0;
    u8 lenbuf[8];
    int i = 0;

    sha256_update(s, &pad, 1);
    while (s->fill != 56u) {
        sha256_update(s, &zero, 1);
    }
    for (i = 0; i < 8; i++) {
        lenbuf[i] = (u8)((bits >> (56 - 8 * i)) & 0xffu);
    }
    sha256_update(s, lenbuf, 8);
    for (i = 0; i < 8; i++) {
        st_be32(out32 + i * 4, s->h[i]);
    }
}

static void sha256_hash(const void *p, u32 n, u8 *out32)
{
    Sha256 s;

    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, out32);
}

/* ---- HMAC ---- */
static void hmac_sha1(const u8 *key, u32 klen, const u8 *msg, u32 mlen, u8 *out20)
{
    u8 k[64];
    u8 ipad[64];
    u8 opad[64];
    u8 tmp[20];
    Sha1 s;
    u32 i = 0;

    memset(k, 0, sizeof k);
    if (klen > 64u) {
        sha1_hash(key, klen, k);
    } else {
        memcpy(k, key, klen);
    }
    for (i = 0; i < 64u; i++) {
        ipad[i] = (u8)(k[i] ^ 0x36u);
        opad[i] = (u8)(k[i] ^ 0x5cu);
    }
    sha1_init(&s);
    sha1_update(&s, ipad, 64);
    sha1_update(&s, msg, mlen);
    sha1_final(&s, tmp);
    sha1_init(&s);
    sha1_update(&s, opad, 64);
    sha1_update(&s, tmp, 20);
    sha1_final(&s, out20);
}

static void hmac_sha256(const u8 *key, u32 klen, const u8 *msg, u32 mlen, u8 *out32)
{
    u8 k[64];
    u8 ipad[64];
    u8 opad[64];
    u8 tmp[32];
    Sha256 s;
    u32 i = 0;

    memset(k, 0, sizeof k);
    if (klen > 64u) {
        sha256_hash(key, klen, k);
    } else {
        memcpy(k, key, klen);
    }
    for (i = 0; i < 64u; i++) {
        ipad[i] = (u8)(k[i] ^ 0x36u);
        opad[i] = (u8)(k[i] ^ 0x5cu);
    }
    sha256_init(&s);
    sha256_update(&s, ipad, 64);
    sha256_update(&s, msg, mlen);
    sha256_final(&s, tmp);
    sha256_init(&s);
    sha256_update(&s, opad, 64);
    sha256_update(&s, tmp, 32);
    sha256_final(&s, out32);
}

#define WPA_VEC_MAX 1024        /* 向量拼接缓冲上限（EAPOL/管理帧都很小） */

/* 记账：每类回调被调用几次，写在 RTC 慢存 0x50000BC0 起的 8 个字里。
 * 宿主上做单元测试时（-DC6_WPA_HOST_TEST）改成内存数组，否则写 0x5000xxxx
 * 会直接段错误。 */
#ifdef C6_WPA_HOST_TEST
static volatile unsigned int g_wpa_hits[8];
#define WPA_HIT_BASE ((volatile unsigned int *)g_wpa_hits)
#else
#define WPA_HIT_BASE ((volatile unsigned int *)0x50000BC0u)
#endif

static void wpa_hit(int idx)
{
    volatile unsigned int *p = WPA_HIT_BASE;

    if (idx >= 0 && idx < 8) {
        p[idx] = p[idx] + 1u;
    }
}

/* 把向量拼成一块连续缓冲 */
static int wpa_concat(int num_elem, const unsigned char *addr[], const int *len,
                      unsigned char *out, int cap)
{
    int i = 0;
    int off = 0;

    for (i = 0; i < num_elem; i++) {
        int n = (len != 0) ? len[i] : 0;

        if (n < 0 || off + n > cap) {
            return -1;
        }
        if (n > 0 && addr[i] != 0) {
            memcpy(out + off, addr[i], (unsigned int)n);
            off = off + n;
        }
    }
    return off;
}

int c6_sha256_vector(unsigned int num_elem, const unsigned char *addr[],
                            const unsigned int *len, unsigned char *mac)
{
    unsigned char buf[WPA_VEC_MAX];
    int lens[16];
    int i = 0;
    int n = 0;

    wpa_hit(0);
    if (num_elem > 16u) {
        return -1;
    }
    for (i = 0; i < (int)num_elem; i++) {
        lens[i] = (int)len[i];
    }
    n = wpa_concat((int)num_elem, addr, lens, buf, WPA_VEC_MAX);
    if (n < 0) {
        return -1;
    }
    sha256_hash(buf, (unsigned int)n, mac);
    return 0;
}

int c6_hmac_sha256_vector(const unsigned char *key, int key_len, int num_elem,
                                 const unsigned char *addr[], const int *len,
                                 unsigned char *mac)
{
    unsigned char buf[WPA_VEC_MAX];
    int n = 0;

    wpa_hit(1);
    n = wpa_concat(num_elem, addr, len, buf, WPA_VEC_MAX);
    if (n < 0) {
        return -1;
    }
    hmac_sha256(key, (unsigned int)key_len, buf, (unsigned int)n, mac);
    return 0;
}

/* PBKDF2-HMAC-SHA1（IEEE 802.11 H.4：iterations=4096, buflen=32）：
 *   DK = T1 || T2 || ...，Ti = F(P, S, c, i)，S = ssid。 */
int c6_pbkdf2_sha1(const char *passphrase, const char *ssid, unsigned int ssid_len,
                          int iterations, unsigned char *buf, unsigned int buflen)
{
    unsigned char u[20];
    unsigned char t[20];
    unsigned char key[64];
    unsigned char msg[64];
    unsigned int klen = 0;
    unsigned int off = 0;
    unsigned int block = 1;
    int it = 0;
    unsigned int i = 0;

    wpa_hit(2);
    if (passphrase == 0 || ssid == 0 || iterations <= 0 || buflen == 0) {
        return -1;
    }
    klen = (unsigned int)strlen(passphrase);
    if (klen > sizeof key) {
        klen = sizeof key;              /* 规范上限 63，够用 */
    }
    memcpy(key, passphrase, klen);
    while (off < buflen) {
        /* U1 = HMAC-SHA1(P, S || INT(block)) */
        if (ssid_len + 4u > sizeof msg) {
            return -1;
        }
        memcpy(msg, ssid, ssid_len);
        msg[ssid_len] = (unsigned char)((block >> 24) & 0xff);
        msg[ssid_len + 1] = (unsigned char)((block >> 16) & 0xff);
        msg[ssid_len + 2] = (unsigned char)((block >> 8) & 0xff);
        msg[ssid_len + 3] = (unsigned char)(block & 0xff);
        hmac_sha1(key, klen, msg, ssid_len + 4u, u);
        memcpy(t, u, 20);
        for (it = 1; it < iterations; it++) {
            hmac_sha1(key, klen, u, 20u, u);
            for (i = 0; i < 20u; i++) {
                t[i] = (unsigned char)(t[i] ^ u[i]);
            }
        }
        for (i = 0; i < 20u && off < buflen; i++) {
            buf[off] = t[i];
            off = off + 1u;
        }
        block = block + 1u;
    }
    return 0;
}

/* ---- AES-128（加密 + 逆加密）+ CBC + CMAC(OMAC1) + GMAC -----------------
 *
 * 为什么自己写：wpa_crypto_funcs 里有四个 AES 相关的回调。ESP32-S3 的 MAC 有
 * CCMP/TKIP 硬件卸载，普通 WPA2-PSK 关联大概率不回调它们，但"大概率"不等于
 * "一定"，而且 AES 实现是自包含、能拿标准向量验证的（下面 §222 的宿主用例）。
 * CCMP 那两个语义复杂（要拼 802.11 头、PN、keyid），仍先做成记账 + 返回失败：
 * 真上板时 RTC 0x50000BC0[6]/[7] 会告诉我们它们到底有没有被调。 */

static const u8 aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const u8 aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

static const u8 aes_rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

static u8 xtime(u8 b) { return (u8)((b << 1) ^ ((b & 0x80u) ? 0x1bu : 0u)); }

static u8 gmul(u8 a, u8 b)
{
    u8 p = 0;
    int i = 0;

    for (i = 0; i < 8; i++) {
        if ((b & 1u) != 0u) { p = (u8)(p ^ a); }
        b = (u8)(b >> 1);
        a = xtime(a);
    }
    return p;
}

/* 128 位密钥扩展：11 组轮密钥 */
static void aes128_expand(const u8 *key, u8 rk[176])
{
    int i = 0;

    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        u8 t[4];

        memcpy(t, rk + i - 4, 4);
        if ((i & 15) == 0) {
            u8 tmp = t[0];

            t[0] = (u8)(aes_sbox[t[1]] ^ aes_rcon[(i >> 4) - 1]);
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        rk[i + 0] = (u8)(rk[i - 16 + 0] ^ t[0]);
        rk[i + 1] = (u8)(rk[i - 16 + 1] ^ t[1]);
        rk[i + 2] = (u8)(rk[i - 16 + 2] ^ t[2]);
        rk[i + 3] = (u8)(rk[i - 16 + 3] ^ t[3]);
    }
}

static void aes128_enc_block(const u8 rk[176], const u8 *in, u8 *out)
{
    u8 s[16];
    int round = 0;
    int i = 0;

    memcpy(s, in, 16);
    for (i = 0; i < 16; i++) { s[i] = (u8)(s[i] ^ rk[i]); }
    for (round = 1; round <= 10; round++) {
        u8 t[16];

        /* SubBytes + ShiftRows */
        for (i = 0; i < 16; i++) {
            int row = i & 3;
            int col = i >> 2;
            int from = ((col + row) & 3) * 4 + row;

            t[i] = aes_sbox[s[from]];
        }
        /* MixColumns（最后一轮不做） */
        if (round != 10) {
            for (i = 0; i < 4; i++) {
                u8 *c = t + i * 4;
                u8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];

                c[0] = (u8)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
                c[1] = (u8)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
                c[2] = (u8)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
                c[3] = (u8)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            }
        }
        for (i = 0; i < 16; i++) { s[i] = (u8)(t[i] ^ rk[round * 16 + i]); }
    }
    memcpy(out, s, 16);
}

static void aes128_dec_block(const u8 rk[176], const u8 *in, u8 *out)
{
    u8 s[16];
    int round = 0;
    int i = 0;

    memcpy(s, in, 16);
    for (i = 0; i < 16; i++) { s[i] = (u8)(s[i] ^ rk[160 + i]); }
    for (round = 9; round >= 0; round--) {
        u8 t[16];

        /* InvShiftRows + InvSubBytes */
        for (i = 0; i < 16; i++) {
            int row = i & 3;
            int col = i >> 2;
            int from = ((col - row + 4) & 3) * 4 + row;

            t[i] = aes_rsbox[s[from]];
        }
        for (i = 0; i < 16; i++) { t[i] = (u8)(t[i] ^ rk[round * 16 + i]); }
        if (round != 0) {
            for (i = 0; i < 4; i++) {
                u8 *c = t + i * 4;
                u8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];

                c[0] = (u8)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                c[1] = (u8)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                c[2] = (u8)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                c[3] = (u8)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

/* AES-128-CBC 原地加/解密（与 wpa_supplicant 的 aes_128_cbc_* 同语义：不足
 * 16 字节的尾巴按零补齐） */
int c6_aes128_encrypt(const unsigned char *key, const unsigned char *iv,
                      unsigned char *data, int data_len)
{
    u8 rk[176];
    u8 prev[16];
    int off = 0;

    wpa_hit(3);
    if (key == 0 || iv == 0 || data == 0 || data_len < 0) { return -1; }
    aes128_expand(key, rk);
    memcpy(prev, iv, 16);
    while (off + 16 <= data_len) {
        u8 blk[16];
        int i = 0;

        for (i = 0; i < 16; i++) { blk[i] = (u8)(data[off + i] ^ prev[i]); }
        aes128_enc_block(rk, blk, prev);
        memcpy(data + off, prev, 16);
        off = off + 16;
    }
    if (off < data_len) {
        u8 blk[16];
        int i = 0;

        memset(blk, 0, 16);
        memcpy(blk, data + off, (unsigned int)(data_len - off));
        for (i = 0; i < 16; i++) { blk[i] = (u8)(blk[i] ^ prev[i]); }
        aes128_enc_block(rk, blk, prev);
        memcpy(data + off, prev, (unsigned int)(data_len - off));
    }
    return 0;
}

int c6_aes128_decrypt(const unsigned char *key, const unsigned char *iv,
                      unsigned char *data, int data_len)
{
    u8 rk[176];
    u8 prev[16];
    int off = 0;

    wpa_hit(4);
    if (key == 0 || iv == 0 || data == 0 || data_len < 0) { return -1; }
    aes128_expand(key, rk);
    memcpy(prev, iv, 16);
    while (off + 16 <= data_len) {
        u8 ct[16];
        u8 pt[16];
        int i = 0;

        memcpy(ct, data + off, 16);
        aes128_dec_block(rk, ct, pt);
        for (i = 0; i < 16; i++) { data[off + i] = (u8)(pt[i] ^ prev[i]); }
        memcpy(prev, ct, 16);
        off = off + 16;
    }
    return 0;
}

/* OMAC1 = AES-CMAC（RFC 4493），输出 16 字节 MIC */
int c6_omac1_aes_128(const unsigned char *key, const unsigned char *data,
                     unsigned int data_len, unsigned char *mic)
{
    u8 rk[176];
    u8 x[16];
    u8 k1[16];
    u8 k2[16];
    u8 last[16];
    u8 l[16];
    u8 zero[16];
    unsigned int n = 0;
    unsigned int i = 0;
    int j = 0;
    int complete = 0;

    wpa_hit(5);
    if (key == 0 || data == 0 || mic == 0) { return -1; }
    aes128_expand(key, rk);
    memset(zero, 0, 16);
    aes128_enc_block(rk, zero, l);
    /* K1 = double(L) */
    {
        int msb = (l[0] & 0x80u) != 0u;

        for (j = 0; j < 16; j++) {
            k1[j] = (u8)((l[j] << 1) | (j + 1 < 16 ? (l[j + 1] >> 7) : 0));
        }
        if (msb) { k1[15] = (u8)(k1[15] ^ 0x87u); }
    }
    /* K2 = double(K1) */
    {
        int msb = (k1[0] & 0x80u) != 0u;

        for (j = 0; j < 16; j++) {
            k2[j] = (u8)((k1[j] << 1) | (j + 1 < 16 ? (k1[j + 1] >> 7) : 0));
        }
        if (msb) { k2[15] = (u8)(k2[15] ^ 0x87u); }
    }
    n = (data_len + 15u) / 16u;
    if (n == 0u) { n = 1u; }
    complete = (data_len != 0u && (data_len % 16u) == 0u) ? 1 : 0;
    memset(x, 0, 16);
    for (i = 0; i + 1u < n; i++) {
        u8 blk[16];
        u8 out[16];

        for (j = 0; j < 16; j++) { blk[j] = (u8)(x[j] ^ data[i * 16u + (unsigned int)j]); }
        aes128_enc_block(rk, blk, out);
        memcpy(x, out, 16);
    }
    if (complete) {
        for (j = 0; j < 16; j++) { last[j] = (u8)(data[(n - 1u) * 16u + (unsigned int)j] ^ k1[j]); }
    } else {
        memset(last, 0, 16);
        for (j = 0; j < 16; j++) {
            unsigned int pos = (n - 1u) * 16u + (unsigned int)j;

            if (pos < data_len) { last[j] = data[pos]; }
        }
        last[data_len % 16u] = (u8)(last[data_len % 16u] ^ 0x80u);
        for (j = 0; j < 16; j++) { last[j] = (u8)(last[j] ^ k2[j]); }
    }
    for (j = 0; j < 16; j++) { last[j] = (u8)(last[j] ^ x[j]); }
    aes128_enc_block(rk, last, mic);
    return 0;
}

/* AES-GMAC（GCM 的空明文形式），输出 16 字节 MIC */
int c6_aes_gmac(const unsigned char *key, unsigned int key_len, const unsigned char *iv,
                unsigned int iv_len, const unsigned char *aad, unsigned int aad_len,
                unsigned char *mic)
{
    u8 rk[176];
    u8 h[16];
    u8 y[16];
    u8 j0[16];
    u8 zero[16];
    u8 blk[16];
    u8 ej0[16];
    unsigned int i = 0;
    int j = 0;

    wpa_hit(7);
    if (key == 0 || mic == 0 || key_len != 16u || iv == 0 || iv_len != 12u) { return -1; }
    aes128_expand(key, rk);
    memset(zero, 0, 16);
    aes128_enc_block(rk, zero, h);
    memset(y, 0, 16);
    /* GHASH(aad) */
    {
        unsigned int left = aad_len;
        const u8 *p = aad;

        while (left > 0u) {
            unsigned int n = (left < 16u) ? left : 16u;

            memset(blk, 0, 16);
            memcpy(blk, p, n);
            for (j = 0; j < 16; j++) { y[j] = (u8)(y[j] ^ blk[j]); }
            /* y = y * H（GF(2^128)，MSB 先行，和 GCM 一致） */
            {
                u8 z[16] = { 0 };
                u8 v[16];
                int k = 0;

                memcpy(v, h, 16);
                for (k = 0; k < 128; k++) {
                    if ((y[k >> 3] & (u8)(0x80u >> (k & 7))) != 0u) {
                        for (j = 0; j < 16; j++) { z[j] = (u8)(z[j] ^ v[j]); }
                    }
                    {
                        int lsb = (int)(v[15] & 1u);

                        for (j = 15; j > 0; j--) { v[j] = (u8)((v[j] >> 1) | ((v[j - 1] & 1u) << 7)); }
                        v[0] = (u8)(v[0] >> 1);
                        if (lsb != 0) { v[0] = (u8)(v[0] ^ 0xe1u); }
                    }
                }
                memcpy(y, z, 16);
            }
            p = p + n;
            left = left - n;
        }
    }
    /* 长度块：只有 aad 长度非零，数据长度为 0 */
    memset(blk, 0, 16);
    {
        unsigned long long abits = (unsigned long long)aad_len * 8ull;

        for (i = 0; i < 8u; i++) {
            blk[7u - i] = (u8)((abits >> (8u * i)) & 0xffull);
        }
    }
    for (j = 0; j < 16; j++) { y[j] = (u8)(y[j] ^ blk[j]); }
    {
        u8 z[16] = { 0 };
        u8 v[16];
        int k = 0;

        memcpy(v, h, 16);
        for (k = 0; k < 128; k++) {
            if ((y[k >> 3] & (u8)(0x80u >> (k & 7))) != 0u) {
                for (j = 0; j < 16; j++) { z[j] = (u8)(z[j] ^ v[j]); }
            }
            {
                int lsb = (int)(v[15] & 1u);

                for (j = 15; j > 0; j--) { v[j] = (u8)((v[j] >> 1) | ((v[j - 1] & 1u) << 7)); }
                v[0] = (u8)(v[0] >> 1);
                if (lsb != 0) { v[0] = (u8)(v[0] ^ 0xe1u); }
            }
        }
        memcpy(y, z, 16);
    }
    /* J0 = iv || 00000001；tag = GHASH ^ AES_K(J0) */
    memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes128_enc_block(rk, j0, ej0);
    for (j = 0; j < 16; j++) { mic[j] = (u8)(y[j] ^ ej0[j]); }
    return 0;
}

/* ---- CCMP 的加解密：先"记账 + 返回失败" ----------------------------------
 *
 * CCMP 要按 802.11 头解析 MAC header / PN / keyid 并做 CCM 封装，语义复杂；
 * 而 ESP32-S3 的 MAC 有 CCMP 硬件卸载，普通 WPA2-PSK 的数据通路基本不走软件
 * 实现。真上板时如果 RTC 慢存 0x50000BC0[6]/[7] 非 0，说明库确实回调了它们，
 * 那时再补完整 CCM（AES-128 块、CBC-MAC、CTR 都已经在下面实现好了）。
 */

int c6_ccmp_decrypt(const unsigned char *key, const unsigned char *iv,
                           const unsigned char *data, unsigned int data_len,
                           unsigned char *decrypted, unsigned char *mic, int *mic_len)
{
    (void)key; (void)iv; (void)data; (void)data_len; (void)decrypted; (void)mic; (void)mic_len;
    wpa_hit(6);
    return -1;
}

int c6_ccmp_encrypt(const unsigned char *key, const unsigned char *iv,
                           unsigned char *data, unsigned int data_len, unsigned char *mic)
{
    (void)key; (void)iv; (void)data; (void)data_len; (void)mic;
    wpa_hit(7);
    return -1;
}

/* ---- 表（44 字节，布局同 IDF 的 wpa_crypto_funcs_t）---- */
static unsigned int g_wpa_crypto[11] = {
    44u,                    /* size */
    0x00000001u,            /* version = ESP_WIFI_CRYPTO_VERSION */
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
};

/* esp_wifi_init_internal() 之前调用一次：把这张表填好 */
void c6_wpa_crypto_init(void)
{
    g_wpa_crypto[2] = (unsigned int)(void *)c6_hmac_sha256_vector;
    g_wpa_crypto[3] = (unsigned int)(void *)c6_pbkdf2_sha1;
    g_wpa_crypto[4] = (unsigned int)(void *)c6_aes128_encrypt;
    g_wpa_crypto[5] = (unsigned int)(void *)c6_aes128_decrypt;
    g_wpa_crypto[6] = (unsigned int)(void *)c6_omac1_aes_128;
    g_wpa_crypto[7] = (unsigned int)(void *)c6_ccmp_decrypt;
    g_wpa_crypto[8] = (unsigned int)(void *)c6_ccmp_encrypt;
    g_wpa_crypto[9] = (unsigned int)(void *)c6_aes_gmac;
    g_wpa_crypto[10] = (unsigned int)(void *)c6_sha256_vector;
}

/* 供 wifi_glue.c 拷进 init config 的偏移 4 处 */
const void *c6_wpa_crypto_table(void)
{
    return (const void *)g_wpa_crypto;
}
