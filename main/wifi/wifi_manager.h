/**
 * @file    wifi_manager.h
 * @brief   Wi-Fi STA 管理：初始化、扫描附近热点、连接、断线重连、凭据保存
 *
 * @note    基于 ESP-IDF examples/wifi 的 scan 与 station 示例改写：
 *          - 初始化不阻塞等待连接，网络状态通过状态变量对外提供
 *          - Wi-Fi 驱动配置只放 RAM（WIFI_STORAGE_RAM），凭据由本模块写入 NVS
 *          - 所有接口可在任意任务中调用；扫描为阻塞操作（约 1~2 秒），
 *            不要在 LVGL 事件回调里直接调用
 */

#ifndef __WIFI_MANAGER_H__
#define __WIFI_MANAGER_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_SSID_LEN 32        /* SSID 最大字节数（不含结束符） */
#define WIFI_MANAGER_PWD_LEN  64        /* 密码最大字节数（不含结束符） */

/**
 * @brief Wi-Fi 连接状态
 */
typedef enum
{
    WIFI_MGR_IDLE = 0,      /* 未配置/未连接 */
    WIFI_MGR_CONNECTING,    /* 正在连接（含自动重连） */
    WIFI_MGR_CONNECTED,     /* 已连接并获取到 IP */
    WIFI_MGR_FAILED,        /* 连接失败（重试次数用尽） */
} wifi_mgr_state_t;

/**
 * @brief 扫描到的一个热点
 */
typedef struct
{
    char ssid[WIFI_MANAGER_SSID_LEN + 1]; /* 热点名称 */
    int8_t rssi;                          /* 信号强度（dBm） */
    uint8_t authmode;                     /* 认证方式（wifi_auth_mode_t） */
} wifi_mgr_ap_t;

/**
 * @brief 初始化 Wi-Fi STA（不阻塞等待连接）
 *
 * 会初始化 netif/事件循环/Wi-Fi 驱动，并尝试用 NVS 中保存的凭据自动连接。
 * 重复调用安全（已初始化时直接返回 ESP_OK）。
 *
 * @return ESP_OK 成功；其他为 esp_err_t 错误码
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 查询 Wi-Fi 模块是否已初始化
 */
bool wifi_manager_is_started(void);

/**
 * @brief 扫描附近热点（阻塞，约 1~2 秒）
 *
 * @param out 输出数组（按信号强度由驱动排序）
 * @param max 数组最大条目数
 * @return 实际写入条目数；-1 表示失败（未初始化或扫描失败）
 */
int wifi_manager_scan(wifi_mgr_ap_t *out, int max);

/**
 * @brief 连接指定热点，并把凭据写入 NVS（成功后下次开机会自动连接）
 *
 * @param ssid     热点名称（不能为空，最长 32 字节）
 * @param password 密码，开放网络传空字符串或 NULL
 * @return ESP_OK 已下发连接请求；其他为错误码
 * @note 本函数不等待连接结果，结果通过 wifi_manager_get_state() 查询
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief 断开当前连接（保留 NVS 中的凭据）
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief 查询当前连接状态
 */
wifi_mgr_state_t wifi_manager_get_state(void);

/**
 * @brief 查询当前（或最近一次）连接的 SSID
 */
const char *wifi_manager_get_ssid(void);

/**
 * @brief 查询当前 IP（未连接时为空字符串）
 */
const char *wifi_manager_get_ip(void);

/**
 * @brief 查询 NVS 中是否保存了凭据
 */
bool wifi_manager_has_credentials(void);

/**
 * @brief 用 NVS 中已保存的凭据重新连接（不写 Flash）
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * @brief 开启配网热点（STA+AP 模式，默认 192.168.4.1）
 *
 * @param ssid     热点名称（如 "ESP32-Setup"）
 * @param password 热点密码；NULL 或空串表示开放热点
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_ap_start(const char *ssid, const char *password);

/**
 * @brief 关闭配网热点，恢复纯 STA 模式
 */
esp_err_t wifi_manager_ap_stop(void);

#ifdef __cplusplus
}
#endif

#endif
