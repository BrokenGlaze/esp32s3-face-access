/**
 * @file    ui_admin.c
 * @brief   管理员配置界面：大标题 + “录入人脸”“已录入人脸”“历史记录”“网络设置”“系统设置”按钮 + 右下角“返回”
 *
 * @note    另有两个子页面（均默认隐藏）：
 *            - 系统设置：语音播报开关、音量调节、模板自学习开关、进入“修改密码”
 *            - 修改密码：数字键盘连续输入两次新密码，两次一致才生效
 *          子页面与主页面是同级整屏对象，切换时只改 LV_OBJ_FLAG_HIDDEN。
 *          语音开关/音量/新密码均写入 NVS，掉电保存。
 */

#include "ui_admin.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_prompt.h"
#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_common.h"
#include "ui_face_db.h"
#include "ui_history.h"
#include "ui_login.h"
#include "ui_wifi.h"
#include "who_human_face_recognition.hpp"

/* ==================== 配色（0xRRGGBB） ==================== */
#define COL_BG     0x0E1116         /* 与登入页一致的深色背景 */
#define COL_TEXT   0xE6EDF3         /* 主文字 */
#define COL_DIM    0x8B949E         /* 次要文字 */
#define COL_CARD   0x18202B         /* 次要按钮（返回） */
#define COL_TEAL   0x3F6A6A         /* 灰青色主按钮 */
#define COL_LINE   0x2F81F7         /* 标题下方强调条 */
#define COL_ACCENT 0x2F81F7         /* 密码掩码（蓝，与登入页一致） */
#define COL_OK     0x3FB950         /* 操作成功提示（绿） */
#define COL_ERR    0xF85149         /* 错误提示（红） */

/* ==================== 布局参数（横屏 480x320） ==================== */
#define ADMIN_TITLE_W     224       /* “管理员配置界面”7 字 × 16px × 2 倍 */
#define ADMIN_TITLE_H     32
#define ADMIN_TITLE_SCALE 2

#define ADMIN_BTN_W  170
#define ADMIN_BTN_H  56
#define ADMIN_BTN_Y  126            /* 两个主按钮 */
#define ADMIN_BTN_LX 50             /* “录入人脸”按钮 X */
#define ADMIN_BTN_RX 260            /* “已录入人脸”按钮 X */

#define ADMIN_SET_W  140
#define ADMIN_SET_H  44
#define ADMIN_SET_X  20             /* “系统设置” */
#define ADMIN_HIST_X 170            /* “历史记录” */
#define ADMIN_WIFI_X 320            /* “网络设置” */
#define ADMIN_SET_Y  194

#define ADMIN_BACK_W      120
#define ADMIN_BACK_H      48
#define ADMIN_BACK_MARGIN 14        /* 距屏幕右侧/底部的间距 */

/* ---- 顶部临时提示条 ---- */
#define ADMIN_TIP_W   300
#define ADMIN_TIP_H   44
#define ADMIN_TIP_X   ((LCD_WIDTH - ADMIN_TIP_W) / 2)
#define ADMIN_TIP_Y   76            /* 标题与主按钮之间的空白处 */
#define ADMIN_TIP_MS  3000          /* 提示显示时长 */
#define COL_WARN      0x8A5A00      /* 警告提示底色（橙黄） */

/* ---- 系统设置页 ---- */
#define SET_TITLE_W     128         /* “系统设置”4 字 × 16px × 2 倍 */
#define SET_TITLE_H     32
#define SET_TITLE_SCALE 2

#define SET_ROW1_Y 62               /* 语音播报 */
#define SET_ROW2_Y 104              /* 音量 */
#define SET_ROW3_Y 146              /* 相似度 */
#define SET_ROW4_Y 188              /* 模板自学习 */
#define SET_ROW5_Y 230              /* 修改密码 */
#define SET_LBL_X  30

#define SET_VOICE_BTN_X 300
#define SET_VOICE_BTN_W 150
#define SET_ROW_BTN_H   40

#define SET_VOL_BTN_W 60
#define SET_VOL_DEC_X 210
#define SET_VOL_VAL_X 276
#define SET_VOL_VAL_W 84
#define SET_VOL_INC_X 364

#define SET_PWD_BTN_W 160

/* ---- 修改密码页（左侧提示 + 右侧 3x4 数字键盘） ---- */
#define PWD_KEY_W  90
#define PWD_KEY_H  56
#define PWD_KEY_X0 180              /* 键盘第 1 列 X */
#define PWD_KEY_Y0 56               /* 键盘第 1 行 Y */
#define PWD_KEY_DX 98               /* 列间距 */
#define PWD_KEY_DY 64               /* 行间距 */

/* 键盘按键编码：0~9 为数字，其余为功能键 */
#define PWD_KEY_DEL   10            /* 删除 */
#define PWD_KEY_OK    11            /* 确定 */
#define PWD_KEY_COUNT 12            /* 按键总数 */

/* ==================== 内部对象 ==================== */
static lv_obj_t *s_admin_page = NULL;       /* 管理员配置页（整屏覆盖） */
static ui_text_t s_admin_title;             /* 标题文字（放大显示） */
static ui_text_t s_admin_enroll_txt;        /* “录入人脸”按钮文字 */
static ui_text_t s_admin_facedb_txt;        /* “已录入人脸”按钮文字 */
static ui_text_t s_admin_history_txt;       /* “历史记录”按钮文字 */
static ui_text_t s_admin_wifi_txt;          /* “网络设置”按钮文字 */
static ui_text_t s_admin_set_txt;           /* “系统设置”按钮文字 */
static ui_text_t s_admin_back_txt;          /* “返回”按钮文字 */

static lv_obj_t *s_admin_tip = NULL;        /* 顶部临时提示条 */
static ui_text_t s_admin_tip_txt;           /* 提示文字 */
static lv_timer_t *s_admin_tip_timer = NULL;/* 提示自动消失定时器 */

static lv_obj_t *s_set_page = NULL;         /* 系统设置页（整屏覆盖） */
static lv_obj_t *s_set_voice_btn = NULL;    /* 语音播报开关按钮 */
static ui_text_t s_set_title;               /* “系统设置”标题 */
static ui_text_t s_set_voice_lbl;           /* “语音播报”标签 */
static ui_text_t s_set_voice_txt;           /* 开关按钮文字（已开启/已关闭） */
static ui_text_t s_set_vol_lbl;             /* “音量”标签 */
static ui_text_t s_set_vol_val;             /* 当前音量百分比 */
static ui_text_t s_set_vol_dec_txt;         /* 音量“-”按钮文字 */
static ui_text_t s_set_vol_inc_txt;         /* 音量“+”按钮文字 */
static ui_text_t s_set_face_lbl;            /* “相似度”标签 */
static ui_text_t s_set_face_val;            /* 当前相似度百分比 */
static ui_text_t s_set_face_dec_txt;        /* 相似度“-”按钮文字 */
static ui_text_t s_set_face_inc_txt;        /* 相似度“+”按钮文字 */
static lv_obj_t *s_set_learn_btn = NULL;    /* 模板自学习开关按钮 */
static ui_text_t s_set_learn_lbl;           /* “模板自学习”标签 */
static ui_text_t s_set_learn_txt;           /* 开关按钮文字（已开启/已关闭） */
static ui_text_t s_set_pwd_txt;             /* “修改密码”按钮文字 */
static ui_text_t s_set_back_txt;            /* 设置页“返回”按钮文字 */

static lv_obj_t *s_pwd_page = NULL;         /* 修改密码页（整屏覆盖） */
static ui_text_t s_pwd_title;               /* “修改密码”标题 */
static ui_text_t s_pwd_step_txt;            /* “请输入新密码/请再次输入新密码” */
static ui_text_t s_pwd_mask_txt;            /* 密码掩码 */
static ui_text_t s_pwd_msg_txt;             /* 提示信息 */
static ui_text_t s_pwd_back_txt;            /* 密码页“返回”按钮文字 */
static ui_text_t s_pwd_key_txt[PWD_KEY_COUNT]; /* 键盘按键文字 */

static char s_pwd_input[UI_LOGIN_PWD_LEN + 1]; /* 当前正在输入的密码 */
static char s_pwd_first[UI_LOGIN_PWD_LEN + 1]; /* 第一次输入的密码 */
static uint8_t s_pwd_len = 0;                  /* 当前已输入位数 */
static uint8_t s_pwd_step = 1;                 /* 1=输入新密码，2=再次输入确认 */

static void (*s_enroll_cb)(void) = NULL;    /* 录入按钮点击回调 */

/* 管理员页面（含系统设置/修改密码子页）是否正在显示（业务侧据此判断是否允许
   人脸录入，并避免被登入页覆盖）。只读写一个 bool，同 LVGL 对象无关，
   可在任意任务中安全访问 */
static volatile bool s_admin_visible = false;

/* ==================== 页面显示/隐藏 ==================== */

static void show_settings_page(void);
static void show_password_page(void);

static void hide_admin_page(void)
{
    if (s_admin_page != NULL)
    {
        lv_obj_add_flag(s_admin_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_settings_page(void)
{
    if (s_set_page != NULL)
    {
        lv_obj_add_flag(s_set_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_password_page(void)
{
    if (s_pwd_page != NULL)
    {
        lv_obj_add_flag(s_pwd_page, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ==================== 语音播报设置 ==================== */

/**
 * @brief 按当前音量/开关状态刷新设置页控件
 */
static void refresh_voice_ui(void)
{
    bool on = audio_prompt_is_enabled();
    char vol_text[16];
    uint32_t btn_color = on ? COL_TEAL : COL_CARD;

    if (s_set_voice_btn != NULL)
    {
        lv_obj_set_style_bg_color(s_set_voice_btn, lv_color_hex(btn_color), 0);
    }
    ui_text_set(&s_set_voice_txt, on ? "已开启" : "已关闭", UI_COLOR_WHITE,
                UI_RGB565(btn_color));

    snprintf(vol_text, sizeof(vol_text), "%d%%", (int)audio_prompt_get_volume());
    ui_text_set(&s_set_vol_val, vol_text, UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));

    snprintf(vol_text, sizeof(vol_text), "%d%%", human_face_get_threshold());
    ui_text_set(&s_set_face_val, vol_text, UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));

    /* 模板自学习开关 */
    {
        bool learn_on = human_face_get_self_learn() != 0;
        uint32_t learn_color = learn_on ? COL_TEAL : COL_CARD;

        if (s_set_learn_btn != NULL)
        {
            lv_obj_set_style_bg_color(s_set_learn_btn, lv_color_hex(learn_color), 0);
        }
        ui_text_set(&s_set_learn_txt, learn_on ? "已开启" : "已关闭",
                    UI_COLOR_WHITE, UI_RGB565(learn_color));
    }
}

/**
 * @brief 语音播报开关：在“开启/关闭”之间切换（同时写入 NVS）
 */
static void on_voice_toggle_clicked(lv_event_t *e)
{
    (void)e;

    audio_prompt_set_enabled(!audio_prompt_is_enabled());
    refresh_voice_ui();
}

/**
 * @brief 模板自学习开关：在“开启/关闭”之间切换（同时写入 NVS）
 */
static void on_learn_toggle_clicked(lv_event_t *e)
{
    (void)e;

    human_face_set_self_learn(!human_face_get_self_learn());
    refresh_voice_ui();
}

/**
 * @brief 音量减：每次下调 10%，最低 10%（同时写入 NVS）
 */
static void on_volume_down_clicked(lv_event_t *e)
{
    int vol = (int)audio_prompt_get_volume();

    (void)e;

    vol -= 10;
    if (vol < 10)
    {
        vol = 10;
    }
    audio_prompt_set_volume((uint8_t)vol);
    refresh_voice_ui();
}

/**
 * @brief 音量加：每次上调 10%，最高 100%（同时写入 NVS）
 */
static void on_volume_up_clicked(lv_event_t *e)
{
    int vol = (int)audio_prompt_get_volume();

    (void)e;

    vol += 10;
    if (vol > 100)
    {
        vol = 100;
    }
    audio_prompt_set_volume((uint8_t)vol);
    refresh_voice_ui();
}

/**
 * @brief 相似度减：每次下调 5%，最低 50%（同时写入 NVS）
 */
static void on_thresh_down_clicked(lv_event_t *e)
{
    int thresh = human_face_get_threshold();

    (void)e;

    human_face_set_threshold(thresh - 5);
    refresh_voice_ui();
}

/**
 * @brief 相似度加：每次上调 5%，最高 95%（同时写入 NVS）
 */
static void on_thresh_up_clicked(lv_event_t *e)
{
    int thresh = human_face_get_threshold();

    (void)e;

    human_face_set_threshold(thresh + 5);
    refresh_voice_ui();
}

/* ==================== 修改密码 ==================== */

/**
 * @brief 刷新密码掩码：已输入显示 *，未输入显示 _
 */
static void pwd_update_display(void)
{
    char mask[UI_LOGIN_PWD_LEN + 1];
    uint8_t i;

    for (i = 0; i < s_pwd_len; i++)
    {
        mask[i] = '*';
    }
    for (; i < UI_LOGIN_PWD_LEN; i++)
    {
        mask[i] = '_';
    }
    mask[UI_LOGIN_PWD_LEN] = '\0';

    ui_text_set(&s_pwd_mask_txt, mask, UI_RGB565(COL_ACCENT), UI_RGB565(COL_BG));
}

/**
 * @brief 复位密码输入状态并回到“输入新密码”这一步
 */
static void pwd_reset(void)
{
    s_pwd_step = 1;
    s_pwd_len = 0;
    s_pwd_input[0] = '\0';
    s_pwd_first[0] = '\0';

    pwd_update_display();
    ui_text_set(&s_pwd_step_txt, "请输入新密码", UI_RGB565(COL_TEXT),
                UI_RGB565(COL_BG));
    ui_text_set(&s_pwd_msg_txt, "", UI_RGB565(COL_ERR), UI_RGB565(COL_BG));
}

/**
 * @brief 键盘按键处理：数字追加、删除、确定
 */
static void on_pwd_key_clicked(lv_event_t *e)
{
    const int code = (int)(lv_uintptr_t)lv_event_get_user_data(e);

    if (code >= 0 && code <= 9)
    {
        /* 数字键：最多输入 6 位，输入后清除上一次的提示 */
        if (s_pwd_len < UI_LOGIN_PWD_LEN)
        {
            s_pwd_input[s_pwd_len++] = (char)('0' + code);
            s_pwd_input[s_pwd_len] = '\0';
            pwd_update_display();
            ui_text_set(&s_pwd_msg_txt, "", UI_RGB565(COL_ERR), UI_RGB565(COL_BG));
        }
        return;
    }

    switch (code)
    {
    case PWD_KEY_DEL:
        if (s_pwd_len > 0)
        {
            s_pwd_input[--s_pwd_len] = '\0';
            pwd_update_display();
        }
        break;

    case PWD_KEY_OK:
        if (s_pwd_len != UI_LOGIN_PWD_LEN)
        {
            ui_text_set(&s_pwd_msg_txt, "请输入6位密码", UI_RGB565(COL_ERR),
                        UI_RGB565(COL_BG));
            break;
        }

        if (s_pwd_step == 1)
        {
            /* 第一次输入：暂存，等待再次输入确认 */
            memcpy(s_pwd_first, s_pwd_input, sizeof(s_pwd_first));
            s_pwd_step = 2;
            s_pwd_len = 0;
            s_pwd_input[0] = '\0';
            pwd_update_display();
            ui_text_set(&s_pwd_step_txt, "请再次输入新密码", UI_RGB565(COL_TEXT),
                        UI_RGB565(COL_BG));
            ui_text_set(&s_pwd_msg_txt, "", UI_RGB565(COL_ERR), UI_RGB565(COL_BG));
        }
        else if (strcmp(s_pwd_input, s_pwd_first) == 0)
        {
            /* 两次一致：写入新密码（内部同时存 NVS），回到可再次修改的状态 */
            ui_login_set_password(s_pwd_first);
            s_pwd_step = 1;
            s_pwd_len = 0;
            s_pwd_input[0] = '\0';
            s_pwd_first[0] = '\0';
            pwd_update_display();
            ui_text_set(&s_pwd_step_txt, "请输入新密码", UI_RGB565(COL_TEXT),
                        UI_RGB565(COL_BG));
            ui_text_set(&s_pwd_msg_txt, "密码修改成功", UI_RGB565(COL_OK),
                        UI_RGB565(COL_BG));
        }
        else
        {
            /* 两次不一致：清空重来 */
            s_pwd_step = 1;
            s_pwd_len = 0;
            s_pwd_input[0] = '\0';
            s_pwd_first[0] = '\0';
            pwd_update_display();
            ui_text_set(&s_pwd_step_txt, "请输入新密码", UI_RGB565(COL_TEXT),
                        UI_RGB565(COL_BG));
            ui_text_set(&s_pwd_msg_txt, "两次输入不一致", UI_RGB565(COL_ERR),
                        UI_RGB565(COL_BG));
        }
        break;

    default:
        break;
    }
}

/* ==================== 页面切换事件 ==================== */

/**
 * @brief 管理员主页“返回”：回到登入界面
 */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    ui_admin_hide();
    ui_login_show();
}

/**
 * @brief “录入人脸”：转发给业务侧（跳转摄像头画面并开始录入）
 */
static void on_enroll_clicked(lv_event_t *e)
{
    (void)e;
    if (s_enroll_cb != NULL)
    {
        s_enroll_cb();
    }
}

/**
 * @brief “已录入人脸”：进入人脸信息库界面
 */
static void on_facedb_clicked(lv_event_t *e)
{
    (void)e;
    ui_admin_hide();
    ui_face_db_show();
}

/**
 * @brief “历史记录”：进入识别历史记录界面
 */
static void on_history_clicked(lv_event_t *e)
{
    (void)e;
    ui_admin_hide();
    ui_history_show();
}

/**
 * @brief “网络设置”：进入 Wi-Fi 网络设置界面
 */
static void on_wifi_clicked(lv_event_t *e)
{
    (void)e;
    ui_admin_hide();
    ui_wifi_show();
}

/**
 * @brief “系统设置”：进入系统设置页
 */
static void on_settings_clicked(lv_event_t *e)
{
    (void)e;
    /* 系统设置/修改密码仍属于管理员界面，保持 s_admin_visible=true：
       否则业务侧会误判为“登入页”，把整屏登入页盖到设置页上 */
    hide_admin_page();
    show_settings_page();
}

/**
 * @brief 系统设置页“返回”：回到管理员主页
 */
static void on_settings_back_clicked(lv_event_t *e)
{
    (void)e;
    hide_settings_page();
    ui_admin_show();
}

/**
 * @brief 系统设置页“修改密码”：进入密码录入页
 */
static void on_change_pwd_clicked(lv_event_t *e)
{
    (void)e;
    hide_settings_page();
    show_password_page();
}

/**
 * @brief 密码页“返回”：放弃本次修改，回到系统设置页
 */
static void on_pwd_back_clicked(lv_event_t *e)
{
    (void)e;
    hide_password_page();
    show_settings_page();
}

/* ==================== 界面创建辅助 ==================== */

/**
 * @brief 创建一个带居中文字的圆角按钮
 */
static void create_text_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, lv_coord_t h, const char *text,
                               uint32_t bg, ui_text_t *t, lv_event_cb_t cb)
{
    lv_obj_t *btn = ui_btn_create(parent, x, y, w, h, bg, cb, NULL);

    ui_text_init(t, btn, w, UI_INFO_FONT, 0, 0);
    if (t->img != NULL)
    {
        lv_obj_center(t->img);
    }
    ui_text_set(t, text, UI_COLOR_WHITE, UI_RGB565(bg));
}

/**
 * @brief 创建密码页的一个键盘按键
 */
static lv_obj_t *create_pwd_key(lv_obj_t *parent, const char *text, lv_coord_t x,
                                lv_coord_t y, uint32_t bg, int code)
{
    lv_obj_t *btn = ui_btn_create(parent, x, y, PWD_KEY_W, PWD_KEY_H, bg,
                                  on_pwd_key_clicked,
                                  (void *)(lv_uintptr_t)code);

    ui_text_init(&s_pwd_key_txt[code], btn, PWD_KEY_W, UI_INFO_FONT, 0, 0);
    if (s_pwd_key_txt[code].img != NULL)
    {
        lv_obj_center(s_pwd_key_txt[code].img);
    }
    ui_text_set(&s_pwd_key_txt[code], text, UI_COLOR_WHITE, UI_RGB565(bg));
    return btn;
}

/**
 * @brief 创建数字键盘：3x4（1~9、删除、0、确定）
 */
static void create_pwd_keypad(lv_obj_t *parent)
{
    static const char *digits[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
    int i;

    for (i = 0; i < 9; i++)
    {
        create_pwd_key(parent, digits[i],
                       PWD_KEY_X0 + (i % 3) * PWD_KEY_DX,
                       PWD_KEY_Y0 + (i / 3) * PWD_KEY_DY,
                       COL_CARD, i + 1);
    }

    create_pwd_key(parent, "删除", PWD_KEY_X0, PWD_KEY_Y0 + 3 * PWD_KEY_DY,
                   COL_CARD, PWD_KEY_DEL);
    create_pwd_key(parent, "0", PWD_KEY_X0 + PWD_KEY_DX,
                   PWD_KEY_Y0 + 3 * PWD_KEY_DY, COL_CARD, 0);
    create_pwd_key(parent, "确定", PWD_KEY_X0 + 2 * PWD_KEY_DX,
                   PWD_KEY_Y0 + 3 * PWD_KEY_DY, COL_OK, PWD_KEY_OK);
}

/**
 * @brief 创建系统设置页
 */
static void create_settings_page(lv_obj_t *scr)
{
    s_set_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_set_page);
    lv_obj_set_size(s_set_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_set_page, 0, 0);
    lv_obj_set_style_bg_color(s_set_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_set_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_set_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_set_page, LV_OBJ_FLAG_HIDDEN);

    /* 标题 + 强调条 */
    ui_text_init(&s_set_title, s_set_page, SET_TITLE_W, SET_TITLE_H,
                 (LCD_WIDTH - SET_TITLE_W) / 2, 14);
    ui_text_set_scaled(&s_set_title, "系统设置", UI_RGB565(COL_TEXT),
                       UI_RGB565(COL_BG), SET_TITLE_SCALE);

    {
        lv_obj_t *line = lv_obj_create(s_set_page);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 72, 4);
        lv_obj_set_pos(line, (LCD_WIDTH - 72) / 2, 52);
        lv_obj_set_style_bg_color(line, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 第一行：语音播报开关 */
    ui_text_init(&s_set_voice_lbl, s_set_page, 80, UI_INFO_FONT,
                 SET_LBL_X, SET_ROW1_Y + 12);
    ui_text_set(&s_set_voice_lbl, "语音播报", UI_RGB565(COL_TEXT),
                UI_RGB565(COL_BG));

    s_set_voice_btn = ui_btn_create(s_set_page, SET_VOICE_BTN_X, SET_ROW1_Y,
                                    SET_VOICE_BTN_W, SET_ROW_BTN_H,
                                    COL_TEAL, on_voice_toggle_clicked, NULL);
    ui_text_init(&s_set_voice_txt, s_set_voice_btn, SET_VOICE_BTN_W,
                 UI_INFO_FONT, 0, 0);
    if (s_set_voice_txt.img != NULL)
    {
        lv_obj_center(s_set_voice_txt.img);
    }

    /* 第二行：音量 [-] 30% [+] */
    ui_text_init(&s_set_vol_lbl, s_set_page, 48, UI_INFO_FONT,
                 SET_LBL_X, SET_ROW2_Y + 12);
    ui_text_set(&s_set_vol_lbl, "音量", UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));

    ui_text_init(&s_set_vol_val, s_set_page, SET_VOL_VAL_W, UI_INFO_FONT,
                 SET_VOL_VAL_X, SET_ROW2_Y + 12);

    {
        lv_obj_t *dec_btn = ui_btn_create(s_set_page, SET_VOL_DEC_X, SET_ROW2_Y,
                                          SET_VOL_BTN_W, SET_ROW_BTN_H,
                                          COL_CARD, on_volume_down_clicked, NULL);
        ui_text_init(&s_set_vol_dec_txt, dec_btn, SET_VOL_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_set_vol_dec_txt.img != NULL)
        {
            lv_obj_center(s_set_vol_dec_txt.img);
        }
        ui_text_set(&s_set_vol_dec_txt, "-", UI_COLOR_WHITE, UI_RGB565(COL_CARD));

        lv_obj_t *inc_btn = ui_btn_create(s_set_page, SET_VOL_INC_X, SET_ROW2_Y,
                                          SET_VOL_BTN_W, SET_ROW_BTN_H,
                                          COL_CARD, on_volume_up_clicked, NULL);
        ui_text_init(&s_set_vol_inc_txt, inc_btn, SET_VOL_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_set_vol_inc_txt.img != NULL)
        {
            lv_obj_center(s_set_vol_inc_txt.img);
        }
        ui_text_set(&s_set_vol_inc_txt, "+", UI_COLOR_WHITE, UI_RGB565(COL_CARD));
    }

    /* 第三行：相似度 [-] 80% [+]（越高越严格，越低越容易通过） */
    ui_text_init(&s_set_face_lbl, s_set_page, 64, UI_INFO_FONT,
                 SET_LBL_X, SET_ROW3_Y + 12);
    ui_text_set(&s_set_face_lbl, "相似度", UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));

    ui_text_init(&s_set_face_val, s_set_page, SET_VOL_VAL_W, UI_INFO_FONT,
                 SET_VOL_VAL_X, SET_ROW3_Y + 12);

    {
        lv_obj_t *dec_btn = ui_btn_create(s_set_page, SET_VOL_DEC_X, SET_ROW3_Y,
                                          SET_VOL_BTN_W, SET_ROW_BTN_H,
                                          COL_CARD, on_thresh_down_clicked, NULL);
        ui_text_init(&s_set_face_dec_txt, dec_btn, SET_VOL_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_set_face_dec_txt.img != NULL)
        {
            lv_obj_center(s_set_face_dec_txt.img);
        }
        ui_text_set(&s_set_face_dec_txt, "-", UI_COLOR_WHITE, UI_RGB565(COL_CARD));

        lv_obj_t *inc_btn = ui_btn_create(s_set_page, SET_VOL_INC_X, SET_ROW3_Y,
                                          SET_VOL_BTN_W, SET_ROW_BTN_H,
                                          COL_CARD, on_thresh_up_clicked, NULL);
        ui_text_init(&s_set_face_inc_txt, inc_btn, SET_VOL_BTN_W, UI_INFO_FONT, 0, 0);
        if (s_set_face_inc_txt.img != NULL)
        {
            lv_obj_center(s_set_face_inc_txt.img);
        }
        ui_text_set(&s_set_face_inc_txt, "+", UI_COLOR_WHITE, UI_RGB565(COL_CARD));
    }

    /* 第四行：模板自学习开关（识别成功后把高置信样本拟合进模板） */
    ui_text_init(&s_set_learn_lbl, s_set_page, 96, UI_INFO_FONT,
                 SET_LBL_X, SET_ROW4_Y + 12);
    ui_text_set(&s_set_learn_lbl, "模板自学习", UI_RGB565(COL_TEXT),
                UI_RGB565(COL_BG));

    s_set_learn_btn = ui_btn_create(s_set_page, SET_VOICE_BTN_X, SET_ROW4_Y,
                                    SET_VOICE_BTN_W, SET_ROW_BTN_H,
                                    COL_TEAL, on_learn_toggle_clicked, NULL);
    ui_text_init(&s_set_learn_txt, s_set_learn_btn, SET_VOICE_BTN_W,
                 UI_INFO_FONT, 0, 0);
    if (s_set_learn_txt.img != NULL)
    {
        lv_obj_center(s_set_learn_txt.img);
    }

    /* 第五行：修改密码 */
    create_text_button(s_set_page, SET_LBL_X, SET_ROW5_Y,
                       SET_PWD_BTN_W, SET_ROW_BTN_H + 4, "修改密码",
                       COL_TEAL, &s_set_pwd_txt, on_change_pwd_clicked);

    /* 右下角返回 */
    create_text_button(s_set_page,
                       LCD_WIDTH - ADMIN_BACK_W - ADMIN_BACK_MARGIN,
                       LCD_HEIGHT - ADMIN_BACK_H - ADMIN_BACK_MARGIN,
                       ADMIN_BACK_W, ADMIN_BACK_H, "返回",
                       COL_CARD, &s_set_back_txt, on_settings_back_clicked);
}

/**
 * @brief 创建修改密码页（左侧提示 + 右侧数字键盘）
 */
static void create_password_page(lv_obj_t *scr)
{
    s_pwd_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pwd_page);
    lv_obj_set_size(s_pwd_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_pwd_page, 0, 0);
    lv_obj_set_style_bg_color(s_pwd_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_pwd_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_pwd_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pwd_page, LV_OBJ_FLAG_HIDDEN);

    /* 左侧：标题 / 步骤提示 / 掩码 / 提示 */
    ui_text_init(&s_pwd_title, s_pwd_page, 96, UI_INFO_FONT, 14, 16);
    ui_text_set(&s_pwd_title, "修改密码", UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));

    /* 左栏宽度只有 166px（右侧是键盘），提示文字统一用 16 号字 */
    ui_text_init(&s_pwd_step_txt, s_pwd_page, 160, UI_INFO_FONT, 14, 58);
    ui_text_set(&s_pwd_step_txt, "请输入新密码", UI_RGB565(COL_TEXT),
                UI_RGB565(COL_BG));

    ui_text_init(&s_pwd_mask_txt, s_pwd_page, UI_LOGIN_PWD_LEN * 16 + 8,
                 UI_INFO_FONT, 14, 92);
    pwd_update_display();

    ui_text_init(&s_pwd_msg_txt, s_pwd_page, 160, UI_INFO_FONT, 14, 124);
    ui_text_set(&s_pwd_msg_txt, "", UI_RGB565(COL_ERR), UI_RGB565(COL_BG));

    create_text_button(s_pwd_page, 14, 250, ADMIN_BACK_W, ADMIN_BACK_H, "返回",
                       COL_CARD, &s_pwd_back_txt, on_pwd_back_clicked);

    /* 右侧：数字键盘 */
    create_pwd_keypad(s_pwd_page);
}

/**
 * @brief 显示系统设置页并同步控件状态
 */
static void show_settings_page(void)
{
    refresh_voice_ui();

    if (s_set_page != NULL)
    {
        lv_obj_clear_flag(s_set_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_set_page);
        lv_obj_invalidate(lv_scr_act());
    }
}

/**
 * @brief 显示修改密码页（每次进入都从第一步开始）
 */
static void show_password_page(void)
{
    pwd_reset();

    if (s_pwd_page != NULL)
    {
        lv_obj_clear_flag(s_pwd_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_pwd_page);
        lv_obj_invalidate(lv_scr_act());
    }
}

/* ==================== 对外接口 ==================== */

void ui_admin_create(lv_obj_t *scr)
{
    s_admin_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_admin_page);
    lv_obj_set_size(s_admin_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_admin_page, 0, 0);
    lv_obj_set_style_bg_color(s_admin_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_admin_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_admin_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_admin_page, LV_OBJ_FLAG_HIDDEN);

    /* 顶部标题：32 号字库汉字太少，用 16 号字模放大 2 倍 */
    ui_text_init(&s_admin_title, s_admin_page, ADMIN_TITLE_W, ADMIN_TITLE_H,
                 (LCD_WIDTH - ADMIN_TITLE_W) / 2, 22);
    ui_text_set_scaled(&s_admin_title, "管理员配置界面", UI_RGB565(COL_TEXT),
                       UI_RGB565(COL_BG), ADMIN_TITLE_SCALE);

    /* 标题下方强调条 */
    {
        lv_obj_t *line = lv_obj_create(s_admin_page);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 72, 4);
        lv_obj_set_pos(line, (LCD_WIDTH - 72) / 2, 62);
        lv_obj_set_style_bg_color(line, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 中部：两个灰青色主按钮 */
    create_text_button(s_admin_page, ADMIN_BTN_LX, ADMIN_BTN_Y, ADMIN_BTN_W,
                       ADMIN_BTN_H, "录入人脸", COL_TEAL, &s_admin_enroll_txt,
                       on_enroll_clicked);
    create_text_button(s_admin_page, ADMIN_BTN_RX, ADMIN_BTN_Y, ADMIN_BTN_W,
                       ADMIN_BTN_H, "已录入人脸", COL_TEAL, &s_admin_facedb_txt,
                       on_facedb_clicked);

    /* 主按钮与返回之间：系统设置 / 历史记录 / 网络设置 入口 */
    create_text_button(s_admin_page, ADMIN_SET_X, ADMIN_SET_Y, ADMIN_SET_W,
                       ADMIN_SET_H, "系统设置", COL_CARD, &s_admin_set_txt,
                       on_settings_clicked);
    create_text_button(s_admin_page, ADMIN_HIST_X, ADMIN_SET_Y, ADMIN_SET_W,
                       ADMIN_SET_H, "历史记录", COL_CARD, &s_admin_history_txt,
                       on_history_clicked);
    create_text_button(s_admin_page, ADMIN_WIFI_X, ADMIN_SET_Y, ADMIN_SET_W,
                       ADMIN_SET_H, "网络设置", COL_CARD, &s_admin_wifi_txt,
                       on_wifi_clicked);

    /* 右下角：返回按钮（与登入页“退出”按钮同款样式） */
    create_text_button(s_admin_page,
                       LCD_WIDTH - ADMIN_BACK_W - ADMIN_BACK_MARGIN,
                       LCD_HEIGHT - ADMIN_BACK_H - ADMIN_BACK_MARGIN,
                       ADMIN_BACK_W, ADMIN_BACK_H, "返回",
                       COL_CARD, &s_admin_back_txt, on_back_clicked);

    /* 顶部临时提示条（默认隐藏，显示后 3 秒自动消失） */
    s_admin_tip = lv_obj_create(s_admin_page);
    lv_obj_remove_style_all(s_admin_tip);
    lv_obj_set_size(s_admin_tip, ADMIN_TIP_W, ADMIN_TIP_H);
    lv_obj_set_pos(s_admin_tip, ADMIN_TIP_X, ADMIN_TIP_Y);
    lv_obj_set_style_bg_color(s_admin_tip, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_bg_opa(s_admin_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_admin_tip, 10, 0);
    lv_obj_clear_flag(s_admin_tip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_admin_tip, LV_OBJ_FLAG_SCROLLABLE);
    ui_text_init(&s_admin_tip_txt, s_admin_tip, ADMIN_TIP_W - 8, UI_INFO_FONT,
                 4, (ADMIN_TIP_H - UI_INFO_FONT) / 2);
    lv_obj_add_flag(s_admin_tip, LV_OBJ_FLAG_HIDDEN);

    /* 两个子页面（最后创建，显示时覆盖在主页面之上） */
    create_settings_page(scr);
    create_password_page(scr);
}

void ui_admin_show(void)
{
    s_admin_visible = true;

    hide_settings_page();
    hide_password_page();

    if (s_admin_page != NULL)
    {
        lv_obj_clear_flag(s_admin_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_admin_page);
        lv_obj_invalidate(lv_scr_act());   /* 整屏重绘，避免上一页残留 */
    }
}

void ui_admin_hide(void)
{
    s_admin_visible = false;

    if (s_admin_tip != NULL)
    {
        lv_obj_add_flag(s_admin_tip, LV_OBJ_FLAG_HIDDEN);
    }

    hide_admin_page();
    hide_settings_page();
    hide_password_page();
}

/**
 * @brief 顶部提示条自动消失定时器回调
 */
static void admin_tip_timeout_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_admin_tip != NULL)
    {
        lv_obj_add_flag(s_admin_tip, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_admin_show_tip(const char *text)
{
    if (s_admin_tip == NULL)
    {
        return;
    }

    ui_text_set(&s_admin_tip_txt, text, UI_COLOR_WHITE, UI_RGB565(COL_WARN));
    lv_obj_clear_flag(s_admin_tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_admin_tip); /* 保证提示条在主按钮之上 */

    if (s_admin_tip_timer == NULL)
    {
        s_admin_tip_timer = lv_timer_create(admin_tip_timeout_cb, ADMIN_TIP_MS, NULL);
    }
    if (s_admin_tip_timer != NULL)
    {
        lv_timer_set_repeat_count(s_admin_tip_timer, 1);
        lv_timer_reset(s_admin_tip_timer);
    }

    lv_obj_invalidate(s_admin_tip);
}

void ui_admin_set_enroll_cb(void (*cb)(void))
{
    s_enroll_cb = cb;
}

bool ui_admin_is_visible(void)
{
    return s_admin_visible;
}
