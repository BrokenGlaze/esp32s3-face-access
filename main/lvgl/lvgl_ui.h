/**
 * @file    lvgl_ui.h
 * @brief   LVGL 界面模块对外的统一入口：LVGL 端口初始化 + 三个界面切换
 *
 * @note    使用说明：
 *          - 初始化顺序：i2c_init -> pca9557_init -> lcd_init -> tp_init -> lvgl_ui_init
 *          - 预览、按钮、文字全部是 LVGL 对象，业务任务只能通过本模块接口更新，
 *            不得再调用 esp_lcd_panel_draw_bitmap / lcd_fill_rect 等裸绘接口，
 *            否则会与 LVGL 的 flush 抢 SPI 总线
 *          - 所有接口内部自行加/解 LVGL 锁（lvgl_port_lock/unlock），可在任意任务中调用
 *          - 界面分别位于 ui_face / ui_login / ui_admin / ui_face_db / ui_history 模块：
 *              ui_face   人脸识别界面（摄像头预览 + 状态横幅 + “登入”/“识别”按钮）
 *              ui_login  登入界面（标题 + 密码掩码 + 提示 + 退出 + 3x4 数字键盘）
 *              ui_admin  管理员配置界面（标题 + 返回按钮）
 *              ui_face_db 人脸信息库界面（已录入名单 + 删除 + 返回）
 *              ui_history 识别历史记录界面（3 行 3 列照片 + 放大查看 + 返回）
 *              ui_wifi    网络设置界面（Wi-Fi 状态 + 手机网页配网）
 *          - 中文文字使用 font 目录的点阵字库渲染成 RGB565 图像，再交给 lv_img 显示，
 *            避免引入 LVGL 的 CJK 字库
 */

#ifndef __LVGL_UI_H__
#define __LVGL_UI_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lcd.h"

/* ==================== 界面布局（横屏 480x320） ====================
 *   x: [0, 140)      左侧空闲区：上三分之一放“登入”按钮、
 *                    下三分之一放“识别”按钮
 *   x: [140, 480)    右侧摄像头预览 340x320，识别状态横幅显示在预览下方
 * 想调整预览区域，只改这几个宏即可（需满足 X+W <= LCD_WIDTH、Y+H <= LCD_HEIGHT）。
 */
#define UI_PREVIEW_W 340                            /* 预览区域宽度（像素） */
#define UI_PREVIEW_H 320                            /* 预览区域高度（像素） */
#define UI_PREVIEW_X (LCD_WIDTH - UI_PREVIEW_W)     /* 预览区域左上角 X（140） */
#define UI_PREVIEW_Y 0                              /* 预览区域左上角 Y */

#define UI_INFO_FONT 16                             /* 信息文字字号（仅支持 16/32） */

#define UI_BTN_W 70                                 /* 按钮宽度（图像 70x70） */
#define UI_BTN_H 70                                 /* 按钮高度 */
#define UI_BTN_X ((UI_PREVIEW_X - UI_BTN_W) / 2)     /* 空闲区水平居中（35） */
#define UI_BTN_Y (LCD_HEIGHT / 6 - UI_BTN_H / 2)    /* “登入”按钮：上三分之一中心（18） */
#define UI_BTN2_Y (LCD_HEIGHT * 5 / 6 - UI_BTN_H / 2) /* “识别”按钮：下三分之一中心（232） */

/**
 * @brief 识别状态横幅的类别（决定横幅配色）
 */
typedef enum
{
    UI_STATUS_INFO = 0, /* 一般信息（深灰蓝） */
    UI_STATUS_OK,       /* 成功（绿） */
    UI_STATUS_FAIL,     /* 失败（红） */
    UI_STATUS_WARN      /* 提示/警告（橙黄） */
} ui_status_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL 端口并创建全部界面（必须在 lcd_init() 与 tp_init() 之后调用）
 * @return ESP_OK 表示成功
 */
esp_err_t lvgl_ui_init(void);

/**
 * @brief 注册“登入”按钮按下回调（按下瞬间触发一次）
 * @param cb 回调函数，传 NULL 表示取消
 */
void lvgl_ui_set_login_cb(void (*cb)(void));

/**
 * @brief 注册“识别”按钮按下回调（按下瞬间触发一次）
 * @param cb 回调函数，传 NULL 表示取消
 * @note 业务侧把它接到与 KEY2 单击相同的处理逻辑上
 */
void lvgl_ui_set_recognize_cb(void (*cb)(void));

/**
 * @brief 注册管理员界面“录入”按钮按下回调（按下瞬间触发一次）
 * @param cb 回调函数，传 NULL 表示取消
 */
void lvgl_ui_set_enroll_cb(void (*cb)(void));

/**
 * @brief 注册登入页“退出”按钮回调（按下后触发，界面自动回到人脸识别界面）
 * @param cb 回调函数，传 NULL 表示取消
 */
void lvgl_ui_set_exit_cb(void (*cb)(void));

/**
 * @brief 更新摄像头预览画面（RGB565，最近邻缩放到 UI_PREVIEW_W x UI_PREVIEW_H）
 * @param src    源像素数据（摄像头帧，字节序与屏幕一致）
 * @param src_w  源图宽度（像素）
 * @param src_h  源图高度（像素）
 * @note 内部持 LVGL 锁完成缩放与重绘请求，可每帧调用
 */
void lvgl_ui_preview_frame(const uint16_t *src, uint32_t src_w, uint32_t src_h);

/**
 * @brief 在人脸识别界面（预览下方）显示识别状态横幅
 *
 * 横幅为整块彩色圆角条：第一行是大号标题（32 像素高），第二行是细节文字（16 像素高）。
 * 颜色由 kind 决定：成功=绿、失败=红、提示=橙黄、一般信息=深灰蓝。
 *
 * @param kind   状态类别（见 ui_status_t）
 * @param title  标题（如“识别成功”“未检测到人脸”）
 * @param detail 细节文字（如“Person_0 92%”），可为 NULL 或空串（此时只显示标题）
 */
void lvgl_ui_set_status(int kind, const char *title, const char *detail);

/**
 * @brief 清除识别状态横幅
 */
void lvgl_ui_clear_status(void);

/**
 * @brief 在管理员配置界面顶部弹出一条临时提示（3 秒后自动消失）
 * @param text 提示文字（UTF-8）
 * @note 内部自行加/解 LVGL 锁，可在任意任务中调用
 */
void lvgl_ui_admin_toast(const char *text);

/**
 * @brief 切换到登入界面（覆盖全屏，隐藏人脸识别界面）
 */
void lvgl_ui_show_login(void);

/**
 * @brief 切换到人脸识别界面（隐藏登入页/管理员页/键盘/弹窗）
 */
void lvgl_ui_show_preview(void);

/**
 * @brief   只显示摄像头画面（隐藏登入页、管理员页、两个按钮；状态横幅仍按内容显示）
 * @note 人脸录入时使用，便于用户对准摄像头
 */
void lvgl_ui_show_camera_only(void);

/**
 * @brief 切换到管理员配置界面（隐藏登入页与人脸识别界面）
 */
void lvgl_ui_show_admin(void);

/**
 * @brief 查询当前是否处于管理员配置界面
 * @return true 表示管理员界面正在显示
 * @note 仅读取状态标志，可在任意任务中调用；业务侧用它限制人脸录入权限
 */
bool lvgl_ui_admin_active(void);

/**
 * @brief 查询系统是否处于待机状态
 * @return true 表示已进入待机（10 秒无触摸后熄屏，触摸屏幕后自动唤醒）
 * @note 待机期间 LVGL 仍保持运行以轮询触摸，但人脸识别流水线（采集/AI/显示）
 *       会通过本接口判断并暂停；可在任意任务中调用
 */
bool lvgl_ui_standby_active(void);

#ifdef __cplusplus
}
#endif

#endif
