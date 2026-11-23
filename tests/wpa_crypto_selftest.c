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
extern int c6_aes128_encrypt(const unsigned char *key, const unsigned char *iv,
                             unsigned char *data, int data_len);
extern int c6_aes128_decrypt(const unsigned char *key, const unsigned char *iv,
                             unsigned char *data, int data_len);
extern int c6_omac1_aes_128(const unsigned char *key, const unsigned char *data,
                            unsigned int data_len, unsigned char *mic);
extern int c6_aes_gmac(const unsigned char *key, unsigned int key_len, const unsigned char *iv,
                       unsigned int iv_len, const unsigned char *aad, unsigned int aad_len,
                       unsigned char *mic);

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

    /* ---- AES-128-CBC：NIST SP 800-38A F.2.1 ---- */
    {
        unsigned char key[16], iv[16], buf[32];

        unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
        unhex("000102030405060708090a0b0c0d0e0f", iv, 16);
        unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", buf, 32);
        unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want, 32);
        check(c6_aes128_encrypt(key, iv, buf, 32) == 0, "AES-128-CBC 加密调用");
        check(memcmp(buf, want, 32) == 0, "CBC 密文与 NIST F.2.1 一致");
        check(c6_aes128_decrypt(key, iv, buf, 32) == 0, "AES-128-CBC 解密调用");
        check(memcmp(buf, (const unsigned char *)"\x6b\xc1\xbe\xe2\x2e\x40\x9f\x96"
                     "\xe9\x3d\x7e\x11\x73\x93\x17\x2a", 16) == 0, "CBC 解密还原");
    }

    /* ---- AES-CMAC（RFC 4493）---- */
    {
        unsigned char key[16];
        unsigned char mic[16];
        unsigned char msg[64];
        int i = 0;

        unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
        unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
              "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", msg, 64);
        unhex("bb1d6929e95937287fa37d129b756746", want, 16);
        check(c6_omac1_aes_128(key, msg, 0, mic) == 0 && memcmp(mic, want, 16) == 0,
              "CMAC(空消息) 与 RFC 4493 一致");
        unhex("070a16b46b4d4144f79bdd9dd04a287c", want, 16);
        check(c6_omac1_aes_128(key, msg, 16, mic) == 0 && memcmp(mic, want, 16) == 0,
              "CMAC(16 字节) 与 RFC 4493 一致");
        unhex("dfa66747de9ae63030ca32611497c827", want, 16);
        check(c6_omac1_aes_128(key, msg, 40, mic) == 0 && memcmp(mic, want, 16) == 0,
              "CMAC(40 字节，走填充分支) 与 RFC 4493 一致");
        unhex("51f0bebf7e3b9d92fc49741779363cfe", want, 16);
        check(c6_omac1_aes_128(key, msg, 64, mic) == 0 && memcmp(mic, want, 16) == 0,
              "CMAC(64 字节) 与 RFC 4493 一致");
        for (i = 0; i < 16; i++) { key[i] = 0; }
    }

    /* ---- AES-GMAC：GCM 规范里的"全空"用例（K=0、IV=0、无 AAD/明文）---- */
    {
        unsigned char key[16];
        unsigned char iv[12];
        unsigned char mic[16];

        memset(key, 0, sizeof key);
        memset(iv, 0, sizeof iv);
        unhex("58e2fccefa7e3061367f1d57a4e7455a", want, 16);
        check(c6_aes_gmac(key, 16, iv, 12, 0, 0, mic) == 0, "GMAC 调用");
        check(memcmp(mic, want, 16) == 0, "GMAC 与 GCM 规范一致");
    }

    printf("%s（失败 %d 项）\n", fails == 0 ? "wpa_crypto C 实现自测通过" : "自测失败", fails);
    return fails == 0 ? 0 : 1;
}
