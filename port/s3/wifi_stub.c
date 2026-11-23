/* 未链接闭源 WiFi 库（S3_WITH_WIFI 未定义）时的链接兜底。
 *
 * w2c.S / win_island.c / wifi_glue.c 这些 ABI 桥在普通构建里也参与链接，
 * 但它们引用的 esp_wifi_start、__real_*（--wrap 目标）和 g_log_level 只存在于
 * IDF 的闭源 .a 里。以前 `make s3` 会因此 undefined reference，需要外部传
 * /tmp/wstub.o 才能出镜像（见 docs/wifi-plan.md「构建侧的真实状态」）。
 *
 * 这里把那份临时桩固化进仓库，并在 S3_WITH_WIFI 构建里整段排除 —— 真 WiFi
 * 镜像必须用闭源库的强符号，不能被桩满足（弱符号会阻止归档成员被拉取）。
 * 桩函数按 windowed ABI 编译，与调用方（wifi_glue.c / win_island.c）一致。
 */
#if !defined(S3_WITH_WIFI)

int g_log_level = 0;

/* 库的状态机诊断用的全局数组（真正链接闭源库时由 libpp 提供）。
 * 普通构建里给一块零-filled 内存，win_island/wifi_glue 的诊断读不会缺符号。 */
unsigned char g_wifi_osi_funcs_md5[64] = {0};

/* 闭源库的公开入口：不链接库时明确返回失败，而不是让链接器报缺失。 */
int esp_wifi_start(void)
{
    return -1;
}

/* win_island.c 的 PHY 回调只在 S3_WITH_LIBPHY 构建里定义（它调用
 * register_chipv7_phy）；wifi_glue.c 的 OSI _phy_enable 槽位无条件引用它。 */
int s3_win_phy_enable_hook(void)
{
    return -1;
}

/* wifi_glue.c 的 __wrap_* 计数壳引用的 --wrap 目标。普通构建不传 --wrap，
 * 这些壳不会被调用，桩只为把符号补全。 */
int __real_wifi_menuconfig_init(void *cfg)
{
    (void)cfg;
    return -1;
}

int __real_misc_nvs_init(void)
{
    return -1;
}

int __real_ic_create_wifi_task(void)
{
    return -1;
}

int __real_ieee80211_ioctl_init(void)
{
    return -1;
}

int __real_pp_create_task(void)
{
    return -1;
}

int __real_pp_post(unsigned int ppq, unsigned int data)
{
    (void)ppq;
    (void)data;
    return -1;
}

int __real_wifi_init_in_caller_task(void)
{
    return -1;
}

/* main 的 WiFi 追踪壳新增的 --wrap 目标（普通构建不传 --wrap，仅补符号）。 */
int __real_wifi_scan_start_process(void *msg)
{
    (void)msg;
    return -1;
}

int __real_ieee80211_ioctl_process(void *msg)
{
    (void)msg;
    return -1;
}

int __real_ieee80211_sta_scan(void *arg)
{
    (void)arg;
    return -1;
}

int __real_pm_offchan_ready_or_defer(void *a, void *b)
{
    (void)a;
    (void)b;
    return -1;
}

int __real_chm_acquire_lock(void *a)
{
    (void)a;
    return -1;
}

void __real_ieee80211_send_probereq(unsigned int a1, unsigned int a2,
                                   unsigned int a3, unsigned int a4,
                                   unsigned int a5, unsigned int a6,
                                   unsigned int a7, unsigned int a8)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    (void)a5; (void)a6; (void)a7; (void)a8;
}

unsigned int __real_ic_tx_pkt(unsigned int a)
{
    (void)a;
    return 0u;
}

int __real_ieee80211_regdomain_is_active_scan(unsigned int ch)
{
    (void)ch;
    return 0;
}

#endif /* !S3_WITH_WIFI */