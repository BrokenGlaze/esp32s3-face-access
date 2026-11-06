/**
 * @file    lvgl_ui.c
 * @brief   LVGL 端口初始化 + 界面切换的门面（facade）
 *
 * @note    各界面实现在同目录下：
 *            ui_face.c    人脸识别界面（摄像头预览 + 状态横幅 + 登入/识别按钮）
 *            ui_login.c   登入界面（标题 + 密码掩码 + 提示 + 退出 + 数字键盘）
 *            ui_admin.c   管理员配置界面
 *            ui_face_db.c 人脸信息库界面
 *            ui_history.c 识别历史记录界面
 *          ui_common.c 提供公共绘制原语（图像、点阵文字、按钮、遮罩）。
 *          本文件只负责：LVGL 端口/显示/触摸注册、创建各界面、对外接口加锁转发。
 */

#include "lvgl_ui.h"

#include <stdio.h>

#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "tp.h"
#include "ui_admin.h"
#include "ui_face.h"
#include "ui_face_db.h"
#include "ui_history.h"
#include "ui_login.h"
#include "ui_wifi.h"
#include "ui_common.h"

/* ==================== 待机（10 秒无触摸自动熄屏） ====================
 * 待机检测不额外创建任务：用 LVGL 定时器（跑在 LVGL 任务上下文）轮询触摸快照，
 * 上层人脸识别流水线通过 lvgl_ui_standby_active() 判断并暂停采集/AI/显示。
 *   - 轮询周期 30ms；读取的只是 tp_click/tp_snapshot 维护的内存快照，无 I2C 开销
 *   - 进入待机：关闭背光并禁用 LVGL 输入，避免唤醒的那一次触摸误触按钮
 *   - 唤醒：点亮背光，等手指抬起后再恢复 LVGL 输入
 */
#define STANDBY_IDLE_TIMEOUT_MS 10000 /* 无触摸进入待机的超时（10 秒） */
#define STANDBY_POLL_PERIOD_MS  30    /* 触摸轮询周期（毫秒） */

static volatile bool s_standby_active = false; /* 当前是否处于待机 */
static bool s_touch_hold_off = false;          /* 唤醒后丢弃“唤醒这一按”，等手指真正抬起 */
static uint32_t s_standby_last_active = 0;     /* 最近一次触摸的 LVGL 时间戳 */

/* ==================== 触摸输入设备（读采样任务发布的快照） ====================
 * 不再用 lvgl_port_add_touch()：它的 indev 回调直接调用 esp_lcd_touch_read_data()，
 * 而该路径的 I2C 超时被硬编码为 -1（无限等待），FT6x36 NACK 时会把 LVGL 任务锁死；
 * 且它每 30ms 才采样一次，遇到预览界面的连续整帧重绘会整段跳过，快速点击易丢。
 *
 * 这里改为读 tp.c 中独立采样任务（20ms、带超时直读）维护的快照：
 *   - 当前按下：直接上报 PRESSED + 实时坐标
 *   - 手指已抬起但点击未被采到：用粘滞点击合成一次 PRESSED，下一轮再报 RELEASED，
 *     LVGL 由此正常产生 LV_EVENT_CLICKED，彻底消除“时灵时不灵”
 */
#define TOUCH_SYNTH_NONE   0 /* 当前没有合成序列 */
#define TOUCH_SYNTH_PRESS  1 /* 已合成按下，下一轮需补报抬起 */

static uint8_t s_touch_synth = TOUCH_SYNTH_NONE;
static bool s_touch_reported_press = false; /* 上一次是否已向 LVGL 上报真实按下 */
static uint16_t s_touch_last_x = 0;         /* 最近上报的坐标（抬起时沿用） */
static uint16_t s_touch_last_y = 0;

/* ==================== 输入分发诊断（定位“整屏点击偶发全无反应”） ====================
 * 每秒输出一行，用来区分三种完全不同的故障：
 *   gap_max  : 两次 read_cb 调用的最大间隔。远大于 30ms 说明 LVGL 任务被刷屏阻塞，
 *              输入分发本身极慢（此时“采样正常、点击无效”）
 *   calls    : 每秒 read_cb 实际调用次数 = 真实的输入轮询频率（60ms 周期即约 16 次）
 *   dead     : 按压坐标上没有按钮（落在背景 / 预览区），此时点不动是正常的
 *   hit_null : “按下”那一刻命中测试没有找到任何可点击对象 → 坐标/显示层不对
 *   press/rel: 上报的按下/抬起次数，与 tp.c 的 down/up 逐条对照即可看出哪一段丢事件
 */
#define IN_DIAG_LOG 1

static uint32_t s_in_last_us = 0;    /* 上一次 read_cb 的时刻 */
static uint32_t s_in_gap_max_ms = 0; /* 两次 read_cb 的最大间隔 */
static uint32_t s_in_report_us = 0;  /* 上一次统计输出时刻 */
static uint32_t s_in_calls = 0;      /* 每秒 read_cb 调用次数 */
static uint32_t s_in_press = 0;      /* 每秒 PRESSED 上报次数 */
static uint32_t s_in_rel = 0;        /* 每秒 RELEASED 上报次数 */
static uint32_t s_in_dead = 0;       /* 每秒“按压不在按钮上”的次数 */
static uint32_t s_in_hit_null = 0;   /* 每秒命中测试为空的次数 */
static bool s_indev_enabled = true;  /* 软件侧记录的 LVGL 输入使能状态 */

/**
 * @brief 与 LVGL 内核一致的三层命中测试（sys / top / scr），仅诊断用
 * @return 命中的可点击对象，NULL 表示该点没有任何可点击对象
 */
static lv_obj_t *diag_hit_test(uint16_t x, uint16_t y)
{
    lv_point_t p = {.x = (lv_coord_t)x, .y = (lv_coord_t)y};
    lv_obj_t *o = lv_indev_search_obj(lv_layer_sys(), &p);
    if (o == NULL)
    {
        o = lv_indev_search_obj(lv_layer_top(), &p);
    }
    if (o == NULL)
    {
        o = lv_indev_search_obj(lv_scr_act(), &p);
    }
    return o;
}

/**
 * @brief 按压落点诊断：区分“命中按钮 / 命中背景 / 什么都没命中”
 * @param x,y  上报坐标
 * @param synth true 表示这是补发的合成按下（真实采样漏掉了这一次）
 */
static void in_diag_press_point(uint16_t x, uint16_t y, bool synth)
{
    lv_obj_t *hit = diag_hit_test(x, y);

    if (ui_obj_is_btn(hit))
    {
        return; /* 落在按钮上：后面一定有 [BTN] down 打印 */
    }

    if (hit == NULL)
    {
        s_in_hit_null++;
    }
    else
    {
        s_in_dead++;
    }

#if IN_DIAG_LOG
    if (hit == NULL)
    {
        printf("[IN] %spress (%u,%u) 命中 NULL（无可点击对象）\n",
               synth ? "synth " : "", (unsigned)x, (unsigned)y);
    }
    else
    {
        lv_area_t a;

        lv_obj_get_coords(hit, &a);
        printf("[IN] %spress (%u,%u) 未落按钮，命中 area=(%d,%d)-(%d,%d)%s\n",
               synth ? "synth " : "", (unsigned)x, (unsigned)y,
               (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
               (hit == lv_scr_act()) ? "（屏幕背景）" : "");
    }
#endif
}

/**
 * @brief 输入分发侧的每秒统计输出（只在 read_cb 里调用，位于 LVGL 任务上下文）
 */
static void in_diag_report(uint32_t now_us)
{
#if IN_DIAG_LOG
    if (s_in_report_us == 0)
    {
        s_in_report_us = now_us;
        return;
    }
    if (now_us - s_in_report_us < 1000000)
    {
        return;
    }

    printf("[IN] 1s calls=%lu gap_max=%lums press=%lu rel=%lu dead=%lu hit_null=%lu indev=%d\n",
           (unsigned long)s_in_calls, (unsigned long)s_in_gap_max_ms,
           (unsigned long)s_in_press, (unsigned long)s_in_rel,
           (unsigned long)s_in_dead, (unsigned long)s_in_hit_null,
           s_indev_enabled ? 1 : 0);

    s_in_report_us = now_us;
    s_in_calls = 0;
    s_in_gap_max_ms = 0;
    s_in_press = 0;
    s_in_rel = 0;
    s_in_dead = 0;
    s_in_hit_null = 0;
#else
    (void)now_us;
#endif
}

/**
 * @brief LVGL 触摸输入回调：把采样任务的快照翻译成 LVGL 事件
 * @param drv 输入设备驱动（未使用）
 * @param data 待填充的输入数据
 * @note 全程只读内存，不做任何 I2C 操作，耗时恒定，不会被刷屏拖慢
 *
 * LVGL 只在“采样到按下”且“采样到抬起”时才产生 LV_EVENT_CLICKED：
 *   - 真实按下：每轮上报 PRESSED，并在抬起的那一轮上报 RELEASED（正常路径）
 *   - 按下->抬起都发生在两次 LVGL 采样之间（被刷屏挤掉）：采样任务的粘滞点击
 *     会让这里合成一次 PRESSED、下一轮再补 RELEASED，事件不再丢失
 */
static void touch_indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x = 0;
    uint16_t y = 0;

    (void)drv;

#if IN_DIAG_LOG
    /* 调用间隔：>40ms 说明 LVGL 任务本身被刷屏/锁拖慢，输入分发跟不上手指 */
    uint32_t now_us = esp_timer_get_time();
    s_in_calls++;
    if (s_in_last_us != 0)
    {
        uint32_t gap_ms = (uint32_t)((now_us - s_in_last_us) / 1000);
        if (gap_ms > s_in_gap_max_ms)
        {
            s_in_gap_max_ms = gap_ms;
        }
    }
    s_in_last_us = now_us;
    in_diag_report(now_us);
#endif

    /* 0) 刚被唤醒：这一按只用于唤醒，一律按“抬起”上报，
          等手指真正离开后再恢复正常点击（不再依赖“等抬起才恢复输入”） */
    if (s_touch_hold_off)
    {
        tp_click_take(NULL, NULL);
        s_touch_reported_press = false;
        s_touch_synth = TOUCH_SYNTH_NONE;
        s_in_rel++;
        data->point.x = s_touch_last_x;
        data->point.y = s_touch_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* 1) 真实按下：上报实时坐标 */
    if (tp_snapshot_pressed(&x, &y))
    {
        if (!s_touch_reported_press)
        {
            /* 只在“按下开始”那一刻做命中测试，确认这个坐标上真的有按钮 */
            s_in_press++;
            in_diag_press_point(x, y, false);
        }
        s_touch_reported_press = true;
        s_touch_synth = TOUCH_SYNTH_NONE; /* 真实按下优先 */
        tp_click_take(NULL, NULL);        /* 丢弃同一按下的粘滞标志，避免重复触发 */
        s_touch_last_x = x;
        s_touch_last_y = y;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    /* 2) 真实按下刚结束：LVGL 已见过按下，这里只需补一次抬起 */
    if (s_touch_reported_press)
    {
        s_touch_reported_press = false;
        tp_click_take(NULL, NULL); /* 该次点击已由真实采样上报，丢弃粘滞标志 */
        s_in_rel++;
        data->point.x = s_touch_last_x;
        data->point.y = s_touch_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* 3) 上一轮合成过按下，本轮补报抬起 */
    if (s_touch_synth == TOUCH_SYNTH_PRESS)
    {
        s_touch_synth = TOUCH_SYNTH_NONE;
        s_in_rel++;
        data->point.x = s_touch_last_x;
        data->point.y = s_touch_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* 4) 手指已抬起但点击从未被采样到：合成一次按下，下一轮再抬起 */
    if (tp_click_take(&x, &y))
    {
        /* 诊断：置位→被消费的延迟，可区分“触摸没被采到”与“LVGL 分发太慢” */
        uint32_t latch_us = tp_click_latch_us();
        if (latch_us != 0)
        {
            tp_note_click_latency((uint32_t)((esp_timer_get_time() - latch_us) / 1000));
        }

        s_in_press++;
        in_diag_press_point(x, y, true);

        s_touch_synth = TOUCH_SYNTH_PRESS;
        s_touch_last_x = x;
        s_touch_last_y = y;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    /* 5) 无触摸（空闲：不计数，否则每秒空转会把 rel 统计淹掉） */
    data->point.x = s_touch_last_x;
    data->point.y = s_touch_last_y;
    data->state = LV_INDEV_STATE_RELEASED;
}

/**
 * @brief 读取一次触摸屏是否被按下（待机检测用）
 * @return true 表示有手指按下
 * @note 读的是采样任务发布的快照，无 I2C 事务；待机时 LVGL 输入已禁用，
 *       但快照仍在由独立任务持续更新，因此仍能可靠唤醒
 */
static bool standby_touch_pressed(void)
{
    return tp_snapshot_pressed(NULL, NULL);
}

/**
 * @brief 待机检测定时器回调：轮询触摸，超时熄屏，触摸唤醒
 * @param timer LVGL 定时器句柄（未使用）
 */
static void standby_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint32_t now = lv_tick_get();
    bool touching = standby_touch_pressed();

    /* 待机期间的触摸只用于唤醒，不产生点击：
       采样任务的粘滞点击必须在此清空，否则唤醒后会被合成成一次
       落在手指位置上的“按下→抬起”，误触该处的按钮 */
    if (s_standby_active)
    {
        tp_click_take(NULL, NULL);
    }

    if (touching)
    {
        s_standby_last_active = now;

        if (s_standby_active)
        {
            s_standby_active = false;

            /* 立即恢复输入，而不是等“手指抬起”这个不可靠的事件：
               旧逻辑一旦快照卡在“按下”，lv_indev_enable(true) 永远执行不到，
               整屏点击会彻底失效（待机定时器却仍正常，正是你的现象）。
               唤醒这一按由 read_cb 的 s_touch_hold_off 分支丢弃，不会误触。 */
            s_touch_hold_off = true;
            tp_click_take(NULL, NULL); /* 丢弃唤醒这一按的粘滞点击 */
            s_indev_enabled = true;
            lv_indev_enable(NULL, true);
            lcd_backlight_set(1);
            printf("standby: wake up\n");
        }
    }
    else
    {
        /* 唤醒手指已抬起：解除“丢弃态”，之后恢复正常点击 */
        if (s_touch_hold_off)
        {
            s_touch_hold_off = false;
            tp_click_take(NULL, NULL); /* 丢弃唤醒那一按的粘滞点击 */
        }

        /* 自愈：不在待机且输入却是禁用状态，说明被意外禁用，强制恢复 */
        if (!s_standby_active && !s_indev_enabled)
        {
            s_indev_enabled = true;
            lv_indev_enable(NULL, true);
            printf("standby: indev watchdog re-enable\n");
        }

        /* 超时无触摸：进入待机 */
        if (!s_standby_active && (now - s_standby_last_active) >= STANDBY_IDLE_TIMEOUT_MS)
        {
            s_standby_active = true;
            s_indev_enabled = false;
            lv_indev_enable(NULL, false);
            lcd_backlight_set(0);
            printf("standby: enter\n");
        }
    }
}

esp_err_t lvgl_ui_init(void)
{
    esp_err_t ret = ESP_OK;

    /* ---- 1. 初始化 LVGL 端口 ---- */
    /**
     * LVGL 任务默认优先级 4 且不绑核，而摄像头/AI/显示三个任务都是优先级 5，
     * 预览界面运行时 AI 推理与整帧预览重绘长期占满 CPU，LVGL 任务被持续抢占，
     * 触摸读取（默认 20~30ms 一次）经常整段被跳过，导致“登入/识别”按钮点不动。
     *
     * 这里改为：优先级 6（高于三个流水线任务）+ 绑核 1。
     * 选核 1 是因为摄像头任务在 core1，其余时间都阻塞在 fb_get 等帧，
     * 而 AI 与显示任务在 core0，绑核 1 不会拖慢推理；SPI 刷屏阻塞时也会让出 CPU。
     */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 6;
    port_cfg.task_affinity = 1;

    ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK)
    {
        printf("lvgl_port_init failed, ret = %d\n", ret);
        return ret;
    }

    /* 绘制缓冲必须在内部 RAM（SPI DMA 不能直接读 PSRAM）：480x8 双缓冲约 15KB。
       行数不宜再大：内部 RAM 总量只有约 46KB，还要容纳 AI 任务 8KB 栈、
       摄像头/LCD/按键任务栈，以及人脸检测与识别模型约 28KB 的张量，
       16 行双缓冲要 30KB，会挤掉 LCD 任务导致其创建失败（登录页切换失效）。
       8 行的代价只是每次刷屏行数更少、块刷新次数略多，肉眼无差异 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_get_io_handle(),
        .panel_handle = lcd_get_panel_handle(),
        .buffer_size = LCD_WIDTH * 8,
        .double_buffer = true,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .monochrome = false,
        .rotation = {.swap_xy = LCD_SWAP_XY, .mirror_x = LCD_MIRROR_X, .mirror_y = LCD_MIRROR_Y},
        .flags = {.buff_dma = true, .buff_spiram = false},
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL)
    {
        printf("lvgl_port_add_disp failed\n");
        return ESP_FAIL;
    }

    /* ---- 2. 创建界面 ---- */
    lvgl_port_lock(0);

    /* 注册触摸输入设备：读 tp.c 采样任务维护的快照，不做 I2C、不会被刷屏拖慢 */
    {
        static lv_indev_drv_t indev_drv;

        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = touch_indev_read_cb;

        if (lv_indev_drv_register(&indev_drv) == NULL)
        {
            printf("touch indev register failed\n");
        }
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_face_create(scr);    /* 人脸识别界面（默认显示） */
    ui_login_create(scr);   /* 登入界面（默认隐藏） */
    ui_admin_create(scr);   /* 管理员配置界面（默认隐藏） */
    ui_face_db_create(scr); /* 人脸信息库界面（默认隐藏） */
    ui_history_create(scr); /* 识别历史记录界面（默认隐藏，最后创建以保证在最上层） */
    ui_wifi_create(scr);    /* 网络设置界面（默认隐藏，位于最上层） */

    ui_login_hide();
    ui_admin_hide();
    ui_face_db_hide();
    ui_history_hide();
    ui_wifi_hide();
    ui_face_show();

    /* ---- 3. 启动待机检测定时器（10 秒无触摸熄屏，触摸唤醒） ---- */
    s_standby_last_active = lv_tick_get();
    if (lv_timer_create(standby_timer_cb, STANDBY_POLL_PERIOD_MS, NULL) == NULL)
    {
        printf("standby timer create failed\n");
    }

    lvgl_port_unlock();

    printf("lvgl_ui ready: %dx%d\n", LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

void lvgl_ui_set_login_cb(void (*cb)(void))
{
    ui_face_set_login_cb(cb);
}

void lvgl_ui_set_recognize_cb(void (*cb)(void))
{
    ui_face_set_recognize_cb(cb);
}

void lvgl_ui_set_enroll_cb(void (*cb)(void))
{
    ui_admin_set_enroll_cb(cb);
}

void lvgl_ui_set_exit_cb(void (*cb)(void))
{
    ui_login_set_exit_cb(cb);
}

void lvgl_ui_preview_frame(const uint16_t *src, uint32_t src_w, uint32_t src_h)
{
    lvgl_port_lock(0);
    ui_face_frame(src, src_w, src_h);
    lvgl_port_unlock();
}

void lvgl_ui_set_status(int kind, const char *title, const char *detail)
{
    lvgl_port_lock(0);
    ui_face_set_status(kind, title, detail);
    lvgl_port_unlock();
}

void lvgl_ui_clear_status(void)
{
    lvgl_port_lock(0);
    ui_face_clear_status();
    lvgl_port_unlock();
}

void lvgl_ui_admin_toast(const char *text)
{
    lvgl_port_lock(0);
    ui_admin_show_tip(text);
    lvgl_port_unlock();
}

void lvgl_ui_show_login(void)
{
    lvgl_port_lock(0);
    ui_face_hide();
    ui_login_show();
    lvgl_port_unlock();
}

void lvgl_ui_show_preview(void)
{
    lvgl_port_lock(0);
    ui_login_hide();
    ui_admin_hide();
    ui_face_db_hide();
    ui_history_hide();
    ui_wifi_hide();
    ui_face_show();
    lvgl_port_unlock();
}

void lvgl_ui_show_camera_only(void)
{
    lvgl_port_lock(0);
    ui_login_hide();
    ui_admin_hide();
    ui_face_db_hide();
    ui_history_hide();
    ui_wifi_hide();
    ui_face_show_camera_only();
    lvgl_port_unlock();
}

void lvgl_ui_show_admin(void)
{
    lvgl_port_lock(0);
    ui_login_hide();
    ui_face_hide();
    ui_face_db_hide();
    ui_history_hide();
    ui_wifi_hide();
    ui_admin_show();
    lvgl_port_unlock();
}

bool lvgl_ui_admin_active(void)
{
    return ui_admin_is_visible();
}

bool lvgl_ui_standby_active(void)
{
    return s_standby_active;
}
