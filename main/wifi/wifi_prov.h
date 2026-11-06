/**
 * @file    wifi_prov.h
 * @brief   手机网页配网：设备开热点 + HTTP 页面填写 Wi-Fi 账号密码
 *
 * @note    流程：wifi_prov_start() 开启热点与网页服务 → 手机连接热点后
 *          浏览器打开 192.168.4.1 → 选择/填写 Wi-Fi → 提交 → 设备连接 →
 *          页面轮询 /status 显示结果。离开网络设置页或调用 wifi_prov_stop()
 *          会关闭热点与网页服务。
 */

#ifndef __WIFI_PROV_H__
#define __WIFI_PROV_H__

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROV_AP_SSID "ESP32-Setup"   /* 配网热点名称 */
#define WIFI_PROV_AP_URL  "192.168.4.1"   /* 配网页面地址 */

/**
 * @brief 开启配网热点与 HTTP 配网页面
 * @return ESP_OK 成功；其他为错误码
 */
esp_err_t wifi_prov_start(void);

/**
 * @brief 关闭配网服务并恢复纯 STA 模式
 */
void wifi_prov_stop(void);

/**
 * @brief 查询配网服务是否在运行
 */
bool wifi_prov_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
