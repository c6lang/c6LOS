/* G3 主机单元：wire 辅助（NUL JSON 取值、base64、PNG unfilter）不依赖内核。 */
#define C6CDP_NO_MAIN
#include "../../user/c6chrome-display.c"

int main(void)
{
    int fail = 0;
    uint8_t out[64];
    char s[64];
    long v = 0;

    if (b64_decode("aGVsbG8=", out, sizeof(out)) != 5 ||
        memcmp(out, "hello", 5) != 0) {
        dprintf(2, "FAIL: base64\n");
        fail++;
    }
    if (b64_decode("aGVsbG8", out, sizeof(out)) != 5 ||
        memcmp(out, "hello", 5) != 0) {
        dprintf(2, "FAIL: base64 无 padding\n");
        fail++;
    }
    if (json_str("{\"method\":\"Page.screencastFrame\",\"params\":{\"x\":1}}",
                 "method", s, sizeof(s)) != 0 ||
        strcmp(s, "Page.screencastFrame") != 0) {
        dprintf(2, "FAIL: json_str\n");
        fail++;
    }
    if (json_int("{\"sessionId\":\"abc\",\"params\":{\"sessionId\":42}}",
                 "sessionId", &v) != 0 || v != 42) {
        dprintf(2, "FAIL: json_int 数字优先\n");
        fail++;
    }
    if (fail == 0) {
        dprintf(1, "PASS: cdp host (base64/json)\n");
        return 0;
    }
    return 1;
}
