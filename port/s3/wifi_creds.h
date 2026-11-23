/* WiFi 账号（STA）—— 板子要联网就必须填这里，或者用 make 变量传：
 *
 *     make C6_ROOT=/home/winger/c6 https S3_WIFI_SSID='"MyAP"' S3_WIFI_PASS='"secret"'
 *
 * 注意 make 变量里的引号要写成 '"..."'（双引号是 C 字符串的一部分）。
 * 留空则会跳过关联，串口打印 "[wifi] 没给 SSID，跳过关联"。
 */
/* 本地账号文件（不进版本库，见 .gitignore）：port/s3/wifi_creds_local.h
 * 里面写
 *     #define S3_WIFI_SSID "MyAP"
 *     #define S3_WIFI_PASS "secret"
 * 这样既不用在命令行上跟引号搏斗，也不会把口令提交进仓库。 */
#if defined(__has_include)
#if __has_include("wifi_creds_local.h")
#include "wifi_creds_local.h"
#endif
#endif

#ifndef S3_WIFI_SSID
#define S3_WIFI_SSID ""
#endif
#ifndef S3_WIFI_PASS
#define S3_WIFI_PASS ""
#endif
