/* tests/wpa_crypto_selftest.c —— 宿主上用 gcc 直接测 port/s3/wpa_crypto.c
 *
 * wpa_crypto.c 现在是**自带** SHA-1/SHA-256/HMAC 的普通 C（不依赖 c6 模块），
 * 所以可以直接用宿主 gcc 编译验证，向量全部是公开标准值：
 *   - PBKDF2-HMAC-SHA1：IEEE 802.11i（password + IEEE → PSK）
 *   - SHA-256("abc")：NIST FIPS 180-2
 *   - HMAC-SHA256：RFC 4231 case 2（顺便验证"多段向量拼接"这条路径）
 *
 * 跑法：./tools/tls12_selftest.sh（里面用 gcc -DC6_WPA_HOST_TEST 编）
 */
#include <stdio.h>
#include <string.h>

extern int c6_pbkdf2_sha1(const char *pass, const char *ssid, unsigned int ssid_len,
                          int iterations, unsigned char *buf, unsigned int buflen);
extern int c6_sha256_vector(unsigned int num_elem, const unsigned char *addr[],
                            const unsigned int *len, unsigned char *mac);
extern int c6_hmac_sha256_vector(const unsigned char *key, int key_len, int num_elem,
                                 const unsigned char *addr[], const int *len,
                                 unsigned char *mac);

static int fails = 0;

static int unhex(const char *s, unsigned char *out, int cap)
{
    int n = 0;

    while (s[0] && s[1] && n < cap) {
        int hi = (s[0] <= '9') ? s[0] - '0' : s[0] - 'a' + 10;
        int lo = (s[1] <= '9') ? s[1] - '0' : s[1] - 'a' + 10;

        out[n++] = (unsigned char)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static void check(int cond, const char *what)
{
    if (cond) {
        printf("   ok   %s\n", what);
    } else {
        printf("   FAIL %s\n", what);
        fails++;
    }
}

int main(void)
{
    unsigned char psk[32], want[32], mac[32], hmac[32];
    const unsigned char *vec[2];
    unsigned int lens[1];
    int ilens[2];

    unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", want, 32);
    check(c6_pbkdf2_sha1("password", "IEEE", 4, 4096, psk, 32) == 0, "pbkdf2 调用");
    check(memcmp(psk, want, 32) == 0, "PSK 与 IEEE 802.11i 向量一致");

    unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    vec[0] = (const unsigned char *)"abc";
    lens[0] = 3;
    check(c6_sha256_vector(1, vec, lens, mac) == 0, "sha256_vector 调用");
    check(memcmp(mac, want, 32) == 0, "SHA-256(abc) 与 NIST 向量一致");

    unhex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want, 32);
    vec[0] = (const unsigned char *)"what do ya want ";
    vec[1] = (const unsigned char *)"for nothing?";
    ilens[0] = 16;
    ilens[1] = 12;
    check(c6_hmac_sha256_vector((const unsigned char *)"Jefe", 4, 2, vec, ilens, hmac) == 0,
          "hmac_sha256_vector 调用");
    check(memcmp(hmac, want, 32) == 0, "HMAC-SHA256 与 RFC 4231 向量一致");

    printf("%s（失败 %d 项）\n", fails == 0 ? "wpa_crypto C 实现自测通过" : "自测失败", fails);
    return fails == 0 ? 0 : 1;
}
