/* WiFi 账号（STA）—— 板子要联网就必须填这里，或者用 make 变量传：
 *
 *     make C6_ROOT=/home/winger/c6 https S3_WIFI_SSID='"MyAP"' S3_WIFI_PASS='"secret"'
 *
 * 注意 make 变量里的引号要写成 '"..."'（双引号是 C 字符串的一部分）。
 * 留空则会跳过关联，串口打印 "[wifi] 没给 SSID，跳过关联"。
 */
#ifndef S3_WIFI_SSID
#define S3_WIFI_SSID ""
#endif
#ifndef S3_WIFI_PASS
#define S3_WIFI_PASS ""
#endif
