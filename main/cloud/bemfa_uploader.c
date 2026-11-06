/**
 * @file    bemfa_uploader.c
 * @brief   巴法云照片上传实现（队列 + 低优先级任务 + JPEG 编码 + HTTPS）
 *
 * @note    - 图片接口：https://apis.bemfa.com/vb/api/v1/imagesUploadBin
 *            Authorization = 私钥，Authtopic = 图片主题，body = JPEG 原始字节
 *          - 照片先由识别模块存成 180×120 RGB565，这里再做 JPEG 编码，
 *            编码输出与 TLS 缓冲都使用 PSRAM，避免挤占内部 RAM
 *          - HTTPS 证书链依赖系统时间，因此必须等 SNTP 校时完成后才上传
 */

#include "bemfa_uploader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bemfa_config.h"
#include "bemfa_mqtt.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "time_sync.h"
#include "wifi_manager.h"
#include "who_human_face_recognition.hpp"

#define TAG "bemfa_upload"

#define UPLOAD_URL          "https://apis.bemfa.com/vb/api/v1/imagesUploadBin"
#define UPLOAD_QUEUE_LEN    8       /* 最多同时排队的照片数 */
#define UPLOAD_TASK_STACK   5120    /* 字节（TLS 握手需要一定栈空间） */
#define UPLOAD_TASK_PRIO    2       /* 低于摄像头/LCD/AI 任务，不抢占识别 */
#define UPLOAD_JPEG_QUALITY 75
#define UPLOAD_RETRY_MAX    3       /* 单张照片最多尝试次数 */
#define UPLOAD_RETRY_DELAY  2000    /* 重试间隔（ms） */

#define WECHAT_TIMEOUT_MS   8000    /* 微信通知请求超时 */
#define WECHAT_REALTIME_SEC 300     /* 只有 5 分钟内的识别才推送微信（补传旧记录不推） */
#define WECHAT_URL_MAX      160     /* 巴法云返回的图片地址缓冲 */

/**
 * @struct upload_item_t
 * @brief 队列中的一条上传任务
 */
typedef struct
{
    uint32_t seq;       /* 历史记录序号 */
    uint32_t epoch;     /* 识别时间（UNIX 秒，0=未校时） */
    int sim;            /* 相似度百分比 */
    char name[32];      /* 识别到的姓名 */
} upload_item_t;

/**
 * @struct http_resp_t
 * @brief 极简响应收集器（只保留前 191 字节，用于解析 {"code":0,...}）
 */
typedef struct
{
    char buf[192];
    size_t len;
} http_resp_t;

static QueueHandle_t s_queue = NULL;    /* 上传队列 */
static uint16_t *s_pixels = NULL;       /* 从历史记录取照片的 PSRAM 缓冲 */
static bool s_started = false;

/**
 * @brief 等待联网与校时完成（阻塞，期间照片留在队列里）
 *
 * HTTPS 需要正确系统时间校验证书有效期，SNTP 未完成时不能上传。
 */
static void wait_online(void)
{
    bool logged = false;

    while ((wifi_manager_get_state() != WIFI_MGR_CONNECTED) || !time_sync_is_valid())
    {
        if (!logged)
        {
            ESP_LOGI(TAG, "等待联网/校时，照片排队中");
            logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/**
 * @brief HTTP 事件回调：收集响应体，便于判断业务返回码
 */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;

    if ((evt->event_id == HTTP_EVENT_ON_DATA) && (resp != NULL) && (evt->data_len > 0))
    {
        size_t space = sizeof(resp->buf) - 1 - resp->len;
        size_t n = ((size_t)evt->data_len < space) ? (size_t)evt->data_len : space;

        memcpy(resp->buf + resp->len, evt->data, n);
        resp->len += n;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/**
 * @brief 解析巴法云响应中的 "code" 字段
 * @return true 表示 code == 0（成功）
 */
static bool response_ok(const http_resp_t *resp)
{
    const char *p = strstr(resp->buf, "\"code\"");

    if (p == NULL)
    {
        return true; /* 没有 JSON 字段时以 HTTP 状态码为准 */
    }
    p = strchr(p, ':');
    if (p == NULL)
    {
        return false;
    }
    p++;
    while ((*p == ' ') || (*p == '\t'))
    {
        p++;
    }
    return *p == '0';
}

/**
 * @brief 从巴法云响应里提取 "url" 字段（图片地址）
 *
 * 成功响应形如 {"url":"https://img2.bemfa.com/xxx.jpg"}，纯字符串查找即可。
 */
static void response_get_url(const http_resp_t *resp, char *out, size_t out_len)
{
    const char *p;
    const char *end;
    size_t n;

    out[0] = '\0';
    p = strstr(resp->buf, "\"url\"");
    if (p == NULL)
    {
        return;
    }
    p = strchr(p, ':');
    if (p == NULL)
    {
        return;
    }
    p++;
    while ((*p == ' ') || (*p == '\t'))
    {
        p++;
    }
    if (*p != '"')
    {
        return;
    }
    p++;
    end = strchr(p, '"');
    if (end == NULL)
    {
        return;
    }
    n = (size_t)(end - p);
    if (n >= out_len)
    {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

 /**
 * @brief 把 JPEG 上传到巴法云图片主题
 *
 * @param jpg JPEG 数据
 * @param jpg_len JPEG 长度
 * @param url_out 输出云端图片地址（可为 NULL）
 * @param url_len 地址缓冲长度
 * @return true 上传成功
 */
static bool http_upload(const uint8_t *jpg, size_t jpg_len, char *url_out, size_t url_len)
{
    http_resp_t resp = {0};
    esp_http_client_config_t cfg = {
        .url = UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .event_handler = http_event_cb,
        .user_data = &resp,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int status;
    bool ok;

    client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        ESP_LOGW(TAG, "HTTP 客户端创建失败（内存不足）");
        return false;
    }

    esp_http_client_set_header(client, "Authorization", BEMFA_UID);
    esp_http_client_set_header(client, "Authtopic", BEMFA_IMAGE_TOPIC);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_post_field(client, (const char *)jpg, (int)jpg_len);

    ESP_LOGI(TAG, "[MEM] 上传前 internal free=%u largest=%u dma largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    ok = (err == ESP_OK) && (status == 200) && response_ok(&resp);

    if (ok && (url_out != NULL) && (url_len > 0))
    {
        response_get_url(&resp, url_out, url_len);
    }

    ESP_LOGI(TAG, "HTTPS 上传：err=%d status=%d 响应=%s", (int)err, status, resp.buf);
    esp_http_client_cleanup(client);
    return ok;
}

/**
 * @brief 通过巴法云微信接口推送识别通知（走 HTTP 80，不占用 TLS 内存）
 *
 * @param msg 通知正文（UTF-8）
 * @param img_url 云端图片地址（可为空字符串）
 * @return true 推送成功
 */
static bool wechat_notify(const char *msg, const char *img_url)
{
    http_resp_t resp = {0};
    char body[512];
    esp_http_client_config_t cfg = {
        .url = BEMFA_WECHAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = WECHAT_TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 512,
        .event_handler = http_event_cb,
        .user_data = &resp,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int status;
    bool ok;

    snprintf(body, sizeof(body),
             "{\"uid\":\"%s\",\"device\":\"%s\",\"message\":\"%s\",\"group\":\"%s\",\"url\":\"%s\"}",
             BEMFA_UID, BEMFA_WECHAT_DEVICE, msg,
             BEMFA_WECHAT_GROUP, (img_url != NULL) ? img_url : "");

    client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        ESP_LOGW(TAG, "[WECHAT] 客户端创建失败（内存不足）");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    ok = (err == ESP_OK) && (status == 200) && response_ok(&resp);

    ESP_LOGI(TAG, "[WECHAT] %s：err=%d status=%d 响应=%s",
             ok ? "推送成功" : "推送失败", (int)err, status, resp.buf);
    esp_http_client_cleanup(client);
    return ok;
}

/**
 * @brief 把历史照片（RGB565 大端）编码成 JPEG
 *
 * @param out 输出 JPEG 缓冲（调用者用 free() 释放；可能位于 PSRAM）
 * @return JPEG 长度；0 表示失败
 */
static size_t encode_jpeg(uint8_t **out)
{
    size_t len = 0;

    *out = NULL;
    if (!fmt2jpg((uint8_t *)s_pixels, FACE_HISTORY_PIXELS * sizeof(uint16_t),
                 FACE_HISTORY_W, FACE_HISTORY_H, PIXFORMAT_RGB565,
                 UPLOAD_JPEG_QUALITY, out, &len))
    {
        *out = NULL;
        return 0;
    }
    return len;
}

/**
 * @brief 上传一条排队记录（含重试）
 * @return true 上传成功
 */
static bool process_item(const upload_item_t *item)
{
    uint8_t *jpg = NULL;
    size_t jpg_len;
    char img_url[WECHAT_URL_MAX] = "";
    bool ok = false;

    if (human_face_history_read_seq(item->seq, s_pixels, FACE_HISTORY_W,
                                    FACE_HISTORY_H) != 1)
    {
        ESP_LOGW(TAG, "记录 %u 已被新照片覆盖，跳过上传", (unsigned)item->seq);
        return true; /* 视作完成，避免阻塞后续记录 */
    }

    jpg_len = encode_jpeg(&jpg);
    if ((jpg == NULL) || (jpg_len == 0))
    {
        ESP_LOGW(TAG, "记录 %u JPEG 编码失败，跳过上传", (unsigned)item->seq);
        free(jpg);
        return true;
    }

    for (int attempt = 1; (attempt <= UPLOAD_RETRY_MAX) && !ok; attempt++)
    {
        ok = http_upload(jpg, jpg_len, img_url, sizeof(img_url));
        if (!ok && (attempt < UPLOAD_RETRY_MAX))
        {
            ESP_LOGW(TAG, "第 %d 次上传失败，%d ms 后重试", attempt, UPLOAD_RETRY_DELAY);
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY));
            wait_online();
        }
    }

    if (ok)
    {
        char time_buf[32];
        char msg[96];
        time_t ts = (time_t)(item->epoch ? item->epoch : (uint32_t)time_sync_epoch());
        struct tm tm_now;

        human_face_history_mark_uploaded(item->seq);

        localtime_r(&ts, &tm_now);
        strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M:%S", &tm_now);
        snprintf(msg, sizeof(msg), "识别 %s %s %d%%", item->name, time_buf, item->sim);
        bemfa_mqtt_publish_status(msg);

        /* 只推实时识别；开机补传的旧记录不推微信，避免连发刷屏 */
        if ((item->epoch != 0) &&
            (((uint32_t)time(NULL) - item->epoch) <= WECHAT_REALTIME_SEC))
        {
            for (int attempt = 1; attempt <= 2; attempt++)
            {
                if (wechat_notify(msg, img_url))
                {
                    break;
                }
                if (attempt < 2)
                {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }

        ESP_LOGI(TAG, "照片已上传：%s（%u 字节，序号 %u）", item->name,
                 (unsigned)jpg_len, (unsigned)item->seq);
    }
    else
    {
        ESP_LOGW(TAG, "记录 %u 上传失败，保留未上传标记（下次开机补传）",
                 (unsigned)item->seq);
    }

    free(jpg);
    return ok;
}

/**
 * @brief 上传任务：逐条处理队列中的记录
 */
static void upload_task(void *arg)
{
    upload_item_t item;

    (void)arg;

    /* 开机补传：把上次没传成功的记录重新排队（新到旧的顺序不影响结果） */
    {
        int count = human_face_history_count();

        for (int i = 0; i < count; i++)
        {
            human_face_history_meta_t meta;

            if (human_face_history_get_meta(i, &meta) != 1)
            {
                break;
            }
            if (meta.uploaded)
            {
                continue;
            }
            bemfa_uploader_enqueue(meta.seq, meta.epoch, meta.name, meta.sim);
        }
    }

    for (;;)
    {
        if (xQueuePeek(s_queue, &item, pdMS_TO_TICKS(10000)) != pdTRUE)
        {
            /* 队列空闲：给 MQTT 一次启动/重试机会（首次启动常因内存不足失败） */
            bemfa_mqtt_ensure_started();
            continue;
        }

        wait_online();
        process_item(&item);

        /* 成功与失败都出队：失败记录保持“未上传”，下次开机由补传重新入队 */
        xQueueReceive(s_queue, &item, 0);

        bemfa_mqtt_ensure_started();
    }
}

void bemfa_uploader_start(void)
{
    if (s_started)
    {
        return;
    }

    s_queue = xQueueCreate(UPLOAD_QUEUE_LEN, sizeof(upload_item_t));
    s_pixels = (uint16_t *)heap_caps_malloc(FACE_HISTORY_PIXELS * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM);
    if ((s_queue == NULL) || (s_pixels == NULL))
    {
        ESP_LOGE(TAG, "照片上传初始化失败（内存不足）");
        if (s_queue != NULL)
        {
            vQueueDelete(s_queue);
            s_queue = NULL;
        }
        if (s_pixels != NULL)
        {
            heap_caps_free(s_pixels);
            s_pixels = NULL;
        }
        return;
    }

    s_started = true;
    if (xTaskCreatePinnedToCore(upload_task, "bemfa_upload", UPLOAD_TASK_STACK, NULL,
                                UPLOAD_TASK_PRIO, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "照片上传任务创建失败（内存不足）");
        s_started = false;
        vQueueDelete(s_queue);
        s_queue = NULL;
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return;
    }

    ESP_LOGI(TAG, "照片上传任务已启动（图片主题 %s）", BEMFA_IMAGE_TOPIC);
}

bool bemfa_uploader_enqueue(uint32_t seq, uint32_t epoch, const char *name, int sim)
{
    upload_item_t item;

    if (!s_started || (s_queue == NULL))
    {
        return false;
    }

    memset(&item, 0, sizeof(item));
    item.seq = seq;
    item.epoch = epoch;
    item.sim = sim;
    snprintf(item.name, sizeof(item.name), "%s", (name != NULL) ? name : "未知");

    if (xQueueSend(s_queue, &item, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "上传队列已满，记录 %u 暂不上传（下次开机补传）", (unsigned)seq);
        return false;
    }
    return true;
}
