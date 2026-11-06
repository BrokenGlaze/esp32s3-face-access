/**
 * @file    ui_login.c
 * @brief   登入界面实现：左侧标题/密码掩码/提示/退出，右侧 3x4 数字键盘
 *
 * @note    布局与配色取自门锁 Demo 的键盘界面（深色背景 + 蓝色掩码 + 圆角按键）。
 *          工程没有引入 LVGL 内置中文字体，文字统一用点阵字库渲染成 LVGL 图像。
 */

#include "ui_login.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lcd.h"
#include "lvgl.h"
#include "lvgl_ui.h"
#include "nvs.h"
#include "ui_admin.h"
#include "ui_common.h"
#include "ui_face.h"

/* ==================== 密码与键盘参数 ==================== */

#define TAG "UI_LOGIN"

#define UI_PWD_DEFAULT "123456"      /* 出厂默认管理员密码 */

/* NVS 命名空间与键：密码修改后立即写入，掉电保存 */
#define LOGIN_NVS_NS "login_cfg"
#define LOGIN_NVS_PWD "password"

/* 管理员密码：默认值写死，可在管理员界面“修改密码”中运行时修改 */
static char s_pwd_code[UI_LOGIN_PWD_LEN + 1] = UI_PWD_DEFAULT;

/* 键盘：3 列 4 行，按键 90x56，列间距 98，行间距 64 */
#define UI_KEY_W 90
#define UI_KEY_H 56
#define UI_KEY_X0 180                /* 键盘第 1 列 X */
#define UI_KEY_Y0 56                 /* 键盘第 1 行 Y */
#define UI_KEY_DX 98                 /* 列间距 */
#define UI_KEY_DY 64                 /* 行间距 */

/* 键盘按键编码：0~9 为数字，其余为功能键 */
#define KEY_DEL 10                   /* 删除 */
#define KEY_OK 11                    /* 确定 */
#define KEY_COUNT 12                 /* 按键总数 */

/* ==================== 配色（0xRRGGBB） ==================== */

#define COL_BG     0x0E1116          /* 全局深色背景 */
#define COL_CARD   0x18202B          /* 次要按钮 */
#define COL_TEXT   0xE6EDF3          /* 主文字 */
#define COL_DIM    0x8B949E          /* 次要文字 */
#define COL_ACCENT 0x2F81F7          /* 密码掩码（蓝） */
#define COL_OK     0x3FB950          /* 确定键（绿） */
#define COL_ERR    0xF85149          /* 错误提示（红） */
#define COL_KEY    0x22303F          /* 数字键底色 */

/* ==================== 内部对象 ==================== */

static lv_obj_t *s_login_page = NULL;       /* 登入页（整屏覆盖） */

static char s_pwd[UI_LOGIN_PWD_LEN + 1];          /* 已输入的密码 */
static uint8_t s_pwd_len = 0;               /* 已输入的位数 */

static void (*s_exit_cb)(void) = NULL;      /* 登入页“退出”回调 */

static ui_text_t s_title_txt;               /* 标题文字 */
static ui_text_t s_mask_txt;                 /* 密码掩码文字 */
static ui_text_t s_msg_txt;                 /* 左侧提示文字 */
static ui_text_t s_exit_txt;                /* 退出按钮文字 */
static ui_text_t s_key_txt[KEY_COUNT];      /* 键盘按键文字 */

/* ==================== 文字绘制 ==================== */

/**
 * @brief 渲染一块图像文字（可左对齐或居中）
 * @param t      文字图像结构
 * @param text   文字（UTF-8），NULL 或空串表示只填背景色
 * @param color  文字颜色（标准 RGB565）
 * @param bg     背景色（标准 RGB565），需与所在控件底色一致，避免出现黑框
 * @param font   字号（16 或 32）
 * @param center true 表示在缓冲内居中，false 表示靠左
 */
static void login_text_paint(ui_text_t *t, const char *text, uint16_t color,
                             uint16_t bg, uint32_t font, bool center)
{
    uint16_t bg_swap;
    uint32_t i;

    if (t->img == NULL || t->buf == NULL)
    {
        return;
    }

    /* 缓冲按“高低字节交换”的 RGB565 存放（与 lcd_render_string_to_buf 一致） */
    bg_swap = (uint16_t)((bg >> 8) | (bg << 8));
    for (i = 0; i < t->w * t->h; i++)
    {
        t->buf[i] = bg_swap;
    }

    if (text != NULL && text[0] != '\0')
    {
        uint32_t tw = ui_text_width(text, font);
        uint32_t x = (center && tw < t->w) ? (t->w - tw) / 2 : 0;
        uint32_t y = (font < t->h) ? (t->h - font) / 2 : 0;

        lcd_render_string_to_buf(t->buf, t->w, t->h, x, y,
                                 color, bg, font, text);
    }

    lv_obj_invalidate(t->img);
}

/**
 * @brief 刷新标题/提示等左对齐文字
 */
static void set_left_text(ui_text_t *t, const char *text, uint16_t color)
{
    login_text_paint(t, text, color, UI_RGB565(COL_BG), UI_INFO_FONT, false);
}

/* ==================== 密码显示 ==================== */

/**
 * @brief 更新密码掩码：已输入显示 *，未输入显示 _（32 号 ASCII）
 */
static void update_pwd_display(void)
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

    login_text_paint(&s_mask_txt, mask, UI_RGB565(COL_ACCENT),
                     UI_RGB565(COL_BG), 32, false);
}

/* ==================== 事件回调 ==================== */

/**
 * @brief 登入页“退出”按钮：回调业务侧恢复采集，并回到人脸识别界面
 */
static void on_exit_clicked(lv_event_t *e)
{
    if (s_exit_cb != NULL)
    {
        s_exit_cb();
    }
    ui_login_hide();
    ui_face_show();
}

/**
 * @brief 键盘按键处理：数字追加、删除、确定
 */
static void on_key_clicked(lv_event_t *e)
{
    const int code = (int)(lv_uintptr_t)lv_event_get_user_data(e);

    if (code >= 0 && code <= 9)
    {
        /* 数字键：最多输入 UI_LOGIN_PWD_LEN 位，输入后清除上一次的错误提示 */
        if (s_pwd_len < UI_LOGIN_PWD_LEN)
        {
            s_pwd[s_pwd_len++] = (char)('0' + code);
            s_pwd[s_pwd_len] = '\0';
            update_pwd_display();
            set_left_text(&s_msg_txt, "", UI_RGB565(COL_ERR));
        }
        return;
    }

    switch (code)
    {
    case KEY_DEL:
        if (s_pwd_len > 0)
        {
            s_pwd[--s_pwd_len] = '\0';
            update_pwd_display();
        }
        break;

    case KEY_OK:
        if (s_pwd_len == 0)
        {
            set_left_text(&s_msg_txt, "请输入6位密码", UI_RGB565(COL_ERR));
            break;
        }

        if (s_pwd_len == UI_LOGIN_PWD_LEN && strcmp(s_pwd, s_pwd_code) == 0)
        {
            /* 密码正确：直接进入管理员页 */
            ui_login_hide();
            ui_face_hide();
            ui_admin_show();
        }
        else
        {
            /* 密码错误：提示 + 清空输入 */
            set_left_text(&s_msg_txt, "密码错误", UI_RGB565(COL_ERR));
            s_pwd_len = 0;
            s_pwd[0] = '\0';
            update_pwd_display();
        }
        break;

    default:
        break;
    }
}

/* ==================== 界面创建 ==================== */

/**
 * @brief 创建一个按键（含居中文字）
 */
static lv_obj_t *create_key(lv_obj_t *parent, const char *text, lv_coord_t x,
                            lv_coord_t y, uint32_t bg, uint16_t txt_color,
                            int code)
{
    lv_obj_t *btn = ui_btn_create(parent, x, y, UI_KEY_W, UI_KEY_H, bg,
                                  on_key_clicked, (void *)(lv_uintptr_t)code);

    ui_text_init(&s_key_txt[code], btn, 48, UI_INFO_FONT, 0, 0);
    if (s_key_txt[code].img != NULL)
    {
        lv_obj_center(s_key_txt[code].img);
    }
    login_text_paint(&s_key_txt[code], text, txt_color, UI_RGB565(bg),
                     UI_INFO_FONT, true);
    return btn;
}

/**
 * @brief 创建数字键盘：3x4（1~9、删除、0、确定）
 */
static void create_keypad(lv_obj_t *parent)
{
    static const char *digits[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
    int i;

    for (i = 0; i < 9; i++)
    {
        create_key(parent, digits[i],
                   UI_KEY_X0 + (i % 3) * UI_KEY_DX,
                   UI_KEY_Y0 + (i / 3) * UI_KEY_DY,
                   COL_KEY, UI_COLOR_WHITE, i + 1);
    }

    /* 底行：删除(10) / 0 / 确定(11) */
    create_key(parent, "删除", UI_KEY_X0, UI_KEY_Y0 + 3 * UI_KEY_DY,
               COL_KEY, UI_COLOR_WHITE, KEY_DEL);
    create_key(parent, "0", UI_KEY_X0 + UI_KEY_DX, UI_KEY_Y0 + 3 * UI_KEY_DY,
               COL_KEY, UI_COLOR_WHITE, 0);
    create_key(parent, "确定", UI_KEY_X0 + 2 * UI_KEY_DX, UI_KEY_Y0 + 3 * UI_KEY_DY,
               COL_OK, UI_COLOR_WHITE, KEY_OK);
}

/**
 * @brief 从 NVS 载入已保存的管理员密码（无记录或 NVS 不可用时保留默认密码）
 */
static void login_load_password(void)
{
    nvs_handle_t handle;
    size_t len = sizeof(s_pwd_code);
    char pwd[UI_LOGIN_PWD_LEN + 1];

    if (nvs_open(LOGIN_NVS_NS, NVS_READONLY, &handle) != ESP_OK)
    {
        return; /* 首次上电尚无记录 */
    }

    /* 读进临时缓冲，长度必须正好是指定位数才采用，避免脏数据 */
    if (nvs_get_str(handle, LOGIN_NVS_PWD, pwd, &len) == ESP_OK &&
        len == UI_LOGIN_PWD_LEN + 1)
    {
        memcpy(s_pwd_code, pwd, sizeof(s_pwd_code));
    }

    nvs_close(handle);
}

void ui_login_create(lv_obj_t *scr)
{
    login_load_password();

    s_login_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_login_page);
    lv_obj_set_size(s_login_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_login_page, 0, 0);
    lv_obj_set_style_bg_color(s_login_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_login_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_login_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_login_page, LV_OBJ_FLAG_HIDDEN);

    /* 左上：标题 */
    ui_text_init(&s_title_txt, s_login_page, 160, UI_INFO_FONT, 14, 16);
    set_left_text(&s_title_txt, "请输入密码", UI_RGB565(COL_TEXT));

    /* 标题下方：密码掩码（32 号星号/下划线）
     * 缓冲宽度 = 每个字符占 16px + 右侧留一个字模宽度 32px，
     * 否则第 6 个字符会因“换行判断”被排到缓冲外而显示不全。 */
    ui_text_init(&s_mask_txt, s_login_page, UI_LOGIN_PWD_LEN * 16 + 32, 32, 14, 62);
    update_pwd_display();

    /* 掩码下方：错误提示（最多 3 行，宽度 160 自动换行） */
    ui_text_init(&s_msg_txt, s_login_page, 160, UI_INFO_FONT * 3, 14, 110);
    set_left_text(&s_msg_txt, "", UI_RGB565(COL_ERR));

    /* 左下：退出按钮（回到人脸识别界面） */
    {
        lv_obj_t *exit_btn = ui_btn_create(s_login_page, 14, 250, 120, 48,
                                           COL_CARD, on_exit_clicked, NULL);
        ui_text_init(&s_exit_txt, exit_btn, 48, UI_INFO_FONT, 0, 0);
        if (s_exit_txt.img != NULL)
        {
            lv_obj_center(s_exit_txt.img);
        }
        login_text_paint(&s_exit_txt, "退出", UI_COLOR_WHITE,
                         UI_RGB565(COL_CARD), UI_INFO_FONT, true);
    }

    /* 右侧：数字键盘 */
    create_keypad(s_login_page);

    printf("ui_login: login page ready\n");
}

void ui_login_show(void)
{
    /* 复位输入与提示 */
    s_pwd_len = 0;
    s_pwd[0] = '\0';
    update_pwd_display();
    set_left_text(&s_msg_txt, "", UI_RGB565(COL_ERR));

    if (s_login_page != NULL)
    {
        lv_obj_clear_flag(s_login_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_login_hide(void)
{
    if (s_login_page != NULL)
    {
        lv_obj_add_flag(s_login_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_login_set_exit_cb(void (*cb)(void))
{
    s_exit_cb = cb;
}

void ui_login_set_password(const char *pwd)
{
    nvs_handle_t handle;
    char old[UI_LOGIN_PWD_LEN + 1];
    esp_err_t ret;

    if (pwd == NULL || strlen(pwd) != UI_LOGIN_PWD_LEN)
    {
        return;
    }

    memcpy(old, s_pwd_code, sizeof(old));
    memcpy(s_pwd_code, pwd, UI_LOGIN_PWD_LEN);
    s_pwd_code[UI_LOGIN_PWD_LEN] = '\0';

    ret = nvs_open(LOGIN_NVS_NS, NVS_READWRITE, &handle);
    if (ret == ESP_OK)
    {
        ret = nvs_set_str(handle, LOGIN_NVS_PWD, s_pwd_code);
        if (ret == ESP_OK)
        {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if (ret != ESP_OK)
    {
        /* 保存失败：回滚内存中的密码，避免重启后又变回旧密码却没有提示 */
        memcpy(s_pwd_code, old, sizeof(old));
        ESP_LOGW(TAG, "save password failed, ret=%d", (int)ret);
    }
}
