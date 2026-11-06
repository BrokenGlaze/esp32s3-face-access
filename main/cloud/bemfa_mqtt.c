/**
 * @file    bemfa_mqtt.c
 * @brief   巴法云 MQTT 实现（基于 ESP-IDF mqtt_client）
 *
 * @note    - 连接地址 mqtt://bemfa.com:9501，client_id = 用户私钥
 *          - 设备状态发布到 door004/up（只更新云端数据，避免自己推自己）
 *          - 采用较小的任务栈与缓冲，适配本项目紧张的内部 RAM
 */

#include "bemfa_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "bemfa_config.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "time_sync.h"
#include "wifi_manager.h"

#define TAG "bemfa_mqtt"
#define BEMFA_STATUS_TOPIC BEMFA_MQTT_TOPIC "/up"

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static bool s_started = false;

/**
 * @brief MQTT 事件处理：连上后发布在线状态
 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)arg;
    (void)base;

    switch ((esp_mqtt_event_id_t)id)
    {
    case MQTT_EVENT_CONNECTED:
    {
        char msg[64];

        s_connected = true;
        strcpy(msg, "online");
        if (time_sync_is_valid())
        {
            char t[32];

            time_sync_format(t, sizeof(t), "%m-%d %H:%M");
            snprintf(msg, sizeof(msg), "online %s", t);
        }
        esp_mqtt_client_publish(s_client, BEMFA_STATUS_TOPIC, msg, 0, 0, 0);
        ESP_LOGI(TAG, "MQTT 已连接，已发布在线状态");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT 已断开，等待自动重连");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT 错误（event_id=%d）", (int)event->event_id);
        break;

    default:
        break;
    }
}

/**
 * @brief 创建并启动 MQTT 客户端（只启动一次）
 */
static void start_client(void)
{
    if (s_started)
    {
        return;
    }

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = BEMFA_MQTT_URI,
        .credentials.client_id = BEMFA_UID,
        .session.last_will = {
            .msg = "offline",
            .msg_len = 0,
            .topic = BEMFA_STATUS_TOPIC,
            .qos = 0,
            .retain = 0,
        },
        .network.disable_auto_reconnect = false,
        .task.stack_size = 4096,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "MQTT 客户端创建失败（内存不足）");
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK)
    {
        ESP_LOGE(TAG, "MQTT 启动失败（internal free=%u largest=%u）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return;
    }

    s_started = true;
}

/**
 * @brief 获取到 IP 后启动 MQTT
 */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    start_client();
}

void bemfa_mqtt_start(void)
{
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_got_ip, NULL, NULL);

    if (wifi_manager_get_state() == WIFI_MGR_CONNECTED)
    {
        start_client();
    }
}

void bemfa_mqtt_publish_status(const char *text)
{
    if (s_client == NULL || text == NULL || !s_connected)
    {
        return;
    }
    esp_mqtt_client_publish(s_client, BEMFA_STATUS_TOPIC, text, 0, 0, 0);
}

bool bemfa_mqtt_is_connected(void)
{
    return s_connected;
}

void bemfa_mqtt_ensure_started(void)
{
    /* 首次启动常因内部 RAM 不足失败，空闲时再试一次 */
    if (!s_started && (wifi_manager_get_state() == WIFI_MGR_CONNECTED))
    {
        start_client();
    }
}
