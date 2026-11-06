/**
 * @file    ui_common.c
 * @brief   LVGL 界面公共组件实现
 */

#include "ui_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "lvgl_ui.h"

void *ui_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

lv_obj_t *ui_image_create(lv_obj_t *parent, lv_img_dsc_t *dsc, const void *buf,
                          uint32_t w, uint32_t h, lv_coord_t x, lv_coord_t y)
{
    dsc->header.always_zero = 0;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = w * h * 2;
    dsc->data = (const uint8_t *)buf;

    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);  /* 图像默认不拦截触摸 */
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);

    return img;
}

uint32_t ui_text_width(const char *text, uint32_t font)
{
    uint32_t w = 0;
    size_t i = 0;
    size_t len = strlen(text);

    while (i < len)
    {
        if ((unsigned char)text[i] > 0x7F)
        {
            w += font;
            i += 3;
        }
        else
        {
            w += font / 2;
            i += 1;
        }
    }
    return w;
}

void ui_text_init(ui_text_t *t, lv_obj_t *parent, uint32_t w, uint32_t h,
                  lv_coord_t x, lv_coord_t y)
{
    t->w = w;
    t->h = h;
    t->buf = (uint16_t *)ui_alloc(w * h * sizeof(uint16_t));
    if (t->buf == NULL)
    {
        t->img = NULL;
        return;
    }
    memset(t->buf, 0, w * h * sizeof(uint16_t));
    t->img = ui_image_create(parent, &t->dsc, t->buf, w, h, x, y);
}

void ui_text_set(ui_text_t *t, const char *text, uint16_t color, uint16_t bg)
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
        uint32_t tw = ui_text_width(text, UI_INFO_FONT);
        uint32_t x = (tw < t->w) ? (t->w - tw) / 2 : 0;
        uint32_t y = (UI_INFO_FONT < t->h) ? (t->h - UI_INFO_FONT) / 2 : 0;

        lcd_render_string_to_buf(t->buf, t->w, t->h, x, y, color, bg, UI_INFO_FONT, text);
    }

    lv_obj_invalidate(t->img);
}

void ui_text_set_scaled(ui_text_t *t, const char *text, uint16_t color,
                        uint16_t bg, uint8_t scale)
{
    uint16_t *tmp;
    uint16_t bg_swap;
    uint32_t sw, sh, i, j, dx, dy;

    if (t->img == NULL || t->buf == NULL || scale == 0)
    {
        return;
    }

    /* 小图尺寸 = 目标尺寸 / 放大倍数；按小图排版后再逐像素放大 */
    sw = t->w / scale;
    sh = t->h / scale;
    if (sw == 0 || sh == 0)
    {
        return;
    }

    tmp = (uint16_t *)ui_alloc(sw * sh * sizeof(uint16_t));
    if (tmp == NULL)
    {
        return;
    }

    bg_swap = (uint16_t)((bg >> 8) | (bg << 8));
    for (i = 0; i < sw * sh; i++)
    {
        tmp[i] = bg_swap;
    }

    if (text != NULL && text[0] != '\0')
    {
        uint32_t tw = ui_text_width(text, UI_INFO_FONT);
        uint32_t x = (tw < sw) ? (sw - tw) / 2 : 0;
        uint32_t y = (UI_INFO_FONT < sh) ? (sh - UI_INFO_FONT) / 2 : 0;

        lcd_render_string_to_buf(tmp, sw, sh, x, y, color, bg, UI_INFO_FONT, text);
    }

    for (j = 0; j < sh; j++)
    {
        for (i = 0; i < sw; i++)
        {
            uint16_t px = tmp[j * sw + i];
            for (dy = 0; dy < scale; dy++)
            {
                for (dx = 0; dx < scale; dx++)
                {
                    t->buf[(j * scale + dy) * t->w + i * scale + dx] = px;
                }
            }
        }
    }

    free(tmp);
    lv_obj_invalidate(t->img);
}

void ui_render_lines(uint16_t *buf, uint32_t w, uint32_t h,
                     const char *line1, const char *line2)
{
    /* 黑底：RGB565 0x0000 交换后仍为 0 */
    memset(buf, 0, w * h * sizeof(uint16_t));

    if (line1 != NULL && line1[0] != '\0')
    {
        lcd_render_string_to_buf(buf, w, h, 0, 0, 0xFFFF, 0x0000, UI_INFO_FONT, line1);
    }
    if (line2 != NULL && line2[0] != '\0')
    {
        lcd_render_string_to_buf(buf, w, h, 0, UI_INFO_FONT, 0xFFFF, 0x0000, UI_INFO_FONT, line2);
    }
}

/**
 * @brief 按钮事件诊断：打印“按下 / 点击”是否真正到达按钮
 * @note  与 lvgl_ui.c 的 [IN] 日志对照，可定位事件丢在“采样 / 分发 / 命中 / 按钮”哪一段
 */
static void btn_diag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;

    lv_obj_get_coords(obj, &a);
    printf("[BTN] %s area=(%d,%d)-(%d,%d)\n",
           (code == LV_EVENT_PRESSED) ? "down" : "clicked",
           (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
}

/**
 * @brief 按钮标记：写进对象的 user_data，用于命中测试时识别“这是按钮”
 */
static const uint8_t s_btn_marker = 0x5A;

bool ui_obj_is_btn(const void *obj)
{
    return (obj != NULL) &&
           (lv_obj_get_user_data((lv_obj_t *)obj) == (void *)&s_btn_marker);
}

/**
 * @brief 创建一个可点击的普通对象（按钮基底）
 */
lv_obj_t *ui_btn_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, uint32_t bg,
                        lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg + 0x202020), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(btn, (void *)&s_btn_marker);

    lv_obj_add_event_cb(btn, btn_diag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, btn_diag_cb, LV_EVENT_CLICKED, NULL);

    if (cb != NULL)
    {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    return btn;
}

lv_obj_t *ui_mask_create(lv_obj_t *parent, lv_opa_t opa, lv_event_cb_t cb)
{
    lv_obj_t *mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, opa, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    if (cb != NULL)
    {
        lv_obj_add_event_cb(mask, cb, LV_EVENT_CLICKED, NULL);
    }
    return mask;
}
