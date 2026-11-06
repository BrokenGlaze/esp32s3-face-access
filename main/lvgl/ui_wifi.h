/**
 * @file    ui_wifi.h
 * @brief   网络设置界面：当前 Wi-Fi 状态 + 手机配网 + 重新连接 + 返回
 *
 * @note    配网通过 wifi_prov（设备热点 + 网页）完成；本页只负责显示状态与
 *          启动/停止配网服务。所有函数不持 LVGL 锁，调用者必须已持有。
 */

#ifndef __UI_WIFI_H__
#define __UI_WIFI_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建网络设置界面（默认隐藏）
 * @param scr 屏幕对象
 */
void ui_wifi_create(lv_obj_t *scr);

/**
 * @brief 显示网络设置界面，并启动状态刷新定时器
 */
void ui_wifi_show(void);

/**
 * @brief 隐藏网络设置界面（同时停止配网热点/网页服务与刷新定时器）
 */
void ui_wifi_hide(void);

#ifdef __cplusplus
}
#endif

#endif
