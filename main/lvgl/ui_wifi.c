/**
 * @file    ui_wifi.c
 * @brief   网络设置界面实现：状态显示 + 手机配网入口
 *
 * @note    页面每 1 秒刷新一次状态（连接结果由 Wi-Fi 事件异步更新）。
 *          离开本页会自动停止配网热点与网页服务，避免长期占用内存。
 */

#include "ui_wifi.h"

#include <stdio.h>

#include "lcd.h"
#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_admin.h"
#include "ui_common.h"
#include "wifi_manager.h"
#include "wifi_prov.h"

/* ==================== 配色（0xRRGGBB） ==================== */
#define COL_BG   0x0E1116
#define COL_TEXT 0xE6EDF3
#define COL_DIM  0x8B949E
#define COL_CARD 0x18202B
#define COL_TEAL 0x3F6A6A
#define COL_ERR  0xF85149

/* ==================== 布局参数（横屏 480x320） ==================== */
#define WIFI_TITLE_W     128
#define WIFI_TITLE_H     32
#define WIFI_TITLE_SCALE 2

#define WIFI_TXT_W 440
#define WIFI_TXT_X 20

#define WIFI_STATUS_Y 64
#define WIFI_DETAIL_Y 84
#define WIFI_HINT_Y   108

#define WIFI_BTN_W 130
#define WIFI_BTN_H 48
#define WIFI_BTN_Y 220
#define WIFI_BTN_X1 25
#define WIFI_BTN_X2 175
#define WIFI_BTN_X3 325

/* ==================== 内部对象 ==================== */
static lv_obj_t *s_wifi_page = NULL;
static ui_text_t s_title;
static ui_text_t s_status_txt;
static ui_text_t s_detail_txt;
static ui_text_t s_hint_txt;
static ui_text_t s_prov_txt;
static ui_text_t s_reconnect_txt;
static ui_text_t s_back_txt;

static lv_timer_t *s_tick = NULL;

/* ==================== 状态刷新 ==================== */

/**
 * @brief 刷新页面上的状态文字与按钮文案
 */
static void refresh_texts(void)
{
    char buf[96];
    wifi_mgr_state_t state = wifi_manager_get_state();
    const char *ssid = wifi_manager_get_ssid();
    const char *ip = wifi_manager_get_ip();

    switch (state)
    {
    case WIFI_MGR_CONNECTED:
        snprintf(buf, sizeof(buf), "已连接: %s", ssid);
        ui_text_set(&s_status_txt, buf, UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));
        snprintf(buf, sizeof(buf), "IP: %s", ip);
        ui_text_set(&s_detail_txt, buf, UI_RGB565(COL_DIM), UI_RGB565(COL_BG));
        break;

    case WIFI_MGR_CONNECTING:
        ui_text_set(&s_status_txt, "正在连接", UI_RGB565(COL_TEXT),
                    UI_RGB565(COL_BG));
        snprintf(buf, sizeof(buf), "SSID: %s", ssid);
        ui_text_set(&s_detail_txt, buf, UI_RGB565(COL_DIM), UI_RGB565(COL_BG));
        break;

    case WIFI_MGR_FAILED:
        ui_text_set(&s_status_txt, "连接失败", UI_RGB565(COL_ERR),
                    UI_RGB565(COL_BG));
        ui_text_set(&s_detail_txt, "请检查密码或重新配网", UI_RGB565(COL_DIM),
                    UI_RGB565(COL_BG));
        break;

    default:
        ui_text_set(&s_status_txt, "未连接", UI_RGB565(COL_DIM),
                    UI_RGB565(COL_BG));
        ui_text_set(&s_detail_txt, "", UI_RGB565(COL_DIM), UI_RGB565(COL_BG));
        break;
    }

    if (wifi_prov_is_running())
    {
        snprintf(buf, sizeof(buf), "热点: %s   网页: %s",
                 WIFI_PROV_AP_SSID, WIFI_PROV_AP_URL);
        ui_text_set(&s_hint_txt, buf, UI_RGB565(COL_TEXT), UI_RGB565(COL_BG));
        ui_text_set(&s_prov_txt, "停止配网", UI_COLOR_WHITE, UI_RGB565(COL_TEAL));
    }
    else
    {
        ui_text_set(&s_hint_txt,
                    (ssid[0] != '\0') ? "可点击 手机配网 重新配置"
                                      : "点击 手机配网 开始配置",
                    UI_RGB565(COL_DIM), UI_RGB565(COL_BG));
        ui_text_set(&s_prov_txt, "手机配网", UI_COLOR_WHITE, UI_RGB565(COL_TEAL));
    }
}

/**
 * @brief 1 秒周期刷新（只在页面可见时运行）
 */
static void wifi_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_texts();
}

/* ==================== 事件回调 ==================== */

/**
 * @brief “手机配网”/“停止配网”按钮：启动或停止配网服务
 */
static void on_prov_clicked(lv_event_t *e)
{
    (void)e;

    if (wifi_prov_is_running())
    {
        wifi_prov_stop();
    }
    else if (wifi_prov_start() != ESP_OK)
    {
        ui_text_set(&s_hint_txt, "配网服务启动失败", UI_RGB565(COL_ERR),
                    UI_RGB565(COL_BG));
    }
    refresh_texts();
}

/**
 * @brief “重新连接”：用 NVS 中保存的凭据再连一次
 */
static void on_reconnect_clicked(lv_event_t *e)
{
    (void)e;

    if (wifi_manager_reconnect() != ESP_OK)
    {
        ui_text_set(&s_hint_txt, "尚未保存 Wi-Fi，请先手机配网",
                    UI_RGB565(COL_ERR), UI_RGB565(COL_BG));
        return;
    }
    refresh_texts();
}

/**
 * @brief “返回”：停止配网并回到管理员界面
 */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;

    ui_wifi_hide();
    ui_admin_show();
}

/* ==================== 界面创建 ==================== */

/**
 * @brief 创建一个带居中文字的按钮
 */
static void create_text_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                               const char *text, uint32_t bg, ui_text_t *t,
                               lv_event_cb_t cb)
{
    lv_obj_t *btn = ui_btn_create(parent, x, y, WIFI_BTN_W, WIFI_BTN_H, bg, cb, NULL);

    ui_text_init(t, btn, WIFI_BTN_W, UI_INFO_FONT, 0, 0);
    if (t->img != NULL)
    {
        lv_obj_center(t->img);
    }
    ui_text_set(t, text, UI_COLOR_WHITE, UI_RGB565(bg));
}

void ui_wifi_create(lv_obj_t *scr)
{
    s_wifi_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wifi_page);
    lv_obj_set_size(s_wifi_page, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_wifi_page, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_wifi_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);

    /* 标题 + 强调条 */
    ui_text_init(&s_title, s_wifi_page, WIFI_TITLE_W, WIFI_TITLE_H,
                 (LCD_WIDTH - WIFI_TITLE_W) / 2, 12);
    ui_text_set_scaled(&s_title, "网络设置", UI_RGB565(COL_TEXT),
                       UI_RGB565(COL_BG), WIFI_TITLE_SCALE);

    {
        lv_obj_t *line = lv_obj_create(s_wifi_page);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 72, 4);
        lv_obj_set_pos(line, (LCD_WIDTH - 72) / 2, 48);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x2F81F7), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }

    ui_text_init(&s_status_txt, s_wifi_page, WIFI_TXT_W, UI_INFO_FONT,
                 WIFI_TXT_X, WIFI_STATUS_Y);
    ui_text_init(&s_detail_txt, s_wifi_page, WIFI_TXT_W, UI_INFO_FONT,
                 WIFI_TXT_X, WIFI_DETAIL_Y);
    ui_text_init(&s_hint_txt, s_wifi_page, WIFI_TXT_W, UI_INFO_FONT,
                 WIFI_TXT_X, WIFI_HINT_Y);

    /* 底部三个按钮：手机配网 / 重新连接 / 返回 */
    create_text_button(s_wifi_page, WIFI_BTN_X1, WIFI_BTN_Y, "手机配网",
                       COL_TEAL, &s_prov_txt, on_prov_clicked);
    create_text_button(s_wifi_page, WIFI_BTN_X2, WIFI_BTN_Y, "重新连接",
                       COL_CARD, &s_reconnect_txt, on_reconnect_clicked);
    create_text_button(s_wifi_page, WIFI_BTN_X3, WIFI_BTN_Y, "返回",
                       COL_CARD, &s_back_txt, on_back_clicked);
}

void ui_wifi_show(void)
{
    refresh_texts();

    if (s_wifi_page != NULL)
    {
        lv_obj_clear_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tick == NULL)
    {
        s_tick = lv_timer_create(wifi_tick_cb, 1000, NULL);
    }
}

void ui_wifi_hide(void)
{
    if (s_tick != NULL)
    {
        lv_timer_del(s_tick);
        s_tick = NULL;
    }

    /* 离开页面即停止配网，释放热点与 HTTP 服务占用的资源 */
    if (wifi_prov_is_running())
    {
        wifi_prov_stop();
    }

    if (s_wifi_page != NULL)
    {
        lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }
}
