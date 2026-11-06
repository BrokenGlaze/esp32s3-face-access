/**
 * @file    ui_face_db.c
 * @brief   人脸信息库界面：大标题 + 已录入人脸名单 + 右下角“返回”
 *
 * @note    名单由识别模块的 human_face_list_enrolled() 提供（线程安全），
 *          每次显示界面时重新拉取一次。
 */

#include "ui_face_db.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "lcd.h"
#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_common.h"
#include "ui_admin.h"
#include "who_human_face_recognition.hpp"

/* ==================== 配色（0xRRGGBB） ==================== */
#define COL_BG     0x0E1116         /* 与登入页/管理员页一致的深色背景 */
#define COL_TEXT   0xE6EDF3         /* 主文字 */
#define COL_DIM    0x8B949E         /* 次要文字 */
#define COL_CARD   0x18202B         /* 次要按钮（返回 / 清除 / 删除常态） */
#define COL_DANGER 0xE5534B         /* 删除模式红 / 危险操作 */
#define COL_BOX    0x3A4553         /* 勾选框边框 */

/* ==================== 布局参数（横屏 480x320） ==================== */
#define DB_TITLE_W     160          /* “人脸信息库”5 字 × 16px × 2 倍 */
#define DB_TITLE_H     32
#define DB_TITLE_SCALE 2

#define DB_COUNT_W 200              /* “共 N 人” */
#define DB_COUNT_H UI_INFO_FONT

#define DB_ROW_MAX 5                /* 一屏最多显示 5 条 */
#define DB_ROW_W   320              /* 名单文字宽度（右侧留出勾选框位置） */
#define DB_ROW_H   20
#define DB_ROW_Y0  92
#define DB_ROW_DY  34

#define DB_AVATAR_X    22           /* 头像左边缘 X */
#define DB_AVATAR_DY   6            /* 头像相对名单行上移的像素（32 高 vs 20 行高） */

#define DB_CHECK_W     24           /* 勾选框边长 */
#define DB_CHECK_X     430          /* 勾选框左边缘 X */
#define DB_CHECK_DY    2            /* 勾选框相对名单行上移的像素 */

#define DB_BTN_W       120
#define DB_BTN_H       48
#define DB_BTN_Y       (LCD_HEIGHT - DB_BTN_H - 14)   /* 258 */
#define DB_BTN_X1      14           /* “清除全部” */
#define DB_BTN_X2      150          /* “删除” */
#define DB_BTN_X3      346          /* “返回”（靠右） */
#define DB_BTN_GAP     16

/* 名单查询接口最大条目数：显示几条就查几条 */
#define DB_NAME_LEN 32

/* ==================== 内部对象 ==================== */
static lv_obj_t *s_db_page = NULL;          /* 人脸信息库页（整屏覆盖） */
static ui_text_t s_db_title;                /* 标题文字（放大显示） */
static ui_text_t s_db_count_txt;            /* “共 N 人” */
static ui_text_t s_db_rows[DB_ROW_MAX];     /* 名单行 */
static lv_obj_t *s_db_avatar_img[DB_ROW_MAX];      /* 头像图像 */
static lv_img_dsc_t s_db_avatar_dsc[DB_ROW_MAX];   /* 头像图像描述 */
static uint16_t *s_db_avatar_buf[DB_ROW_MAX];      /* 头像像素缓冲（PSRAM） */

static lv_obj_t *s_db_check[DB_ROW_MAX];    /* 勾选框（删除模式下显示） */
static bool s_db_checked[DB_ROW_MAX];       /* 勾选状态 */
static int s_db_ids[DB_ROW_MAX];            /* 当前显示行对应的人脸 ID */
static int s_db_count = 0;                  /* 当前显示行数 */

static bool s_db_delete_mode = false;       /* 是否处于删除选择模式 */
static lv_obj_t *s_db_del_btn = NULL;       /* “删除”按钮 */

static ui_text_t s_db_clear_txt;            /* “清除全部”按钮文字 */
static ui_text_t s_db_del_txt;              /* “删除”按钮文字 */
static ui_text_t s_db_back_txt;             /* “返回”按钮文字 */

/* ==================== 勾选框 ==================== */

/**
 * @brief 按勾选状态刷新某个勾选框的样式
 * @param i 行号
 */
static void apply_check_style(int i)
{
    lv_obj_t *c = s_db_check[i];

    if (c == NULL)
    {
        return;
    }

    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 4, 0);

    if (s_db_checked[i])
    {
        /* 选中：红色实心方块 */
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_DANGER), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(COL_DANGER), 0);
    }
    else
    {
        /* 未选中：空心方框 */
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(COL_BOX), 0);
    }
}

/**
 * @brief 勾选框点击：切换选中状态
 */
static void on_check_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if (!s_db_delete_mode || i < 0 || i >= s_db_count || i >= DB_ROW_MAX)
    {
        return;
    }

    s_db_checked[i] = !s_db_checked[i];
    apply_check_style(i);
}

/**
 * @brief 进入/退出删除选择模式
 *
 * 进入：显示勾选框、“删除”按钮变红；
 * 退出：隐藏勾选框并清空勾选、“删除”按钮恢复常态颜色。
 *
 * @param on true 进入删除模式
 */
static void set_delete_mode(bool on)
{
    int i;

    s_db_delete_mode = on;

    for (i = 0; i < DB_ROW_MAX; i++)
    {
        s_db_checked[i] = false;
        apply_check_style(i);

        if (s_db_check[i] == NULL)
        {
            continue;
        }

        if (on && i < s_db_count)
        {
            lv_obj_clear_flag(s_db_check[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_db_check[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_db_del_btn != NULL)
    {
        lv_obj_set_style_bg_color(s_db_del_btn,
                                  lv_color_hex(on ? COL_DANGER : COL_CARD), 0);
    }
    ui_text_set(&s_db_del_txt, "删除", UI_COLOR_WHITE,
                UI_RGB565(on ? COL_DANGER : COL_CARD));
}

/* ==================== 名单刷新 ==================== */

/**
 * @brief 拉取已录入人脸名单并刷新列表（无数据时显示一行提示）
 */
static void refresh_list(void)
{
    char names[DB_ROW_MAX][DB_NAME_LEN];
    int ids[DB_ROW_MAX];
    char count[32];
    int total = 0;
    int n;
    int i;

    n = human_face_list_enrolled(ids, names, DB_ROW_MAX, &total);

    s_db_count = n;
    for (i = 0; i < DB_ROW_MAX; i++)
    {
        s_db_ids[i] = (i < n) ? ids[i] : -1;

        /* 行数据变化后勾选状态一律复位（删除后行会整体上移） */
        s_db_checked[i] = false;
        apply_check_style(i);
        if (s_db_check[i] != NULL)
        {
            if (s_db_delete_mode && i < n)
            {
                lv_obj_clear_flag(s_db_check[i], LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(s_db_check[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    snprintf(count, sizeof(count), "共 %d 人", total);
    ui_text_set(&s_db_count_txt, count, UI_RGB565(COL_DIM), UI_RGB565(COL_BG));

    for (i = 0; i < DB_ROW_MAX; i++)
    {
        if (s_db_rows[i].img == NULL)
        {
            continue;
        }

        if (i < n)
        {
            ui_text_set(&s_db_rows[i], names[i], UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));
            lv_obj_clear_flag(s_db_rows[i].img, LV_OBJ_FLAG_HIDDEN);

            /* 读取该人脸的头像；文件缺失时隐藏头像，只显示名字 */
            if (s_db_avatar_img[i] != NULL &&
                human_face_avatar_load(ids[i], s_db_avatar_buf[i]) == 1)
            {
                lv_obj_clear_flag(s_db_avatar_img[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_invalidate(s_db_avatar_img[i]);
            }
            else if (s_db_avatar_img[i] != NULL)
            {
                lv_obj_add_flag(s_db_avatar_img[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        else if (i == 0 && total == 0)
        {
            ui_text_set(&s_db_rows[0], "暂无已录入人脸", UI_RGB565(COL_DIM), UI_RGB565(COL_BG));
            lv_obj_clear_flag(s_db_rows[0].img, LV_OBJ_FLAG_HIDDEN);
            if (s_db_avatar_img[0] != NULL)
            {
                lv_obj_add_flag(s_db_avatar_img[0], LV_OBJ_FLAG_HIDDEN);
            }
        }
        else
        {
            lv_obj_add_flag(s_db_rows[i].img, LV_OBJ_FLAG_HIDDEN);
            if (s_db_avatar_img[i] != NULL)
            {
                lv_obj_add_flag(s_db_avatar_img[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ==================== 事件回调 ==================== */

/**
 * @brief “返回”按钮：回到管理员配置界面
 */
static void on_back_clicked(lv_event_t *e)
{
    set_delete_mode(false);
    ui_face_db_hide();
    ui_admin_show();
}

/**
 * @brief “清除全部”按钮：移除所有人脸特征与头像，并刷新名单
 */
static void on_clear_clicked(lv_event_t *e)
{
    (void)e;

    set_delete_mode(false);
    human_face_db_clear();
    refresh_list();
}

/**
 * @brief “删除”按钮：两段式操作
 *
 * 常态点击：进入删除选择模式（按钮变红，各行最右侧出现勾选框）；
 * 删除模式点击：移除所有被勾选的人脸，随后退出模式并恢复按钮颜色。
 */
static void on_delete_clicked(lv_event_t *e)
{
    int i;

    (void)e;

    if (!s_db_delete_mode)
    {
        if (s_db_count > 0)
        {
            set_delete_mode(true);
        }
        return;
    }

    for (i = 0; i < s_db_count && i < DB_ROW_MAX; i++)
    {
        if (s_db_checked[i] && s_db_ids[i] >= 0)
        {
            human_face_db_delete(s_db_ids[i]);
        }
    }

    set_delete_mode(false);
    refresh_list();
}

/* ==================== 界面创建 ==================== */

void ui_face_db_create(lv_obj_t *scr)
{
    int i;

    s_db_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_db_page);
    lv_obj_set_size(s_db_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_db_page, 0, 0);
    lv_obj_set_style_bg_color(s_db_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_db_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_db_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_db_page, LV_OBJ_FLAG_HIDDEN);

    /* 顶部标题：32 号字库汉字太少，用 16 号字模放大 2 倍 */
    ui_text_init(&s_db_title, s_db_page, DB_TITLE_W, DB_TITLE_H,
                 (LCD_WIDTH - DB_TITLE_W) / 2, 16);
    ui_text_set_scaled(&s_db_title, "人脸信息库", UI_RGB565(COL_TEXT),
                       UI_RGB565(COL_BG), DB_TITLE_SCALE);

    /* 标题下方强调条 */
    {
        lv_obj_t *line = lv_obj_create(s_db_page);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 72, 4);
        lv_obj_set_pos(line, (LCD_WIDTH - 72) / 2, 56);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x2F81F7), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 总人数 */
    ui_text_init(&s_db_count_txt, s_db_page, DB_COUNT_W, DB_COUNT_H,
                 (LCD_WIDTH - DB_COUNT_W) / 2, 66);

    /* 名单行 + 头像（头像占用行左侧 48 像素，名字相应右移） */
    for (i = 0; i < DB_ROW_MAX; i++)
    {
        int row_y = DB_ROW_Y0 + i * DB_ROW_DY;

        ui_text_init(&s_db_rows[i], s_db_page, DB_ROW_W, DB_ROW_H,
                     (LCD_WIDTH - DB_ROW_W) / 2 + FACE_AVATAR_W, row_y);

        /* 每行独立的图像描述与 PSRAM 像素缓冲，lv_img_set_src 只保存指针 */
        s_db_avatar_buf[i] = ui_alloc(FACE_AVATAR_PIXELS * sizeof(uint16_t));

        s_db_avatar_img[i] = ui_image_create(s_db_page, &s_db_avatar_dsc[i],
                                             s_db_avatar_buf[i],
                                             FACE_AVATAR_W, FACE_AVATAR_H,
                                             DB_AVATAR_X, row_y - DB_AVATAR_DY);
        lv_obj_add_flag(s_db_avatar_img[i], LV_OBJ_FLAG_HIDDEN);

        /* 行最右侧勾选框（仅删除模式下显示） */
        s_db_check[i] = lv_obj_create(s_db_page);
        lv_obj_remove_style_all(s_db_check[i]);
        lv_obj_set_size(s_db_check[i], DB_CHECK_W, DB_CHECK_W);
        lv_obj_set_pos(s_db_check[i], DB_CHECK_X, row_y - DB_CHECK_DY);
        apply_check_style(i);
        lv_obj_clear_flag(s_db_check[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_db_check[i], on_check_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_flag(s_db_check[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 底部按钮：清除全部 / 删除（左侧），返回（右侧） */
    {
        lv_obj_t *clear_btn = ui_btn_create(s_db_page, DB_BTN_X1, DB_BTN_Y,
                                            DB_BTN_W, DB_BTN_H,
                                            COL_CARD, on_clear_clicked, NULL);
        ui_text_init(&s_db_clear_txt, clear_btn, DB_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_db_clear_txt.img != NULL)
        {
            lv_obj_center(s_db_clear_txt.img);
        }
        ui_text_set(&s_db_clear_txt, "清除全部", UI_COLOR_WHITE, UI_RGB565(COL_CARD));

        s_db_del_btn = ui_btn_create(s_db_page, DB_BTN_X2, DB_BTN_Y,
                                     DB_BTN_W, DB_BTN_H,
                                     COL_CARD, on_delete_clicked, NULL);
        ui_text_init(&s_db_del_txt, s_db_del_btn, DB_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_db_del_txt.img != NULL)
        {
            lv_obj_center(s_db_del_txt.img);
        }
        ui_text_set(&s_db_del_txt, "删除", UI_COLOR_WHITE, UI_RGB565(COL_CARD));

        lv_obj_t *back_btn = ui_btn_create(s_db_page, DB_BTN_X3, DB_BTN_Y,
                                           DB_BTN_W, DB_BTN_H,
                                           COL_CARD, on_back_clicked, NULL);
        ui_text_init(&s_db_back_txt, back_btn, DB_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_db_back_txt.img != NULL)
        {
            lv_obj_center(s_db_back_txt.img);
        }
        ui_text_set(&s_db_back_txt, "返回", UI_COLOR_WHITE, UI_RGB565(COL_CARD));
    }
}

void ui_face_db_show(void)
{
    /* 每次进入都从非删除模式开始 */
    set_delete_mode(false);
    refresh_list();

    if (s_db_page != NULL)
    {
        lv_obj_clear_flag(s_db_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_face_db_hide(void)
{
    if (s_db_page != NULL)
    {
        lv_obj_add_flag(s_db_page, LV_OBJ_FLAG_HIDDEN);
    }
}
