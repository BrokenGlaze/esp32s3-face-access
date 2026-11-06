/**
 * @file    time_sync.c
 * @brief   SNTP 校时实现：Wi-Fi 获取 IP 后自动同步，时区 UTC+8
 */

#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "wifi_manager.h"

#define TAG "time_sync"
#define TIME_SYNC_TZ "CST-8" /* 中国标准时间 UTC+8 */

static volatile bool s_valid = false;
static bool s_sntp_started = false;

/**
 * @brief SNTP 校时完成回调
 */
static void sntp_sync_cb(struct timeval *tv)
{
    char buf[32];

    (void)tv;
    s_valid = true;
    if (time_sync_format(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S"))
    {
        ESP_LOGI(TAG, "校时完成：%s", buf);
    }
}

/**
 * @brief 启动 SNTP（只启动一次）
 */
static void start_sntp(void)
{
    if (s_sntp_started)
    {
        return;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP 已启动，等待校时");
}

/**
 * @brief 获取到 IP 后启动 SNTP
 */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    start_sntp();
}

void time_sync_init(void)
{
    setenv("TZ", TIME_SYNC_TZ, 1);
    tzset();

    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_got_ip, NULL, NULL);

    /* 可能注册时已经连上 Wi-Fi，直接启动一次 */
    if (wifi_manager_get_state() == WIFI_MGR_CONNECTED)
    {
        start_sntp();
    }
}

bool time_sync_is_valid(void)
{
    return s_valid;
}

int64_t time_sync_epoch(void)
{
    if (!s_valid)
    {
        return 0;
    }
    return (int64_t)time(NULL);
}

bool time_sync_format(char *buf, size_t len, const char *fmt)
{
    time_t now;
    struct tm tm_now;

    if (!s_valid || buf == NULL || len == 0 || fmt == NULL)
    {
        return false;
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);
    return strftime(buf, len, fmt, &tm_now) > 0;
}
