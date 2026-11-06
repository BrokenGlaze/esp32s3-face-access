/**
 * @file    ui_history.h
 * @brief   识别历史记录界面：大标题 + 3 行 3 列人脸照片 + 中心下方“返回”
 *
 * @note    照片由识别模块的 human_face_history_count()/human_face_history_image()
 *          提供（线程安全、掉电保存），每次显示界面时重新拉取一次。
 *          点击任意一张照片会在屏幕中心以 3/4 屏幕尺寸放大显示，
 *          再点击任意位置即可退出查看。
 *
 * @note    所有函数不持 LVGL 锁，调用者必须已持有（lvgl_port_lock）；
 *          或在 LVGL 事件回调内调用。
 */

#ifndef __UI_HISTORY_H__
#define __UI_HISTORY_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建识别历史记录界面（默认隐藏）
 * @param scr 屏幕对象
 */
void ui_history_create(lv_obj_t *scr);

/**
 * @brief 显示识别历史记录界面，并刷新历史照片
 */
void ui_history_show(void);

/**
 * @brief 隐藏识别历史记录界面（同时关闭大图查看）
 */
void ui_history_hide(void);

#ifdef __cplusplus
}
#endif

#endif
