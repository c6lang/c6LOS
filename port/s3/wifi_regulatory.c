/* wifi_regulatory.c —— 闭源 net80211 需要的两张监管域数据表。
 *
 * 出处：IDF `components/esp_wifi/regulatory/esp_wifi_regulatory.c` 的
 * **非 5G 分支**（esp32s3 不支持 5G，所以就是 `#else` 那一半，原文 1044..1405 行）。
 * 链接闭源 libnet80211.a 时 `regdomain_table` / `regulatory_data` 是 U，IDF 里由
 * 这个开源组件提供；我们不带整个 IDF，只把数据搬过来。
 *
 * 表结构必须与 IDF `esp_wifi_types_generic.h` 逐字节一致：
 *   wifi_reg_rule_t   = { u8 start_channel; u8 end_channel;
 *                         u16 max_bandwidth:3, max_eirp:6, is_dfs:1, reserved:6; }  (4B)
 *   wifi_regulatory_t = { u8 n_reg_rules; wifi_reg_rule_t reg_rules[2]; }            (10B)
 *   wifi_regdomain_t  = { char cn[2]; u8 regulatory_type; }                          (3B)
 * 下面的数据表格保持 IDF 原文（含注释）以便对照。重新生成命令见
 * docs/wifi-plan.md §202。
 */
#include "c6os/types.h"

#define WIFI_MAX_SUPPORT_COUNTRY_NUM 176
#define WIFI_MAX_REGULATORY_RULE_NUM  2

typedef struct {
    u8 start_channel;
    u8 end_channel;
    u16 max_bandwidth : 3;
    u16 max_eirp : 6;
    u16 is_dfs : 1;
    u16 reserved : 6;
} wifi_reg_rule_t;

typedef struct {
    u8 n_reg_rules;
    wifi_reg_rule_t reg_rules[WIFI_MAX_REGULATORY_RULE_NUM];
} wifi_regulatory_t;

typedef struct {
    char cn[2];
    u8 regulatory_type;
} wifi_regdomain_t;

typedef enum {
    ESP_WIFI_REGULATORY_TYPE_DEFAULT,
    ESP_WIFI_REGULATORY_TYPE_CE,
    ESP_WIFI_REGULATORY_TYPE_ACMA,
    ESP_WIFI_REGULATORY_TYPE_ANATEL,
    ESP_WIFI_REGULATORY_TYPE_ISED,
    ESP_WIFI_REGULATORY_TYPE_SRRC,
    ESP_WIFI_REGULATORY_TYPE_OFCA,
    ESP_WIFI_REGULATORY_TYPE_WPC,
    ESP_WIFI_REGULATORY_TYPE_MIC,
    ESP_WIFI_REGULATORY_TYPE_KCC,
    ESP_WIFI_REGULATORY_TYPE_IFETEL,
    ESP_WIFI_REGULATORY_TYPE_RCM,
    ESP_WIFI_REGULATORY_TYPE_NCC,
    ESP_WIFI_REGULATORY_TYPE_FCC,
    ESP_WIFI_REGULATORY_TYPE_CU,
    ESP_WIFI_REGULATORY_TYPE_GT,
    ESP_WIFI_REGULATORY_TYPE_ID,
    ESP_WIFI_REGULATORY_TYPE_KE,
    ESP_WIFI_REGULATORY_TYPE_KP,
    ESP_WIFI_REGULATORY_TYPE_OM,
    ESP_WIFI_REGULATORY_TYPE_PK,
    ESP_WIFI_REGULATORY_TYPE_TG,
    ESP_WIFI_REGULATORY_TYPE_MAX,
} esp_wifi_regulatory_type_t;

const wifi_regdomain_t regdomain_table[WIFI_MAX_SUPPORT_COUNTRY_NUM] = {
    {"01", ESP_WIFI_REGULATORY_TYPE_DEFAULT},
    {"EU", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AD", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AI", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AM", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AS", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"AT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AU", ESP_WIFI_REGULATORY_TYPE_ACMA},
    {"AW", ESP_WIFI_REGULATORY_TYPE_CE},
    {"AZ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BA", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BB", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BD", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BG", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BH", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BM", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"BN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BO", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BR", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"BS", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BY", ESP_WIFI_REGULATORY_TYPE_CE},
    {"BZ", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"CA", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"CF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CH", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CI", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CO", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CR", ESP_WIFI_REGULATORY_TYPE_OFCA},
    {"CU", ESP_WIFI_REGULATORY_TYPE_CU},
    {"CX", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CY", ESP_WIFI_REGULATORY_TYPE_CE},
    {"CZ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"DE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"DK", ESP_WIFI_REGULATORY_TYPE_CE},
    {"DM", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"DO", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"DZ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"EC", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"EE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"EG", ESP_WIFI_REGULATORY_TYPE_CE},
    {"ES", ESP_WIFI_REGULATORY_TYPE_CE},
    {"ET", ESP_WIFI_REGULATORY_TYPE_CE},
    {"FI", ESP_WIFI_REGULATORY_TYPE_CE},
    {"FM", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"FR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GB", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GD", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"GE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GH", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GP", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"GT", ESP_WIFI_REGULATORY_TYPE_GT},
    {"GU", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"GY", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"HK", ESP_WIFI_REGULATORY_TYPE_OFCA},
    {"HN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"HR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"HT", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"HU", ESP_WIFI_REGULATORY_TYPE_CE},
    {"ID", ESP_WIFI_REGULATORY_TYPE_ID},
    {"IE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"IL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"IN", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"IR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"IS", ESP_WIFI_REGULATORY_TYPE_CE},
    {"IT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"JM", ESP_WIFI_REGULATORY_TYPE_CE},
    {"JO", ESP_WIFI_REGULATORY_TYPE_CE},
    {"JP", ESP_WIFI_REGULATORY_TYPE_MIC},
    {"KE", ESP_WIFI_REGULATORY_TYPE_KE},
    {"KH", ESP_WIFI_REGULATORY_TYPE_CE},
    {"KN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"KP", ESP_WIFI_REGULATORY_TYPE_KP},
    {"KR", ESP_WIFI_REGULATORY_TYPE_KCC},
    {"KW", ESP_WIFI_REGULATORY_TYPE_CE},
    {"KY", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"KZ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LB", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LC", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LI", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LK", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LS", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LU", ESP_WIFI_REGULATORY_TYPE_CE},
    {"LV", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MA", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MC", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MD", ESP_WIFI_REGULATORY_TYPE_CE},
    {"ME", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MH", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"MK", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MO", ESP_WIFI_REGULATORY_TYPE_CU},
    {"MP", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"MQ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MU", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MV", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MW", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MX", ESP_WIFI_REGULATORY_TYPE_CE},
    {"MY", ESP_WIFI_REGULATORY_TYPE_GT},
    {"NA", ESP_WIFI_REGULATORY_TYPE_CE},
    {"NG", ESP_WIFI_REGULATORY_TYPE_CE},
    {"NI", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"NL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"NO", ESP_WIFI_REGULATORY_TYPE_CE},
    {"NP", ESP_WIFI_REGULATORY_TYPE_CE},
    {"NZ", ESP_WIFI_REGULATORY_TYPE_OFCA},
    {"OM", ESP_WIFI_REGULATORY_TYPE_OM},
    {"PA", ESP_WIFI_REGULATORY_TYPE_OFCA},
    {"PE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PG", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PH", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PK", ESP_WIFI_REGULATORY_TYPE_PK},
    {"PL", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PM", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PR", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"PT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"PW", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"PY", ESP_WIFI_REGULATORY_TYPE_CE},
    {"QA", ESP_WIFI_REGULATORY_TYPE_OM},
    {"RE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"RO", ESP_WIFI_REGULATORY_TYPE_CE},
    {"RS", ESP_WIFI_REGULATORY_TYPE_CE},
    {"RU", ESP_WIFI_REGULATORY_TYPE_CE},
    {"RW", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SA", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SG", ESP_WIFI_REGULATORY_TYPE_CU},
    {"SI", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SK", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SV", ESP_WIFI_REGULATORY_TYPE_CE},
    {"SY", ESP_WIFI_REGULATORY_TYPE_OM},
    {"TC", ESP_WIFI_REGULATORY_TYPE_CE},
    {"TD", ESP_WIFI_REGULATORY_TYPE_CE},
    {"TG", ESP_WIFI_REGULATORY_TYPE_TG},
    {"TH", ESP_WIFI_REGULATORY_TYPE_CE},
    {"TN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"TR", ESP_WIFI_REGULATORY_TYPE_CE},
    {"TT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"TW", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"TZ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"UA", ESP_WIFI_REGULATORY_TYPE_OM},
    {"UG", ESP_WIFI_REGULATORY_TYPE_CE},
    {"US", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"UY", ESP_WIFI_REGULATORY_TYPE_CE},
    {"UZ", ESP_WIFI_REGULATORY_TYPE_CE},
    {"VC", ESP_WIFI_REGULATORY_TYPE_CE},
    {"VE", ESP_WIFI_REGULATORY_TYPE_ANATEL},
    {"VI", ESP_WIFI_REGULATORY_TYPE_ISED},
    {"VN", ESP_WIFI_REGULATORY_TYPE_CE},
    {"VU", ESP_WIFI_REGULATORY_TYPE_CE},
    {"WF", ESP_WIFI_REGULATORY_TYPE_CE},
    {"WS", ESP_WIFI_REGULATORY_TYPE_CE},
    {"YE", ESP_WIFI_REGULATORY_TYPE_CE},
    {"YT", ESP_WIFI_REGULATORY_TYPE_CE},
    {"ZA", ESP_WIFI_REGULATORY_TYPE_CE},
    {"ZW", ESP_WIFI_REGULATORY_TYPE_CE},
};

const wifi_regulatory_t regulatory_data[] = {
    /* ESP_WIFI_REGULATORY_TYPE_DEFAULT */
    {
        1,  /* Number of rules */
        {
            { 1, 11, 2, 20, 0, 0 },  /* chan:1~11, max_bw:40M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_CE */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 20, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ACMA */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 36, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:36(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ANATEL */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 30, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:30(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ISED */
    {
        1,  /* Number of rules */
        {
            { 1, 11, 2, 30, 0, 0 },  /* chan:1~11, max_bw:40M, max_power:30(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_CE */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 20, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_OFCA */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 36, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:36(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ANATEL */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 30, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:30(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_MIC */
    {
        2,  /* Number of rules */
        {
            { 1, 13, 2, 20, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:20(dBm), dfs:0, reserved.*/
            { 14, 14, 1, 20, 0, 0 },  /* chan:14~14, max_bw:20M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_KCC */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 23, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:23(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_CE */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 20, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_OFCA */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 36, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:36(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ANATEL */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 30, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:30(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ISED */
    {
        1,  /* Number of rules */
        {
            { 1, 11, 2, 30, 0, 0 },  /* chan:1~11, max_bw:40M, max_power:30(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_CU */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 23, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:23(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_GT */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 26, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:26(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_ID */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 26, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:26(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_KE */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 33, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:33(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_KP */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 1, 20, 0, 0 },  /* chan:1~13, max_bw:20M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_OM */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 20, 0, 0 },  /* chan:1~13, max_bw:40M, max_power:20(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_PK */
    {
        1,  /* Number of rules */
        {
            { 1, 14, 2, 30, 0, 0 },  /* chan:1~14, max_bw:40M, max_power:30(dBm), dfs:0, reserved.*/
        }
    },
    /* ESP_WIFI_REGULATORY_TYPE_TG */
    {
        1,  /* Number of rules */
        {
            { 1, 13, 2, 20, 1, 0 },  /* chan:1~13, max_bw:40M, max_power:20(dBm), dfs:1, reserved.*/
        }
    },
};
