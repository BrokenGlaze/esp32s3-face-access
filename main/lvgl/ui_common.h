/**
 * @file    ui_common.h
 * @brief   LVGL 界面公共组件：PSRAM 缓冲、RGB565 图像、点阵文字、按钮与遮罩
 *
 * @note    本模块只提供底层绘制原语，被 ui_face / ui_login / ui_admin 复用。
 *          所有函数不持 LVGL 锁，调用者必须已持有（lvgl_port_lock）；
 *          或在 LVGL 事件回调（LVGL 任务上下文）内调用。
 */

#ifndef __UI_COMMON_H__
#define __UI_COMMON_H__

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* 颜色（标准 RGB565） */
#define UI_COLOR_WHITE 0xFFFF
#define UI_COLOR_GRAY  0xC618
#define UI_COLOR_RED   0xF800

/* 0xRRGGBB -> 标准 RGB565，供点阵字库渲染使用 */
#define UI_RGB565(hex) (uint16_t)((((hex) >> 8) & 0xF800) | \
                                  (((hex) >> 5) & 0x07E0) | \
                                  (((hex) >> 3) & 0x001F))

/**
 * @struct ui_text_t
 * @brief 点阵字库文字图像（固定尺寸缓冲，文字在缓冲内居中）
 */
typedef struct
{
    lv_obj_t *img;      /* 图像控件 */
    lv_img_dsc_t dsc;   /* 图像描述 */
    uint16_t *buf;      /* RGB565 缓冲（PSRAM） */
    uint32_t w;         /* 缓冲宽度 */
    uint32_t h;         /* 缓冲高度 */
} ui_text_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 PSRAM 分配一块 8 位对齐的缓冲
 * @param size 字节数
 * @return 成功返回指针，失败返回 NULL
 */
void *ui_alloc(size_t size);

/**
 * @brief 创建一张 RGB565 图像对象
 * @param parent 父对象
 * @param dsc    图像描述结构（由本函数填充）
 * @param buf    RGB565 像素数据（LVGL 字节序）
 * @param w      宽度（像素）
 * @param h      高度（像素）
 * @param x      左上角 X
 * @param y      左上角 Y
 * @return 图像对象
 */
lv_obj_t *ui_image_create(lv_obj_t *parent, lv_img_dsc_t *dsc, const void *buf,
                          uint32_t w, uint32_t h, lv_coord_t x, lv_coord_t y);

/**
 * @brief 按字库排版规则计算 UTF-8 文字宽度（中文 = 字号，ASCII = 字号/2）
 * @param text 字符串
 * @param font 字号
 * @return 像素宽度
 */
uint32_t ui_text_width(const char *text, uint32_t font);

/**
 * @brief 初始化一块文字图像（分配缓冲并创建图像控件）
 * @param t      文字图像结构
 * @param parent 父对象
 * @param w      缓冲宽度（像素）
 * @param h      缓冲高度（像素）
 * @param x      左上角 X
 * @param y      左上角 Y
 */
void ui_text_init(ui_text_t *t, lv_obj_t *parent, uint32_t w, uint32_t h,
                  lv_coord_t x, lv_coord_t y);

/**
 * @brief 更新文字图像内容（文字在缓冲内水平、垂直居中）
 * @param t     文字图像结构
 * @param text  文字（UTF-8），NULL 或空串表示清空
 * @param color 文字颜色（标准 RGB565）
 * @param bg    背景色（标准 RGB565），需与所在控件底色一致，避免出现黑框
 */
void ui_text_set(ui_text_t *t, const char *text, uint16_t color, uint16_t bg);

/**
 * @brief 更新文字图像内容并放大显示（16 号字模最近邻放大，用作大号标题）
 * @param t     文字图像结构，缓冲宽高应为 scale 的整数倍
 * @param text  文字（UTF-8），NULL 或空串表示清空
 * @param color 文字颜色（标准 RGB565）
 * @param bg    背景色（标准 RGB565），需与所在控件底色一致
 * @param scale 放大倍数（2 表示 16 号字显示为 32 像素高）
 * @note 32 号字库只有少量汉字，标题类文字用本函数放大 16 号字模实现
 */
void ui_text_set_scaled(ui_text_t *t, const char *text, uint16_t color,
                        uint16_t bg, uint8_t scale);

/**
 * @brief 把两行文字渲染进图像缓冲（左对齐，黑底白字）
 * @param buf   RGB565 缓冲（PSRAM）
 * @param w     缓冲宽度（像素）
 * @param h     缓冲高度（像素）
 * @param line1 第一行文字（UTF-8，可为 NULL）
 * @param line2 第二行文字（UTF-8，可为 NULL）
 */
void ui_render_lines(uint16_t *buf, uint32_t w, uint32_t h,
                     const char *line1, const char *line2);

/**
 * @brief 创建一个纯色圆角按钮（带按下态高亮）
 * @param parent 父对象
 * @param x      左上角 X
 * @param y      左上角 Y
 * @param w      宽度
 * @param h      高度
 * @param bg     常态背景色（如 0x303030）
 * @param cb     点击回调（可为 NULL）
 * @param user   回调用户数据（按键编码等）
 * @return 按钮对象
 */
lv_obj_t *ui_btn_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, uint32_t bg,
                        lv_event_cb_t cb, void *user);

/**
 * @brief 判断对象是否为 ui_btn_create 创建的按钮
 * @param obj 对象（可为 NULL）
 * @return true 表示是按钮
 * @note  用于命中测试诊断：按压落在按钮之外时可直接识别出来
 */
bool ui_obj_is_btn(const void *obj);

/**
 * @brief 创建一个整屏遮罩（半透明，可拦截触摸）
 * @param parent 父对象
 * @param opa    不透明度（0~255）
 * @param cb     点击回调（可为 NULL）
 * @return 遮罩对象
 */
lv_obj_t *ui_mask_create(lv_obj_t *parent, lv_opa_t opa, lv_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
