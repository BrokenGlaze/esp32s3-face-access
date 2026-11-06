/**
 * @file    ui_admin.h
 * @brief   管理员配置界面：大标题 + “录入人脸”“已录入人脸”“历史记录”“系统设置”按钮 + 右下角“返回”
 *
 * @note    系统设置子页可开关语音播报、调节播报音量、调节人脸识别相似度、
 *          修改管理员密码（需连续输入两次），所有设置都会写入 NVS，重启后保持。
 *          “历史记录”进入识别历史记录界面（最近 9 条识别成功的照片，掉电保存）。
 *
 * @note    所有函数不持 LVGL 锁，调用者必须已持有（lvgl_port_lock）；
 *          或在 LVGL 事件回调内调用。
 */

#ifndef __UI_ADMIN_H__
#define __UI_ADMIN_H__

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建管理员配置界面（默认隐藏）
 * @param scr 屏幕对象
 */
void ui_admin_create(lv_obj_t *scr);

/**
 * @brief 显示管理员配置界面
 */
void ui_admin_show(void);

/**
 * @brief 隐藏管理员配置界面
 */
void ui_admin_hide(void);

/**
 * @brief 注册“录入人脸”按钮点击回调
 * @param cb 回调函数，传 NULL 表示取消
 * @note 业务侧接到该回调后切换为纯摄像头画面并开始人脸录入
 */
void ui_admin_set_enroll_cb(void (*cb)(void));

/**
 * @brief 查询管理员配置界面是否正在显示
 * @return true 表示当前处于管理员界面（此时才允许人脸录入）
 * @note 只读一个状态标志，不涉及 LVGL 对象，任意任务可直接调用
 */
bool ui_admin_is_visible(void);

/**
 * @brief 在管理员界面顶部弹出一条临时提示（3 秒后自动消失）
 * @param text 提示文字（UTF-8）
 * @note 用于“已达录入上限”等反馈；调用者必须已持有 LVGL 锁
 */
void ui_admin_show_tip(const char *text);

#ifdef __cplusplus
}
#endif

#endif
