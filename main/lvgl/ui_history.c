/**
 * @file    ui_history.c
 * @brief   识别历史记录界面实现：3 行 3 列人脸照片 + 点击放大 + 中心下方“返回”
 *
 * @note    缩略图 84×56、放大图 360×240（屏幕 3/4），都由识别模块保存的
 *          180×120 原始照片最近邻缩放而来，不依赖 LVGL 的图像缩放功能。
 *          大图查看层是一个整屏半透明遮罩，点击遮罩任意位置即退出查看。
 */

#include "ui_history.h"

#include <stdbool.h>
#include <stdint.h>

#include "lcd.h"
#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_admin.h"
#include "ui_common.h"
#include "who_human_face_recognition.hpp"

/* ==================== 配色（0xRRGGBB） ==================== */
#define COL_BG   0x0E1116           /* 与登入页/管理员页一致的深色背景 */
#define COL_TEXT 0xE6EDF3           /* 主文字 */
#define COL_DIM  0x8B949E           /* 次要文字 */
#define COL_CARD 0x18202B           /* 次要按钮（返回） */

/* ==================== 布局参数（横屏 480x320） ==================== */
#define HIST_TITLE_W     128        /* “历史记录”4 字 × 16px × 2 倍 */
#define HIST_TITLE_H     32
#define HIST_TITLE_SCALE 2
#define HIST_TITLE_Y     6

#define HIST_COLS 3                 /* 每行 3 张 */
#define HIST_ROWS 3                 /* 共 3 行 */
#define HIST_MAX  (HIST_COLS * HIST_ROWS)

#define HIST_THUMB_W    84          /* 缩略图宽度（与保存照片同为 3:2） */
#define HIST_THUMB_H    56          /* 缩略图高度 */
#define HIST_THUMB_GAP  (HIST_THUMB_W / 3)   /* 列间距 = 图片宽度的 1/3（28） */
#define HIST_ROW_GAP    (HIST_THUMB_H / 3)   /* 行间距 = 图片高度的 1/3（18） */
#define HIST_THUMB_DX   (HIST_THUMB_W + HIST_THUMB_GAP)   /* 112 */
#define HIST_THUMB_DY   (HIST_THUMB_H + HIST_ROW_GAP)     /* 74 */
#define HIST_THUMB_X0   ((LCD_WIDTH - (HIST_COLS * HIST_THUMB_W + \
                                      (HIST_COLS - 1) * HIST_THUMB_GAP)) / 2) /* 86 */
#define HIST_THUMB_Y0   48

#define HIST_FULL_W 360             /* 放大图宽度：屏幕 3/4（480 × 3/4） */
#define HIST_FULL_H 240             /* 放大图高度：屏幕 3/4（320 × 3/4） */
#define HIST_FULL_X ((LCD_WIDTH - HIST_FULL_W) / 2)   /* 60 */
#define HIST_FULL_Y ((LCD_HEIGHT - HIST_FULL_H) / 2)  /* 40 */

#define HIST_BTN_W 120
#define HIST_BTN_H 44
#define HIST_BTN_X ((LCD_WIDTH - HIST_BTN_W) / 2)     /* 180：水平居中 */
#define HIST_BTN_Y 262                                /* 屏幕中心下方，底部留 14px 不触底 */

#define HIST_EMPTY_W 160
#define HIST_EMPTY_H UI_INFO_FONT
#define HIST_EMPTY_X ((LCD_WIDTH - HIST_EMPTY_W) / 2)
#define HIST_EMPTY_Y 142

/* ==================== 内部对象 ==================== */
static lv_obj_t *s_hist_page = NULL;        /* 历史记录页（整屏覆盖） */
static ui_text_t s_hist_title;              /* 标题文字（放大显示） */
static ui_text_t s_hist_empty_txt;          /* “暂无历史记录”提示 */
static ui_text_t s_hist_back_txt;           /* “返回”按钮文字 */

static lv_obj_t *s_thumb_img[HIST_MAX];             /* 缩略图对象 */
static lv_img_dsc_t s_thumb_dsc[HIST_MAX];          /* 缩略图描述 */
static uint16_t *s_thumb_buf[HIST_MAX];             /* 缩略图像素缓冲（PSRAM） */

static lv_obj_t *s_view_mask = NULL;        /* 大图查看遮罩（整屏，点击退出） */
static lv_obj_t *s_full_img = NULL;         /* 放大显示的照片 */
static lv_img_dsc_t s_full_dsc;
static uint16_t *s_full_buf = NULL;         /* 放大图像素缓冲（PSRAM） */

/* ==================== 页面显示/隐藏 ==================== */

/**
 * @brief 关闭大图查看
 */
static void hide_view(void)
{
    if (s_view_mask != NULL)
    {
        lv_obj_add_flag(s_view_mask, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 大图查看层被点击：任意位置点击都退出查看
 */
static void on_view_clicked(lv_event_t *e)
{
    (void)e;
    hide_view();
}

/**
 * @brief 缩略图被点击：按屏幕 3/4 尺寸在中心放大显示
 */
static void on_thumb_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_full_img == NULL || s_view_mask == NULL)
    {
        return;
    }

    if (human_face_history_image(index, s_full_buf, HIST_FULL_W, HIST_FULL_H) != 1)
    {
        return;
    }

    lv_obj_invalidate(s_full_img);
    lv_obj_clear_flag(s_view_mask, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief “返回”按钮：回到管理员配置界面
 */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    ui_history_hide();
    ui_admin_show();
}

/* ==================== 历史照片刷新 ==================== */

/**
 * @brief 拉取历史照片并刷新 2 行 5 列缩略图（索引 0 为最新一条）
 */
static void refresh_grid(void)
{
    int count = human_face_history_count();
    int i;

    if (s_hist_empty_txt.img != NULL)
    {
        if (count == 0)
        {
            lv_obj_clear_flag(s_hist_empty_txt.img, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_hist_empty_txt.img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (i = 0; i < HIST_MAX; i++)
    {
        if (s_thumb_img[i] == NULL)
        {
            continue;
        }

        if (i < count &&
            human_face_history_image(i, s_thumb_buf[i], HIST_THUMB_W, HIST_THUMB_H) == 1)
        {
            lv_obj_clear_flag(s_thumb_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_thumb_img[i]);
        }
        else
        {
            lv_obj_add_flag(s_thumb_img[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ==================== 界面创建 ==================== */

void ui_history_create(lv_obj_t *scr)
{
    int i;

    s_hist_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_hist_page);
    lv_obj_set_size(s_hist_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_hist_page, 0, 0);
    lv_obj_set_style_bg_color(s_hist_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_hist_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_hist_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hist_page, LV_OBJ_FLAG_HIDDEN);

    /* 顶部标题：32 号字库汉字太少，用 16 号字模放大 2 倍 */
    ui_text_init(&s_hist_title, s_hist_page, HIST_TITLE_W, HIST_TITLE_H,
                 (LCD_WIDTH - HIST_TITLE_W) / 2, HIST_TITLE_Y);
    ui_text_set_scaled(&s_hist_title, "历史记录", UI_RGB565(COL_TEXT),
                       UI_RGB565(COL_BG), HIST_TITLE_SCALE);

    /* 标题下方强调条 */
    {
        lv_obj_t *line = lv_obj_create(s_hist_page);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 72, 4);
        lv_obj_set_pos(line, (LCD_WIDTH - 72) / 2, 50);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x2F81F7), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 暂无记录提示 */
    ui_text_init(&s_hist_empty_txt, s_hist_page, HIST_EMPTY_W, HIST_EMPTY_H,
                 HIST_EMPTY_X, HIST_EMPTY_Y);
    ui_text_set(&s_hist_empty_txt, "暂无历史记录", UI_RGB565(COL_DIM),
                UI_RGB565(COL_BG));
    if (s_hist_empty_txt.img != NULL)
    {
        lv_obj_add_flag(s_hist_empty_txt.img, LV_OBJ_FLAG_HIDDEN);
    }

    /* 2 行 5 列缩略图（索引 0 为最新，从左到右、从上到下排列） */
    for (i = 0; i < HIST_MAX; i++)
    {
        int col = i % HIST_COLS;
        int row = i / HIST_COLS;

        s_thumb_buf[i] = (uint16_t *)ui_alloc(HIST_THUMB_W * HIST_THUMB_H *
                                              sizeof(uint16_t));
        if (s_thumb_buf[i] == NULL)
        {
            s_thumb_img[i] = NULL;
            continue;
        }

        s_thumb_img[i] = ui_image_create(s_hist_page, &s_thumb_dsc[i],
                                         s_thumb_buf[i], HIST_THUMB_W, HIST_THUMB_H,
                                         HIST_THUMB_X0 + col * HIST_THUMB_DX,
                                         HIST_THUMB_Y0 + row * HIST_THUMB_DY);

        /* 图像默认不拦截触摸，这里需要它可点击以便放大查看 */
        lv_obj_add_flag(s_thumb_img[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_thumb_img[i], on_thumb_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_flag(s_thumb_img[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 中心下方：返回按钮 */
    {
        lv_obj_t *back_btn = ui_btn_create(s_hist_page, HIST_BTN_X, HIST_BTN_Y,
                                           HIST_BTN_W, HIST_BTN_H,
                                           COL_CARD, on_back_clicked, NULL);
        ui_text_init(&s_hist_back_txt, back_btn, HIST_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_hist_back_txt.img != NULL)
        {
            lv_obj_center(s_hist_back_txt.img);
        }
        ui_text_set(&s_hist_back_txt, "返回", UI_COLOR_WHITE, UI_RGB565(COL_CARD));
    }

    /* 大图查看层：最后创建，覆盖在缩略图之上（默认隐藏） */
    s_full_buf = (uint16_t *)ui_alloc(HIST_FULL_W * HIST_FULL_H * sizeof(uint16_t));
    if (s_full_buf != NULL)
    {
        s_view_mask = ui_mask_create(s_hist_page, LV_OPA_70, on_view_clicked);
        s_full_img = ui_image_create(s_view_mask, &s_full_dsc, s_full_buf,
                                     HIST_FULL_W, HIST_FULL_H, HIST_FULL_X, HIST_FULL_Y);
        lv_obj_add_flag(s_view_mask, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_history_show(void)
{
    /* 每次进入都关闭大图并重新拉取历史照片 */
    hide_view();
    refresh_grid();

    if (s_hist_page != NULL)
    {
        lv_obj_clear_flag(s_hist_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_history_hide(void)
{
    hide_view();

    if (s_hist_page != NULL)
    {
        lv_obj_add_flag(s_hist_page, LV_OBJ_FLAG_HIDDEN);
    }
}
