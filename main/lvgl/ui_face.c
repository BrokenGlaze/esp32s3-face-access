/**
 * @file    ui_face.c
 * @brief   人脸识别界面实现：摄像头预览 + “登入”“识别”按钮 + 预览下方识别状态横幅
 *
 * @note    - 预览用 lv_img + lv_img_dsc_t 直接指向 PSRAM 缩放缓冲，每帧只需
 *            缩放 + lv_obj_invalidate()，不需要逐次 memcpy 到 LVGL 内部缓冲
 *          - CONFIG_LV_COLOR_16_SWAP=y 时 LVGL 期望图像数据为“高位字节在前”
 *            （大端）的 RGB565；摄像头帧正好是大端，故预览可直接使用
 *          - 中文文字统一用点阵字库渲染成 LVGL 图像（工程未引入 CJK 字库）
 *          - 识别结果（成功/失败/未检测到人脸）显示为预览画面下方的彩色横幅，
 *            标题 32 像素高、细节 16 像素，配色区分状态，辨识度更高
 */

#include "ui_face.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_ui.h"
#include "ui_common.h"
#include "wifi_manager.h"
#include "esp_wifi.h"

/* ==================== 配色（0xRRGGBB） ==================== */
#define COL_TEAL 0x3F6A6A           /* 灰青色主按钮，与管理员页一致 */
#define COL_LINE 0x2F81F7           /* 标题下方强调条 */
#define COL_TEXT 0xE6EDF3           /* 标题文字 */

/* 左下角 Wi-Fi 图标配色 */
#define COL_WIFI_OFF  0x484F58      /* 未配置/未连接：灰 */
#define COL_WIFI_WARN 0xD29922      /* 连接中：橙黄（与提示横幅一致） */
#define COL_WIFI_OK   0x3FB950      /* 已连接：绿（与成功横幅一致） */

/* 状态横幅配色：背景 / 边框 */
#define COL_ST_INFO      0x1F2A36   /* 一般信息：深灰蓝 */
#define COL_ST_INFO_LINE 0x3A4553
#define COL_ST_OK        0x1E7B3C   /* 成功：绿 */
#define COL_ST_OK_LINE   0x3FB950
#define COL_ST_FAIL      0xA32A24   /* 失败：红 */
#define COL_ST_FAIL_LINE 0xF85149
#define COL_ST_WARN      0x8A5A00   /* 提示/警告：橙黄 */
#define COL_ST_WARN_LINE 0xD29922

/* ==================== 左栏布局（0 ~ UI_PREVIEW_X，横屏 480x320） ==================== */
#define FACE_TITLE_W     128        /* “人脸识别”4 字 × 16px × 2 倍 */
#define FACE_TITLE_H     32
#define FACE_TITLE_SCALE 2
#define FACE_TITLE_Y     8

#define FACE_BTN_W 124
#define FACE_BTN_H 72
#define FACE_BTN_X ((UI_PREVIEW_X - FACE_BTN_W) / 2)  /* 左栏水平居中（8） */
#define FACE_BTN_Y0 76              /* “登入”按钮，在标题与信息区之间居中 */
#define FACE_BTN_DY 116             /* 与“识别”按钮的间距（两者之间留 44px 空白） */

#define FACE_BTN_TXT_W     64       /* 按钮文字缓冲：2 字 × 16px × 2 倍 */
#define FACE_BTN_TXT_H     32
#define FACE_BTN_TXT_SCALE 2

/* ==================== 状态横幅布局（预览画面下方） ==================== */
#define STATUS_BOX_W     320
#define STATUS_BOX_H     80
#define STATUS_BOX_X     (UI_PREVIEW_X + (UI_PREVIEW_W - STATUS_BOX_W) / 2)  /* 150 */
#define STATUS_BOX_Y     (LCD_HEIGHT - STATUS_BOX_H - 8)                    /* 232 */
#define STATUS_TXT_W     (STATUS_BOX_W - 8)                                 /* 312 */
#define STATUS_TITLE_H   32
#define STATUS_DETAIL_H  UI_INFO_FONT
#define STATUS_TITLE_Y   10
#define STATUS_DETAIL_Y  50

/* ==================== 左下角 Wi-Fi 图标布局 ==================== */
#define WIFI_ICON_W     44
#define WIFI_ICON_H     32
#define WIFI_ICON_X     ((UI_PREVIEW_X - WIFI_ICON_W) / 2)
#define WIFI_ICON_Y     280
#define WIFI_ARC_W      4           /* 圆弧线宽 */
#define WIFI_RSSI_TICKS 30          /* 500ms × 30 = 15 秒刷新一次信号强度 */

/* ==================== 内部对象 ==================== */
static lv_obj_t *s_preview_img = NULL;      /* 摄像头预览图像 */
static lv_img_dsc_t s_preview_dsc;
static uint16_t *s_preview_buf = NULL;      /* 预览缩放缓冲（PSRAM，340x320） */

static lv_obj_t *s_status_box = NULL;       /* 状态横幅背景（彩色圆角条） */
static ui_text_t s_status_title;            /* 横幅标题（放大 2 倍） */
static ui_text_t s_status_detail;           /* 横幅细节文字 */
static bool s_has_status = false;           /* 当前是否有状态需要显示 */

static lv_obj_t *s_login_btn = NULL;        /* 登入按钮 */
static ui_text_t s_login_btn_txt;           /* 登入按钮文字 */

static lv_obj_t *s_recog_btn = NULL;        /* 识别按钮 */
static ui_text_t s_recog_btn_txt;           /* 识别按钮文字 */

static ui_text_t s_title_txt;               /* 左栏标题（放大显示） */
static lv_obj_t *s_title_line = NULL;       /* 标题下方强调条 */

static void (*s_login_cb)(void) = NULL;     /* 登入按钮按下回调 */
static void (*s_recog_cb)(void) = NULL;     /* 识别按钮按下回调 */

static lv_obj_t *s_wifi_icon = NULL;                /* 左下角 Wi-Fi 图标 */
static bool s_wifi_pulse = false;                   /* 连接中呼吸闪烁相位 */
static int8_t s_wifi_rssi = -127;                   /* 当前信号强度 */
static bool s_wifi_configured = true;               /* 是否保存过凭据（缓存，绘制时不再读 NVS） */
static wifi_mgr_state_t s_wifi_last_state = WIFI_MGR_IDLE;
static int s_wifi_rssi_tick = 0;                    /* RSSI 刷新计数 */

/* ==================== 事件回调 ==================== */

/**
 * @brief “登入”按钮回调转发（ui_btn_create 只在 LV_EVENT_CLICKED 触发）
 */
static void login_btn_event_cb(lv_event_t *e)
{
    if (s_login_cb != NULL)
    {
        s_login_cb();
    }
}

/**
 * @brief “识别”按钮回调转发（ui_btn_create 只在 LV_EVENT_CLICKED 触发）
 */
static void recog_btn_event_cb(lv_event_t *e)
{
    if (s_recog_cb != NULL)
    {
        s_recog_cb();
    }
}

/* ==================== 界面创建 ==================== */

/**
 * @brief 创建一个带居中文字的灰青色圆角按钮
 */
static lv_obj_t *create_text_button(lv_obj_t *parent, lv_coord_t y,
                                    const char *text, ui_text_t *t,
                                    lv_event_cb_t cb)
{
    lv_obj_t *btn = ui_btn_create(parent, FACE_BTN_X, y, FACE_BTN_W, FACE_BTN_H,
                                  COL_TEAL, cb, NULL);

    /* 按钮文字放大 2 倍（32px 高），在按钮内居中 */
    ui_text_init(t, btn, FACE_BTN_TXT_W, FACE_BTN_TXT_H, 0, 0);
    if (t->img != NULL)
    {
        lv_obj_center(t->img);
    }
    ui_text_set_scaled(t, text, UI_COLOR_WHITE, UI_RGB565(COL_TEAL),
                       FACE_BTN_TXT_SCALE);
    return btn;
}

/**
 * @brief 创建左栏标题与强调条
 */
static void create_title(lv_obj_t *parent)
{
    ui_text_init(&s_title_txt, parent, FACE_TITLE_W, FACE_TITLE_H,
                 (UI_PREVIEW_X - FACE_TITLE_W) / 2, FACE_TITLE_Y);
    ui_text_set_scaled(&s_title_txt, "人脸识别", UI_RGB565(COL_TEXT),
                       0x0000, FACE_TITLE_SCALE);

    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 56, 4);
    lv_obj_set_pos(line, (UI_PREVIEW_X - 56) / 2, 48);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    s_title_line = line;
}

/**
 * @brief 创建“登入”与“识别”按钮（左栏竖排）
 */
static void create_buttons(lv_obj_t *parent)
{
    s_login_btn = create_text_button(parent, FACE_BTN_Y0, "登入",
                                     &s_login_btn_txt, login_btn_event_cb);

    s_recog_btn = create_text_button(parent, FACE_BTN_Y0 + FACE_BTN_DY, "识别",
                                     &s_recog_btn_txt, recog_btn_event_cb);
}

/**
 * @brief 按状态类别取横幅背景色
 */
static uint32_t status_bg_color(int kind)
{
    switch (kind)
    {
    case UI_STATUS_OK:
        return COL_ST_OK;
    case UI_STATUS_FAIL:
        return COL_ST_FAIL;
    case UI_STATUS_WARN:
        return COL_ST_WARN;
    default:
        return COL_ST_INFO;
    }
}

/**
 * @brief 按状态类别取横幅边框色
 */
static uint32_t status_border_color(int kind)
{
    switch (kind)
    {
    case UI_STATUS_OK:
        return COL_ST_OK_LINE;
    case UI_STATUS_FAIL:
        return COL_ST_FAIL_LINE;
    case UI_STATUS_WARN:
        return COL_ST_WARN_LINE;
    default:
        return COL_ST_INFO_LINE;
    }
}

/**
 * @brief 创建预览画面下方的状态横幅（默认隐藏、不拦截触摸）
 */
static void create_status_box(lv_obj_t *parent)
{
    s_status_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_status_box);
    lv_obj_set_size(s_status_box, STATUS_BOX_W, STATUS_BOX_H);
    lv_obj_set_pos(s_status_box, STATUS_BOX_X, STATUS_BOX_Y);
    lv_obj_set_style_radius(s_status_box, 12, 0);
    lv_obj_set_style_border_width(s_status_box, 2, 0);
    lv_obj_set_style_border_opa(s_status_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_status_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_status_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_status_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);

    /* 标题（16 号字模放大 2 倍）与细节文字都居中于横幅内 */
    ui_text_init(&s_status_title, s_status_box, STATUS_TXT_W, STATUS_TITLE_H,
                 (STATUS_BOX_W - STATUS_TXT_W) / 2, STATUS_TITLE_Y);
    ui_text_init(&s_status_detail, s_status_box, STATUS_TXT_W, STATUS_DETAIL_H,
                 (STATUS_BOX_W - STATUS_TXT_W) / 2, STATUS_DETAIL_Y);
}

/* ==================== 左下角 Wi-Fi 图标 ==================== */

/**
 * @brief 圆弧采样点（45°~135°，每 9° 一点，cos/sin 千分比）
 */
static const int16_t s_arc_cos[11] = { 707, 588, 454, 309, 156, 0, -156, -309, -454, -588, -707 };
static const int16_t s_arc_sin[11] = { 707, 809, 891, 951, 988, 1000, 988, 951, 891, 809, 707 };

/**
 * @brief 画一段向上张开的圆弧（用折线模拟，避免依赖 LVGL 的角度约定）
 */
static void wifi_draw_arc(lv_draw_ctx_t *ctx, int32_t cx, int32_t cy, int32_t r,
                          lv_color_t color, lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;
    lv_point_t p0;
    lv_point_t p1;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = WIFI_ARC_W;
    dsc.opa = opa;
    dsc.round_start = 1;
    dsc.round_end = 1;

    p0.x = (lv_coord_t)(cx + (r * s_arc_cos[0]) / 1000);
    p0.y = (lv_coord_t)(cy - (r * s_arc_sin[0]) / 1000);

    for (int i = 1; i < 11; i++)
    {
        p1.x = (lv_coord_t)(cx + (r * s_arc_cos[i]) / 1000);
        p1.y = (lv_coord_t)(cy - (r * s_arc_sin[i]) / 1000);
        lv_draw_line(ctx, &dsc, &p0, &p1);
        p0 = p1;
    }
}

/**
 * @brief 图标绘制：底部圆点 + 1~3 段圆弧
 */
static void wifi_icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t coords;
    int32_t cx;
    int32_t cy;
    uint32_t color;
    lv_opa_t opa = LV_OPA_COVER;
    int arcs = 3;
    wifi_mgr_state_t st = wifi_manager_get_state();

    lv_obj_get_coords(obj, &coords);
    cx = coords.x1 + WIFI_ICON_W / 2;
    cy = coords.y1 + WIFI_ICON_H - 4;      /* 圆点落在图标底部 */

    if (st == WIFI_MGR_CONNECTED)
    {
        color = COL_WIFI_OK;
        arcs = (s_wifi_rssi >= -60) ? 3 : ((s_wifi_rssi >= -70) ? 2 : 1);
    }
    else if (st == WIFI_MGR_CONNECTING)
    {
        color = COL_WIFI_WARN;
        arcs = 1;                          /* 只亮最内一段 */
        opa = s_wifi_pulse ? LV_OPA_COVER : LV_OPA_40;
    }
    else
    {
        color = COL_WIFI_OFF;
        if (!s_wifi_configured)
        {
            opa = LV_OPA_40;               /* 从未配置过：整体更暗 */
        }
    }

    for (int i = 0; i < arcs; i++)
    {
        wifi_draw_arc(ctx, cx, cy, 10 + 6 * i, lv_color_hex(color), opa);
    }

    lv_draw_rect_dsc_t rdsc;
    lv_area_t dot;

    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color = lv_color_hex(color);
    rdsc.bg_opa = opa;
    rdsc.radius = LV_RADIUS_CIRCLE;

    dot.x1 = (lv_coord_t)(cx - 3);
    dot.y1 = (lv_coord_t)(cy - 3);
    dot.x2 = (lv_coord_t)(cx + 3);
    dot.y2 = (lv_coord_t)(cy + 3);
    lv_draw_rect(ctx, &rdsc, &dot);
}

/**
 * @brief 定时刷新：连接中呼吸闪烁，已连接每 15 秒读一次 RSSI
 */
static void wifi_icon_timer_cb(lv_timer_t *timer)
{
    wifi_mgr_state_t st;
    bool changed = false;

    (void)timer;
    if (s_wifi_icon == NULL)
    {
        return;
    }

    st = wifi_manager_get_state();
    if (st != s_wifi_last_state)
    {
        s_wifi_last_state = st;
        s_wifi_configured = wifi_manager_has_credentials();
        s_wifi_rssi_tick = 0;
        changed = true;
    }

    if (st == WIFI_MGR_CONNECTING)
    {
        s_wifi_pulse = !s_wifi_pulse;
        changed = true;
    }
    else if (st == WIFI_MGR_CONNECTED)
    {
        if (++s_wifi_rssi_tick >= WIFI_RSSI_TICKS)
        {
            wifi_ap_record_t ap;

            s_wifi_rssi_tick = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            {
                s_wifi_rssi = ap.rssi;
                changed = true;
            }
        }
    }

    if (changed && !lv_obj_has_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_invalidate(s_wifi_icon);
    }
}

/**
 * @brief 创建左下角 Wi-Fi 图标（纯矢量绘制，不占图片资源）
 */
static void create_wifi_icon(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);

    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, WIFI_ICON_W, WIFI_ICON_H);
    lv_obj_set_pos(icon, WIFI_ICON_X, WIFI_ICON_Y);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(icon, wifi_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_wifi_icon = icon;
    s_wifi_configured = wifi_manager_has_credentials();
    lv_timer_create(wifi_icon_timer_cb, 500, NULL);
}

void ui_face_create(lv_obj_t *scr)
{
    s_preview_buf = (uint16_t *)ui_alloc(UI_PREVIEW_W * UI_PREVIEW_H * sizeof(uint16_t));
    if (s_preview_buf == NULL)
    {
        printf("ui_face: preview buffer alloc failed\n");
        return;
    }
    memset(s_preview_buf, 0, UI_PREVIEW_W * UI_PREVIEW_H * sizeof(uint16_t));

    s_preview_img = ui_image_create(scr, &s_preview_dsc, s_preview_buf,
                                    UI_PREVIEW_W, UI_PREVIEW_H, UI_PREVIEW_X, UI_PREVIEW_Y);

    /* 状态横幅创建在预览之后，显示在画面下方、预览之上 */
    create_status_box(scr);

    create_title(scr);
    create_buttons(scr);
    create_wifi_icon(scr);
}

void ui_face_show(void)
{
    if (s_preview_img != NULL)
    {
        lv_obj_clear_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title_txt.img != NULL)
    {
        lv_obj_clear_flag(s_title_txt.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title_line != NULL)
    {
        lv_obj_clear_flag(s_title_line, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_login_btn != NULL)
    {
        lv_obj_clear_flag(s_login_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_recog_btn != NULL)
    {
        lv_obj_clear_flag(s_recog_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_status_box != NULL && s_has_status)
    {
        lv_obj_clear_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_icon != NULL)
    {
        lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_face_show_camera_only(void)
{
    if (s_preview_img != NULL)
    {
        lv_obj_clear_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title_txt.img != NULL)
    {
        lv_obj_add_flag(s_title_txt.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title_line != NULL)
    {
        lv_obj_add_flag(s_title_line, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_login_btn != NULL)
    {
        lv_obj_add_flag(s_login_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_recog_btn != NULL)
    {
        lv_obj_add_flag(s_recog_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_status_box != NULL)
    {
        lv_obj_add_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_icon != NULL)
    {
        lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_face_hide(void)
{
    if (s_preview_img != NULL)
    {
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_status_box != NULL)
    {
        lv_obj_add_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title_txt.img != NULL)
    {
        lv_obj_add_flag(s_title_txt.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title_line != NULL)
    {
        lv_obj_add_flag(s_title_line, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_login_btn != NULL)
    {
        lv_obj_add_flag(s_login_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_recog_btn != NULL)
    {
        lv_obj_add_flag(s_recog_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_icon != NULL)
    {
        lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_face_frame(const uint16_t *src, uint32_t src_w, uint32_t src_h)
{
    uint32_t x_step, y_step, sx, sy, dx, dy;

    if (s_preview_img == NULL || src == NULL || src_w == 0 || src_h == 0)
    {
        return;
    }

    x_step = (src_w << 16) / UI_PREVIEW_W;
    y_step = (src_h << 16) / UI_PREVIEW_H;
    sy = 0;

    for (dy = 0; dy < UI_PREVIEW_H; dy++)
    {
        const uint16_t *src_row = src + (sy >> 16) * src_w;
        uint16_t *dst_row = s_preview_buf + dy * UI_PREVIEW_W;
        sx = 0;

        for (dx = 0; dx < UI_PREVIEW_W; dx++)
        {
            dst_row[dx] = src_row[sx >> 16];
            sx += x_step;
        }
        sy += y_step;
    }

    lv_obj_invalidate(s_preview_img);
}

void ui_face_set_status(int kind, const char *title, const char *detail)
{
    uint32_t bg;
    uint16_t bg565;
    bool has_detail;

    if (s_status_box == NULL)
    {
        return;
    }

    bg = status_bg_color(kind);
    bg565 = UI_RGB565(bg);

    lv_obj_set_style_bg_color(s_status_box, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(s_status_box, lv_color_hex(status_border_color(kind)), 0);

    /* 标题 16 号字模放大 2 倍（32 像素高），细节文字保持 16 像素 */
    ui_text_set_scaled(&s_status_title, title, UI_COLOR_WHITE, bg565, 2);
    ui_text_set(&s_status_detail, detail, UI_RGB565(COL_TEXT), bg565);

    /* 有细节文字时标题靠上，否则标题在横幅内垂直居中 */
    has_detail = (detail != NULL && detail[0] != '\0');
    if (s_status_title.img != NULL)
    {
        lv_obj_set_pos(s_status_title.img, (STATUS_BOX_W - STATUS_TXT_W) / 2,
                       has_detail ? STATUS_TITLE_Y
                                  : (STATUS_BOX_H - STATUS_TITLE_H) / 2);
    }
    if (s_status_detail.img != NULL)
    {
        if (has_detail)
        {
            lv_obj_clear_flag(s_status_detail.img, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_status_detail.img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_has_status = true;
    lv_obj_clear_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_status_box);
}

void ui_face_clear_status(void)
{
    if (s_status_box == NULL)
    {
        return;
    }

    s_has_status = false;
    lv_obj_add_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
}

void ui_face_set_login_cb(void (*cb)(void))
{
    s_login_cb = cb;
}

void ui_face_set_recognize_cb(void (*cb)(void))
{
    s_recog_cb = cb;
}
