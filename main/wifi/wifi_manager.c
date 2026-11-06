/**
 * @file    wifi_manager.c
 * @brief   Wi-Fi STA 管理实现（扫描 / 连接 / 断线重试 / NVS 凭据）
 *
 * @note    参考 ESP-IDF examples/wifi/scan 与 getting_started/station：
 *          - 扫描：esp_wifi_scan_start(NULL, true) + esp_wifi_scan_get_ap_records()
 *          - 连接：事件组/状态变量 + WIFI_EVENT_STA_DISCONNECTED 里重连
 *          - 与案例的差别：初始化不阻塞等待连接，避免开机和界面被网络拖住
 */

#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "wifi_mgr"

#define WIFI_NVS_NS        "wifi_cfg"   /* NVS 命名空间 */
#define WIFI_NVS_SSID      "ssid"       /* NVS 键：热点名称 */
#define WIFI_NVS_PWD       "pwd"        /* NVS 键：密码 */
#define WIFI_MAX_RETRY     5            /* 断线后最多重连次数 */

static bool s_started = false;
static volatile wifi_mgr_state_t s_state = WIFI_MGR_IDLE;
static volatile int s_retry = 0;
static volatile bool s_manual_disconnect = false; /* 用户主动断开时不自动重连 */
static char s_ssid[WIFI_MANAGER_SSID_LEN + 1] = "";
static char s_pwd[WIFI_MANAGER_PWD_LEN + 1] = "";
static char s_ip[16] = "";

/**
 * @brief 从 NVS 读取保存的凭据
 * @return true 表示读到有效的 SSID
 */
static bool wifi_load_credentials(void)
{
    nvs_handle_t handle;
    char ssid[WIFI_MANAGER_SSID_LEN + 1] = "";
    char pwd[WIFI_MANAGER_PWD_LEN + 1] = "";
    size_t len;

    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }

    len = sizeof(ssid);
    if (nvs_get_str(handle, WIFI_NVS_SSID, ssid, &len) != ESP_OK)
    {
        nvs_close(handle);
        return false;
    }

    len = sizeof(pwd);
    if (nvs_get_str(handle, WIFI_NVS_PWD, pwd, &len) != ESP_OK)
    {
        pwd[0] = '\0'; /* 没有密码字段按开放网络处理 */
    }
    nvs_close(handle);

    ssid[WIFI_MANAGER_SSID_LEN] = '\0';
    pwd[WIFI_MANAGER_PWD_LEN] = '\0';
    if (ssid[0] == '\0')
    {
        return false;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    strncpy(s_pwd, pwd, sizeof(s_pwd) - 1);
    return true;
}

/**
 * @brief 把凭据写入 NVS（掉电保存）
 * @return true 表示写入成功
 */
static bool wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &handle) != ESP_OK)
    {
        ESP_LOGW(TAG, "打开 NVS 失败，凭据未保存");
        return false;
    }

    err = nvs_set_str(handle, WIFI_NVS_SSID, ssid);
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, WIFI_NVS_PWD, (password != NULL) ? password : "");
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "保存 Wi-Fi 凭据失败，ret=%d", (int)err);
        return false;
    }
    return true;
}

/**
 * @brief 用当前 s_ssid / s_pwd 下发 STA 配置并发起连接
 */
static void wifi_start_connect(void)
{
    wifi_config_t cfg = {0};

    if (s_ssid[0] == '\0')
    {
        return;
    }

    memcpy(cfg.sta.ssid, s_ssid, strnlen(s_ssid, WIFI_MANAGER_SSID_LEN));
    memcpy(cfg.sta.password, s_pwd, strnlen(s_pwd, WIFI_MANAGER_PWD_LEN));
    /* authmode 保持 0（WIFI_AUTH_OPEN）：允许开放/WPA/WPA2/WPA3 各种网络 */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.required = false;

    s_retry = 0;
    s_manual_disconnect = false;
    s_state = WIFI_MGR_CONNECTING;
    s_ip[0] = '\0';

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "设置 STA 配置失败，ret=%d", (int)err);
        s_state = WIFI_MGR_FAILED;
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "发起连接失败，ret=%d", (int)err);
        s_state = WIFI_MGR_FAILED;
    }
}

/**
 * @brief Wi-Fi / IP 事件处理
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        if (s_ssid[0] != '\0')
        {
            wifi_start_connect();
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_ip[0] = '\0';
        if (s_manual_disconnect)
        {
            s_state = WIFI_MGR_IDLE;
            return;
        }
        if (s_ssid[0] != '\0' && s_retry < WIFI_MAX_RETRY)
        {
            s_retry++;
            s_state = WIFI_MGR_CONNECTING;
            ESP_LOGI(TAG, "连接断开，第 %d/%d 次重试（SSID:%s）",
                     s_retry, WIFI_MAX_RETRY, s_ssid);
            esp_wifi_connect();
        }
        else
        {
            s_state = WIFI_MGR_FAILED;
            ESP_LOGW(TAG, "连接失败（SSID:%s），重试次数已用尽", s_ssid);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;

        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip, sizeof(s_ip));
        s_retry = 0;
        s_state = WIFI_MGR_CONNECTED;
        ESP_LOGI(TAG, "连接成功 SSID:%s IP:%s", s_ssid, s_ip);
        ESP_LOGI(TAG, "[MEM] 连网后 internal free=%u largest=%u psram free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t err;
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_started)
    {
        return ESP_OK;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init 失败，ret=%d", (int)err);
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "事件循环创建失败，ret=%d", (int)err);
        return err;
    }

    esp_netif_create_default_wifi_sta();

    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init 失败，ret=%d", (int)err);
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WIFI_EVENT 注册失败，ret=%d", (int)err);
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "IP_EVENT 注册失败，ret=%d", (int)err);
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 STA 模式失败，ret=%d", (int)err);
        return err;
    }

    /* 只用 RAM 保存 Wi-Fi 驱动配置，凭据由本模块写 NVS，避免重复占用 NVS */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_load_credentials();

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start 失败，ret=%d", (int)err);
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi 初始化完成（%s）",
             (s_ssid[0] != '\0') ? "使用已保存的凭据" : "尚未配置热点");
    ESP_LOGI(TAG, "[MEM] internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

bool wifi_manager_is_started(void)
{
    return s_started;
}

int wifi_manager_scan(wifi_mgr_ap_t *out, int max)
{
    uint16_t ap_num = 0;
    uint16_t want;
    wifi_ap_record_t *records;
    int count;
    esp_err_t err;

    if (!s_started || out == NULL || max <= 0)
    {
        return -1;
    }

    esp_wifi_scan_stop();
    err = esp_wifi_scan_start(NULL, true); /* true：阻塞等待扫描完成 */
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "启动扫描失败，ret=%d", (int)err);
        return -1;
    }

    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0)
    {
        esp_wifi_clear_ap_list();
        return 0;
    }

    want = (ap_num > (uint16_t)max) ? (uint16_t)max : ap_num;
    records = (wifi_ap_record_t *)calloc(want, sizeof(wifi_ap_record_t));
    if (records == NULL)
    {
        esp_wifi_clear_ap_list();
        return -1;
    }

    count = (int)want;
    err = esp_wifi_scan_get_ap_records(&want, records);
    esp_wifi_clear_ap_list();

    if (err != ESP_OK)
    {
        free(records);
        return -1;
    }

    for (int i = 0; i < (int)want && i < max; i++)
    {
        strncpy(out[i].ssid, (const char *)records[i].ssid, WIFI_MANAGER_SSID_LEN);
        out[i].ssid[WIFI_MANAGER_SSID_LEN] = '\0';
        out[i].rssi = records[i].rssi;
        out[i].authmode = (uint8_t)records[i].authmode;
    }
    count = (int)want;

    free(records);
    ESP_LOGI(TAG, "扫描完成，共 %d 个热点（总数 %u）", count, (unsigned)ap_num);
    return count;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0' ||
        strlen(ssid) > WIFI_MANAGER_SSID_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > WIFI_MANAGER_PWD_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pwd, (password != NULL) ? password : "", sizeof(s_pwd) - 1);
    s_pwd[sizeof(s_pwd) - 1] = '\0';

    wifi_save_credentials(s_ssid, s_pwd);

    esp_wifi_disconnect();
    wifi_start_connect();

    ESP_LOGI(TAG, "开始连接 SSID:%s", s_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_state = WIFI_MGR_IDLE;
    s_manual_disconnect = true;
    s_ip[0] = '\0';
    return esp_wifi_disconnect();
}

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return s_state;
}

const char *wifi_manager_get_ssid(void)
{
    return s_ssid;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip;
}

bool wifi_manager_has_credentials(void)
{
    return s_ssid[0] != '\0';
}

esp_err_t wifi_manager_reconnect(void)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_wifi_disconnect();
    wifi_start_connect();
    return ESP_OK;
}

/**
 * @var s_ap_netif
 * @brief 配网热点使用的 netif（提供 192.168.4.1 与 DHCP 服务）
 */
static esp_netif_t *s_ap_netif = NULL;

esp_err_t wifi_manager_ap_start(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    esp_err_t err;

    if (!s_started || ssid == NULL || ssid[0] == '\0' ||
        strlen(ssid) > WIFI_MANAGER_SSID_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ap_netif == NULL)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL)
        {
            ESP_LOGE(TAG, "创建 AP netif 失败");
            return ESP_FAIL;
        }
    }

    memcpy(cfg.ap.ssid, ssid, strlen(ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.beacon_interval = 100;

    if (password != NULL && password[0] != '\0')
    {
        size_t len = strlen(password);
        if (len < 8 || len > WIFI_MANAGER_PWD_LEN)
        {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(cfg.ap.password, password, len);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "切换 APSTA 模式失败，ret=%d", (int)err);
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 AP 配置失败，ret=%d", (int)err);
        return err;
    }

    ESP_LOGI(TAG, "配网热点已开启：%s（192.168.4.1）", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_ap_stop(void)
{
    esp_err_t err;

    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "恢复 STA 模式失败，ret=%d", (int)err);
    }
    ESP_LOGI(TAG, "配网热点已关闭");
    return err;
}
