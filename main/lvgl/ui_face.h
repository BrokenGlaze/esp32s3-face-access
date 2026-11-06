/**
 * @file    ui_face.h
 * @brief   人脸识别界面：摄像头预览 + 信息横幅 + “登入”/“识别”按钮
 *
 * @note    所有函数不持 LVGL 锁，调用者必须已持有（lvgl_port_lock）；
 *          或在 LVGL 事件回调内调用。
 */

#ifndef __UI_FACE_H__
#define __UI_FACE_H__

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建人脸识别界面（预览、信息区、登入/识别按钮）
 * @param scr 屏幕对象
 */
void ui_face_create(lv_obj_t *scr);

/**
 * @brief 显示人脸识别界面（预览 + 两个按钮 + 有内容时显示信息区）
 */
void ui_face_show(void);

/**
 * @brief 只显示摄像头预览（隐藏两个按钮与信息区）
 * @note 人脸录入时使用，避免按钮遮挡视线
 */
void ui_face_show_camera_only(void);

/**
 * @brief 隐藏人脸识别界面（预览、信息区、两个按钮全部隐藏）
 */
void ui_face_hide(void);

/**
 * @brief 更新摄像头预览画面（RGB565，最近邻缩放到 UI_PREVIEW_W x UI_PREVIEW_H）
 * @param src   源像素数据（摄像头帧，字节序与屏幕一致）
 * @param src_w 源图宽度（像素）
 * @param src_h 源图高度（像素）
 */
void ui_face_frame(const uint16_t *src, uint32_t src_w, uint32_t src_h);

/**
 * @brief 在预览画面下方显示识别状态横幅（彩色圆角条 + 大号标题 + 细节文字）
 * @param kind   状态类别（UI_STATUS_OK/FAIL/WARN/INFO）
 * @param title  标题（如“识别成功”“未检测到人脸”）
 * @param detail 细节文字（如“Person_0 92%”），可为 NULL 或空字符串
 */
void ui_face_set_status(int kind, const char *title, const char *detail);

/**
 * @brief 清除识别状态横幅
 */
void ui_face_clear_status(void);

/**
 * @brief 注册“登入”按钮按下回调（按下瞬间触发一次）
 * @param cb 回调函数，传 NULL 表示取消
 */
void ui_face_set_login_cb(void (*cb)(void));

/**
 * @brief 注册“识别”按钮按下回调（按下瞬间触发一次）
 * @param cb 回调函数，传 NULL 表示取消
 */
void ui_face_set_recognize_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif
