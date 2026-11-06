/**
 * @file    ui_face_db.h
 * @brief   人脸信息库界面：显示已录入人脸名单，右下角“返回”回到管理员界面
 *
 * @note    所有函数不持 LVGL 锁，调用者必须已持有（lvgl_port_lock）；
 *          或在 LVGL 事件回调内调用。
 */

#ifndef __UI_FACE_DB_H__
#define __UI_FACE_DB_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建人脸信息库界面（默认隐藏）
 * @param scr 屏幕对象
 */
void ui_face_db_create(lv_obj_t *scr);

/**
 * @brief 显示人脸信息库界面，并刷新已录入人脸名单
 */
void ui_face_db_show(void);

/**
 * @brief 隐藏人脸信息库界面
 */
void ui_face_db_hide(void);

#ifdef __cplusplus
}
#endif

#endif
