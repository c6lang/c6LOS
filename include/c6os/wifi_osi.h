/* wifi_osi.h -- WiFi closed-source lib OS adapter table (ESP32-S3).
 * 字段顺序**直接从** IDF 的
 *   components/esp_wifi/include/esp_private/wifi_os_adapter.h
 * 生成，并已去掉 S3 上不成立的两个条件块：
 *   - `#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_ESP_WIFI_TARGET_ESP32`
 *     包住的 _phy_common_clock_enable/_disable
 *   - `#if CONFIG_IDF_TARGET_ESP32C6 || ...C5 || ...C61`
 *     包住的 _regdma_link_set_write_wait_content/_sleep_retention_find_link_by_id
 * ⇒ S3 上共 **120** 个字段，_version 在 0，_magic 在 119（偏移 0x1DC）。
 * 这个偏移已被反汇编 libnet80211 的 wifi_osi_funcs_register 证实：
 *   4203d43c:  l32i a12, a2, 0x1dc
 *   4203d43f:  l32r a11, 0xdeadbeaf
 *   ⇒ 第 119 个字必须是魔数。
 *
 * blob (libpp/libnet80211) 通过全局指针 g_osi_funcs_p 读这张表。
 * S3 的 ROM ld 不定义 g_osi_funcs_p（C2/C6/C61 才有），所以由我们自己定义。 */
#ifndef C6OS_WIFI_OSI_H
#define C6OS_WIFI_OSI_H

#include "c6os/types.h"

typedef struct wifi_osi_funcs_t {
    void *_version;
    void *_env_is_chip;
    void *_set_intr;
    void *_clear_intr;
    void *_set_isr;
    void *_ints_on;
    void *_ints_off;
    void *_is_from_isr;
    void *_spin_lock_create;
    void *_spin_lock_delete;
    void *_wifi_int_disable;
    void *_wifi_int_restore;
    void *_task_yield_from_isr;
    void *_semphr_create;
    void *_semphr_delete;
    void *_semphr_take;
    void *_semphr_give;
    void *_wifi_thread_semphr_get;
    void *_mutex_create;
    void *_recursive_mutex_create;
    void *_mutex_delete;
    void *_mutex_lock;
    void *_mutex_unlock;
    void *_queue_create;
    void *_queue_delete;
    void *_queue_send;
    void *_queue_send_from_isr;
    void *_queue_send_to_back;
    void *_queue_send_to_front;
    void *_queue_recv;
    void *_queue_msg_waiting;
    void *_event_group_create;
    void *_event_group_delete;
    void *_event_group_set_bits;
    void *_event_group_clear_bits;
    void *_event_group_wait_bits;
    void *_task_create_pinned_to_core;
    void *_task_create;
    void *_task_delete;
    void *_task_delay;
    void *_task_ms_to_tick;
    void *_task_get_current_task;
    void *_task_get_max_priority;
    void *_malloc;
    void *_free;
    void *_event_post;
    void *_get_free_heap_size;
    void *_rand;
    void *_dport_access_stall_other_cpu_start_wrap;
    void *_dport_access_stall_other_cpu_end_wrap;
    void *_wifi_apb80m_request;
    void *_wifi_apb80m_release;
    void *_phy_disable;
    void *_phy_enable;
    void *_phy_update_country_info;
    void *_read_mac;
    void *_timer_arm;
    void *_timer_disarm;
    void *_timer_done;
    void *_timer_setfn;
    void *_timer_arm_us;
    void *_wifi_reset_mac;
    void *_wifi_clock_enable;
    void *_wifi_clock_disable;
    void *_wifi_rtc_enable_iso;
    void *_wifi_rtc_disable_iso;
    void *_esp_timer_get_time;
    void *_nvs_set_i8;
    void *_nvs_get_i8;
    void *_nvs_set_u8;
    void *_nvs_get_u8;
    void *_nvs_set_u16;
    void *_nvs_get_u16;
    void *_nvs_open;
    void *_nvs_close;
    void *_nvs_commit;
    void *_nvs_set_blob;
    void *_nvs_get_blob;
    void *_nvs_erase_key;
    void *_get_random;
    void *_get_time;
    void *_random;
    void *_slowclk_cal_get;
    void *_log_write;
    void *_log_writev;
    void *_log_timestamp;
    void *_malloc_internal;
    void *_realloc_internal;
    void *_calloc_internal;
    void *_zalloc_internal;
    void *_wifi_malloc;
    void *_wifi_realloc;
    void *_wifi_calloc;
    void *_wifi_zalloc;
    void *_wifi_create_queue;
    void *_wifi_delete_queue;
    void *_coex_init;
    void *_coex_deinit;
    void *_coex_enable;
    void *_coex_disable;
    void *_coex_status_get;
    void *_coex_condition_set;
    void *_coex_wifi_request;
    void *_coex_wifi_release;
    void *_coex_wifi_channel_set;
    void *_coex_event_duration_get;
    void *_coex_pti_get;
    void *_coex_schm_status_bit_clear;
    void *_coex_schm_status_bit_set;
    void *_coex_schm_interval_set;
    void *_coex_schm_interval_get;
    void *_coex_schm_curr_period_get;
    void *_coex_schm_curr_phase_get;
    void *_coex_schm_process_restart;
    void *_coex_schm_register_cb;
    void *_coex_register_start_cb;
    void *_coex_schm_flexible_period_set;
    void *_coex_schm_flexible_period_get;
    void *_coex_schm_get_phase_by_idx;
    void *_magic;
} wifi_osi_funcs_t;

extern wifi_osi_funcs_t g_wifi_osi_funcs;
extern void *g_osi_funcs_p;
void s3_wifi_osi_init(void);

#endif
