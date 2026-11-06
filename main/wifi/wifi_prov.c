/**
 * @file    wifi_prov.c
 * @brief   手机网页配网实现（SoftAP + esp_http_server）
 *
 * @note    - 页面与扫描结果都放在 PSRAM，避免占用紧张的内部 RAM
 *          - 配网写入 NVS 前会暂停摄像头采集，避免写 Flash 与摄像头 DMA 冲突
 *          - 拍照/识别的历史与 Wi-Fi 无关，配网失败不影响本地功能
 */

#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "who_human_face_recognition.hpp"

#define TAG "wifi_prov"

#define PROV_PAGE_MAX     8192   /* 配网页面缓冲上限（PSRAM） */
#define PROV_MAX_AP       12     /* 页面最多列出的热点数 */
#define PROV_BODY_MAX     256    /* POST 表单最大长度 */

static httpd_handle_t s_server = NULL;
static bool s_running = false;

/* ==================== 小工具 ==================== */

/**
 * @brief 把字符串末尾追加到缓冲（自动防溢出）
 */
static void append_str(char *buf, size_t *used, size_t cap, const char *s)
{
    int n;

    if (*used >= cap - 1)
    {
        return;
    }
    n = snprintf(buf + *used, cap - *used, "%s", s);
    if (n > 0)
    {
        *used += ((size_t)n < cap - *used) ? (size_t)n : (cap - *used - 1);
    }
}

/**
 * @brief HTML 转义（用于把 SSID 放进页面）
 */
static void html_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; i++)
    {
        switch (in[i])
        {
        case '&':
            if (o + 5 >= out_len) goto done;
            memcpy(out + o, "&amp;", 5);
            o += 5;
            break;
        case '<':
            if (o + 4 >= out_len) goto done;
            memcpy(out + o, "&lt;", 4);
            o += 4;
            break;
        case '>':
            if (o + 4 >= out_len) goto done;
            memcpy(out + o, "&gt;", 4);
            o += 4;
            break;
        case '"':
            if (o + 6 >= out_len) goto done;
            memcpy(out + o, "&quot;", 6);
            o += 6;
            break;
        default:
            out[o++] = in[i];
            break;
        }
    }
done:
    out[o] = '\0';
}

/**
 * @brief JSON 字符串转义（用于 /status）
 */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; i++)
    {
        unsigned char c = (unsigned char)in[i];

        if (c == '"' || c == '\\')
        {
            if (o + 2 >= out_len) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        }
        else if (c < 0x20)
        {
            continue; /* 控制字符直接跳过 */
        }
        else
        {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/**
 * @brief URL 解码（application/x-www-form-urlencoded）
 */
static void url_decode(const char *src, size_t len, char *out, size_t out_len)
{
    size_t o = 0;

    for (size_t i = 0; i < len && o + 1 < out_len; i++)
    {
        if (src[i] == '+')
        {
            out[o++] = ' ';
        }
        else if (src[i] == '%' && i + 2 < len)
        {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else
        {
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
}

/**
 * @brief 从表单串里取出某个字段
 */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;

    out[0] = '\0';
    while (p != NULL && *p != '\0')
    {
        const char *eq = strchr(p, '=');
        const char *amp;

        if (eq == NULL)
        {
            break;
        }
        amp = strchr(eq, '&');

        if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0)
        {
            size_t vlen = (amp != NULL) ? (size_t)(amp - eq - 1) : strlen(eq + 1);

            url_decode(eq + 1, vlen, out, out_len);
            return out[0] != '\0';
        }
        p = (amp != NULL) ? amp + 1 : NULL;
    }
    return false;
}

/* ==================== HTTP 处理 ==================== */

/**
 * @brief 生成配网首页（列出扫描到的热点 + 手动输入）
 */
static esp_err_t prov_root_get(httpd_req_t *req)
{
    char *page;
    size_t used = 0;
    wifi_mgr_ap_t aps[PROV_MAX_AP];
    int ap_count;

    page = (char *)heap_caps_malloc(PROV_PAGE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (page == NULL)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    append_str(page, &used, PROV_PAGE_MAX,
               "<!doctype html><html><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>ESP32 门禁配网</title>"
               "<style>body{font-family:sans-serif;margin:16px;background:#f6f7f9;color:#222}"
               "h2{margin:0 0 12px}label{display:block;margin-top:16px;font-weight:600}"
               "select,input{width:100%;padding:10px;font-size:16px;box-sizing:border-box}"
               "button{margin-top:20px;width:100%;padding:12px;font-size:17px;background:#2f81f7;"
               "color:#fff;border:0;border-radius:8px}</style></head><body>");
    append_str(page, &used, PROV_PAGE_MAX, "<h2>ESP32 门禁 · Wi-Fi 配网</h2>");

    /* 扫描附近热点（阻塞约 1~2 秒） */
    ap_count = wifi_manager_scan(aps, PROV_MAX_AP);

    append_str(page, &used, PROV_PAGE_MAX,
               "<form method=\"post\" action=\"/connect\">"
               "<label>附近的 Wi-Fi</label><select name=\"ssid\"><option value=\"\">"
               "（请选择）</option>");
    for (int i = 0; i < ap_count; i++)
    {
        char esc[96];

        html_escape(aps[i].ssid, esc, sizeof(esc));
        append_str(page, &used, PROV_PAGE_MAX, "<option value=\"");
        append_str(page, &used, PROV_PAGE_MAX, esc);
        append_str(page, &used, PROV_PAGE_MAX, "\">");
        append_str(page, &used, PROV_PAGE_MAX, esc);
        append_str(page, &used, PROV_PAGE_MAX, "</option>");
    }
    append_str(page, &used, PROV_PAGE_MAX,
               "</select><label>或手动输入 SSID</label>"
               "<input name=\"manual\" maxlength=\"32\" placeholder=\"隐藏网络/手动输入\">"
               "<label>密码</label><input name=\"pwd\" type=\"password\" maxlength=\"64\">"
               "<button type=\"submit\">连接</button></form>"
               "<p id=\"msg\">提交后请稍候，页面会自动刷新连接状态。</p>"
               "<script>"
               "const m=document.getElementById('msg');"
               "setInterval(async()=>{try{const r=await fetch('/status');const j=await r.json();"
               "if(j.state===2){m.textContent='已连接 '+j.ssid+'，IP '+j.ip;}"
               "else if(j.state===1){m.textContent='正在连接…';}"
               "else if(j.state===3){m.textContent='连接失败，请检查密码后重试';}"
               "else{m.textContent='等待配网…';}}catch(e){}},1000);"
               "</script></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, used);
    free(page);
    return ESP_OK;
}

/**
 * @brief 处理提交的 SSID/密码：保存并连接
 */
static esp_err_t prov_connect_post(httpd_req_t *req)
{
    char body[PROV_BODY_MAX];
    char ssid[33] = "";
    char manual[33] = "";
    char pwd[65] = "";
    const char *use_ssid;
    size_t got = 0;
    size_t want = (req->content_len < sizeof(body) - 1) ? req->content_len : sizeof(body) - 1;

    while (got < want)
    {
        int r = httpd_req_recv(req, body + got, (int)(want - got));

        if (r <= 0)
        {
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            break;
        }
        got += (size_t)r;
    }
    body[got] = '\0';

    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "manual", manual, sizeof(manual));
    form_get(body, "pwd", pwd, sizeof(pwd));

    use_ssid = (manual[0] != '\0') ? manual : ssid;
    if (use_ssid[0] == '\0')
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<p style=\"font-family:sans-serif\">未选择 Wi-Fi，"
                                "请返回上一页重试。</p>");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "收到配网请求：SSID=%s", use_ssid);

    /* 连接受理会写 NVS：先暂停摄像头采集，避免与摄像头 DMA 冲突 */
    human_face_capture_pause();
    wifi_manager_connect(use_ssid, pwd);
    human_face_capture_resume();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
                       "<!doctype html><html><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "</head><body style=\"font-family:sans-serif;margin:24px\">"
                       "<h3>正在连接…</h3><p id=\"m\">请稍候</p>"
                       "<script>const m=document.getElementById('m');"
                       "setInterval(async()=>{try{const r=await fetch('/status');const j=await r.json();"
                       "if(j.state===2){m.textContent='已连接 '+j.ssid+'，IP '+j.ip;}"
                       "else if(j.state===3){m.textContent='连接失败，请返回重试';}}catch(e){}},1000);"
                       "</script></body></html>");
    return ESP_OK;
}

/**
 * @brief 返回当前连接状态 JSON
 */
static esp_err_t prov_status_get(httpd_req_t *req)
{
    char ssid[96];
    char ip[32];
    char json[192];

    json_escape(wifi_manager_get_ssid(), ssid, sizeof(ssid));
    json_escape(wifi_manager_get_ip(), ip, sizeof(ip));
    snprintf(json, sizeof(json), "{\"state\":%d,\"ssid\":\"%s\",\"ip\":\"%s\"}",
             (int)wifi_manager_get_state(), ssid, ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ==================== 启动/停止 ==================== */

esp_err_t wifi_prov_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    err = wifi_manager_ap_start(WIFI_PROV_AP_SSID, NULL); /* 开放热点 */
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "开启配网热点失败，ret=%d", (int)err);
        return err;
    }

    cfg.server_port = 80;
    cfg.max_open_sockets = 2;
    cfg.max_uri_handlers = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;

    if (httpd_start(&s_server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "启动配网 HTTP 服务失败");
        wifi_manager_ap_stop();
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = prov_root_get};
    httpd_uri_t uri_connect = {.uri = "/connect", .method = HTTP_POST,
                               .handler = prov_connect_post};
    httpd_uri_t uri_status = {.uri = "/status", .method = HTTP_GET,
                              .handler = prov_status_get};

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_connect);
    httpd_register_uri_handler(s_server, &uri_status);

    s_running = true;
    ESP_LOGI(TAG, "配网服务已启动：热点 %s，地址 http://%s",
             WIFI_PROV_AP_SSID, WIFI_PROV_AP_URL);
    return ESP_OK;
}

void wifi_prov_stop(void)
{
    if (!s_running)
    {
        return;
    }

    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
    wifi_manager_ap_stop();
    s_running = false;
    ESP_LOGI(TAG, "配网服务已停止");
}

bool wifi_prov_is_running(void)
{
    return s_running;
}
