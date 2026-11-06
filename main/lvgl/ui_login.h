/**
 * @file    ui_login.h
 * @brief   登入界面：标题 + 密码掩码 + 提示 + “退出”按钮 + 右侧 3x4 数字键盘
 *
 * @note    密码正确后弹出“进入管理员配置界面”提示，随后自动切到管理员界面。
 *          所有函数不持 LVGL 锁，调用者必须已持有（lvgl_port_lock）；
 *          或在 LVGL 事件回调内调用。
 */

#ifndef __UI_LOGIN_H__
#define __UI_LOGIN_H__

#define UI_LOGIN_PWD_LEN 6 /* 管理员密码位数（纯数字） */

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建登入界面（默认隐藏）
 * @param scr 屏幕对象
 */
void ui_login_create(lv_obj_t *scr);

/**
 * @brief 显示登入界面（复位密码输入、关闭键盘与提示）
 */
void ui_login_show(void);

/**
 * @brief 隐藏登入界面（同时收起数字键盘与提示弹窗）
 */
void ui_login_hide(void);

/**
 * @brief 注册“退出”按钮回调（按下后触发，界面自动回到人脸识别界面）
 * @param cb 回调函数，传 NULL 表示取消
 */
void ui_login_set_exit_cb(void (*cb)(void));

/**
 * @brief 修改管理员密码，并写入 NVS 掉电保存
 * @param pwd 6 位纯数字新密码；长度不为 UI_LOGIN_PWD_LEN 时忽略本次修改
 */
void ui_login_set_password(const char *pwd);

#ifdef __cplusplus
}
#endif

#endif
