/**
 * @file    bemfa_mqtt.h
 * @brief   巴法云 MQTT：设备在线状态与识别事件通知
 *
 * @note    图片本身走巴法云图片接口（HTTP），MQTT 只用于短文本状态/通知。
 */

#ifndef __BEMFA_MQTT_H__
#define __BEMFA_MQTT_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 MQTT 客户端（Wi-Fi 获取 IP 后自动连接，断线自动重连）
 */
void bemfa_mqtt_start(void);

/**
 * @brief 发布一条状态/通知到巴法云主题（topic/up）
 * @param text 文本（建议 < 512 字节）
 * @note 未连接时直接丢弃，不影响本地功能
 */
void bemfa_mqtt_publish_status(const char *text);

/**
 * @brief 查询 MQTT 是否已连接
 */
bool bemfa_mqtt_is_connected(void);

/**
 * @brief 若启动曾失败（内存不足），在网络空闲时重试启动
 * @note 可在低优先级任务里周期性调用，失败时无副作用
 */
void bemfa_mqtt_ensure_started(void);

#ifdef __cplusplus
}
#endif

#endif
