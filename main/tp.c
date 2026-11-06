/**
 * @file    tp.c
 * @brief   触摸屏驱动封装：负责 FT6x36 触摸屏的初始化（创建 I2C 面板 IO 与触摸设备实例），
 *          并提供获取触摸设备句柄的接口，供上层读取触摸坐标使用。
 *
 * @note    使用说明：
 *          - 依赖组件：esp_lcd_touch、esp_lcd_touch_ft6x36（managed_components 自动拉取）
 *          - 前置条件：必须先调用 i2c_init() 完成 I2C 总线初始化，再调用 tp_init()
 *          - 屏幕分辨率配置：x_max=320、y_max=480，需与实际屏幕硬件一致
 *          - 触摸设备地址 0x38（FT6x36），通过宏 ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG()
 * 快速获取默认配置
 *          - 未使用硬件复位/中断引脚（rst_gpio_num=-1, int_gpio_num=-1），采用轮询方式读取
 *          - 调用流程：tp_init() -> tp_get_handle() -> esp_lcd_touch_read_data() /
 * esp_lcd_touch_get_coordinates()
 */

#include "tp.h"                // 自定义触摸屏头文件，提供 tp_init、tp_get_handle 函数声明
#include "esp_timer.h"         // 诊断统计使用高精度计时
#include "lcd.h"               // 使用 LCD_WIDTH / LCD_HEIGHT 计算触摸坐标映射
#include "driver/gpio.h"       // GPIO 驱动头文件，提供 gpio 的函数接口
#include "driver/i2c_master.h" // 新版 I2C 主机驱动（采样任务使用带超时的直读）
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "i2c.h"               // 自定义 I2C 头文件，提供 i2c_init 函数声明
#include <stdio.h>             // 标准输入输出头文件，提供 printf 等函数

// ==================== 触摸采样任务参数 ====================
#define TP_SAMPLE_PERIOD_MS 20   // 采样周期：20ms 一次，兼顾响应速度与 I2C 占用
#define TP_SAMPLE_TIMEOUT_MS 20  // 单次 I2C 事务超时：总线异常时最多阻塞 20ms
#define TP_SAMPLE_TASK_PRIO 7    // 高于 LVGL 任务（6），保证采样不被刷屏挤掉
#define TP_SAMPLE_TASK_CORE 1    // 与 LVGL 任务同核，避免跨核唤醒延迟
#define TP_SAMPLE_STACK 3072     // 任务栈（新版 I2C 驱动 + 日志）
#define TP_I2C_ADDR 0x38         // FT6x36 7 位从机地址

/**
 * @brief 卡死自愈阈值：连续报告“有触点”超过该时长即视为控制器卡死
 *
 * @note  FT6x36 偶发残留触点时，快照会一直停在“按下”，
 *        导致 LVGL 侧永远等不到“抬起”来恢复输入、待机计时也永不超时。
 *        真实使用不可能连续按压 20 秒，超过就强制发布一次抬起（不产生点击）。
 */
#define TP_STUCK_PRESS_MS 20000
#define TP_STUCK_MOVE_PX 8       /* 卡死期间坐标变化超过该像素即认为是真实的新触摸 */

#define TP_REG_READ_DATA 0x00  // 坐标数据起始寄存器（0x03..0x06 为第 1 触点 X/Y）
#define TP_REG_NUM_TOUCH 0x02  // 触点数量寄存器（低 4 位有效）
#define TP_READ_LEN 8          // 只需读到 0x07，多读 1 字节便于对齐，减少总线占用

// 全局变量定义，用于存储各种硬件设备的句柄
static esp_lcd_touch_handle_t g_ft6x36_handle =
    NULL; // FT6x36触摸屏设备句柄（静态全局，仅本文件内部访问）

/**
 * @var s_tp_dev
 * @brief 采样任务专用的 FT6x36 I2C 设备句柄（与触摸驱动同地址，总线锁自动串行化）
 */
static i2c_master_dev_handle_t s_tp_dev = NULL;

/**
 * @var s_tp_lock
 * @brief 保护“坐标 + 按下状态”成对读取的自旋锁（临界区内不做任何 I2C 操作）
 */
static portMUX_TYPE s_tp_lock = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_tp_pressed = false;       /* 当前是否被按下 */
static volatile bool s_tp_click_pending = false; /* 粘滞标志：发生过一次完整点击，等待取走 */
static volatile uint16_t s_tp_x = 0;             /* 最近坐标 X */
static volatile uint16_t s_tp_y = 0;             /* 最近坐标 Y */
static volatile uint16_t s_tp_click_x = 0;       /* 点击坐标 X（按下时的位置） */
static volatile uint16_t s_tp_click_y = 0;       /* 点击坐标 Y */

/* 前向声明：采样任务实现在文件末尾，tp_init() 中创建 */
static void tp_sample_task(void *arg);

/* ==================== 诊断统计（定位“按键时灵时不灵”的成因） ====================
 * 每秒打印一行，用于区分两类故障：
 *   1) 采样任务被饿死（CPU 争用）：max_gap 明显大于 20ms
 *   2) 触摸控制器压根没报触摸（阈值/耦合问题）：press_edges 为 0 或极少
 *   3) LVGL 来不及消费（刷屏拖慢输入分发）：latch_latency_max 很大
 */
#define TP_DIAG_LOG 1 /* 置 0 可关闭诊断日志 */

static volatile uint32_t s_tp_click_us = 0;        /* 粘滞点击的置位时刻（us） */
static volatile uint32_t s_tp_latency_max_ms = 0;  /* 点击置位→被 LVGL 取走的最大延迟 */
static uint32_t s_tp_diag_gap_max_ms = 0;          /* 采样循环的最大间隔 */
static uint32_t s_tp_diag_press_edges = 0;         /* 每秒“无→有”的按下沿数量 */
static uint32_t s_tp_diag_i2c_fail = 0;            /* 每秒 I2C 读取失败次数 */

/**
 * @brief 记录一次“粘滞点击被取走”的延迟（由 LVGL 输入回调调用）
 * @param latency_ms 点击置位到被消费的毫秒数
 */
void tp_note_click_latency(uint32_t latency_ms)
{
    if (latency_ms > s_tp_latency_max_ms)
    {
        s_tp_latency_max_ms = latency_ms;
    }
}

/**
 * @brief 读取最近一次粘滞点击的置位时刻（us），供输入回调计算延迟
 */
uint32_t tp_click_latch_us(void)
{
    return s_tp_click_us;
}

/**
 * @brief Touchscreen 初始化函数：初始化触摸屏设备
 */
esp_err_t tp_init(void) // 触摸屏初始化函数：返回 ESP_OK 表示成功
{
    esp_lcd_panel_io_handle_t io_handle =
        NULL; // 定义 I2C 面板 IO 句柄并初始化为空，用于管理 I2C 通信

    // 初始化触摸屏设备，核心要得到触摸屏的设备地址
    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG(); // 使用 FT6x36 默认宏配置 I2C 面板 IO（设备地址
                                              // 0x38、8位寄存器寻址等）
    io_config.scl_speed_hz =
        I2C_FREQ_HZ; // 宏未配置该字段（默认 0），新版驱动要求 scl_speed_hz>0，
                     // 必须显式指定 SCL 频率（100kHz），否则报 invalid scl frequency

    // 初始化触摸屏设备
    esp_lcd_touch_config_t tp_cfg = {
        // 定义触摸屏配置结构体，设置触摸屏参数
#if TP_NATIVE_TO_DISPLAY
        /* 让驱动直接输出横屏显示坐标（LVGL 端口直接取用）。
         * 注意 esp_lcd_touch 内部顺序是“先 mirror 再 swap”，且 mirror 用
         * x_max/y_max 参与运算，所以这里要填原始竖屏坐标的最大索引：
         *   x_max = 319（原始竖屏宽 320）、y_max = 479（原始竖屏高 480）
         * 最终结果：x = 479 - ty、y = tx（触摸 X 轴与显示 Y 轴同向，故不镜像 X）。 */
        .x_max = LCD_HEIGHT - 1, // 319：原始竖屏 X 方向最大索引
        .y_max = LCD_WIDTH - 1,  // 479：原始竖屏 Y 方向最大索引
#else
        .x_max = 320, // 触摸屏最大 X 坐标（水平分辨率）320
        .y_max = 480, // 触摸屏最大 Y 坐标（垂直分辨率）480
#endif
        .rst_gpio_num = -1, // 不使用复位引脚（-1 表示未连接），依赖上电默认状态
        .int_gpio_num = -1, // 不使用中断引脚（-1 表示未连接），采用轮询方式读取
        .levels =           // 配置复位与中断引脚的有效电平
        {
            .reset = 0,     // 复位引脚低电平有效（未接线，仅作默认配置）
            .interrupt = 0, // 中断引脚低电平有效（未接线，仅作默认配置）
        },
        .flags = // 配置触摸坐标的坐标变换标志位
        {
#if TP_NATIVE_TO_DISPLAY
            .swap_xy = 1,  // 交换 X/Y 坐标轴（竖屏 -> 横屏）
            .mirror_x = 0, // 触摸 X 轴与显示 Y 轴同向，不能镜像（否则上下颠倒）
            .mirror_y = 1, // 镜像 Y 轴（与 LCD_MIRROR_Y 保持一致）
#else
            .swap_xy = 0,  // 不交换 X/Y 坐标轴
            .mirror_x = 0, // 不镜像 X 轴
            .mirror_y = 0, // 不镜像 Y 轴
#endif
        },
    };

    esp_err_t ret = ESP_OK; // 定义错误码变量并初始化为成功，用于保存各步骤返回值

#if (I2C_VERSION == I2C_VERSION_1)
    ret = esp_lcd_new_panel_io_i2c(I2C_NUM, &io_config, &io_handle);
#else
    ret = esp_lcd_new_panel_io_i2c_v2(i2c_get_bus_handle(), &io_config, &io_handle);
#endif
    if (ret != ESP_OK) // 判断面板 IO 创建是否成功
    {
        printf("esp_lcd_new_panel_io_i2c failed, ret = %d\n",
               ret); // 创建失败时打印错误信息及错误码
        return ret;  // 失败则返回错误码，终止初始化
    }

    ret = esp_lcd_touch_new_i2c_ft6x36(
        io_handle,
        &tp_cfg,
        &g_ft6x36_handle); // 基于面板 IO 创建 FT6x36 触摸设备实例，保存句柄到全局变量
    if (ret != ESP_OK)     // 判断触摸设备创建是否成功
    {
        printf("esp_lcd_touch_new_i2c_ft6x36 failed, ret = %d\n",
               ret); // 创建失败时打印错误信息及错误码
        return ret;  // 失败则返回错误码，终止初始化
    }

    /* ---- 为采样任务注册一个独立的 FT6x36 设备句柄 ----
       同一条总线上允许多个设备句柄指向同一地址，驱动内部的总线锁会串行化事务。
       之所以不复用 esp_lcd_touch 的读取路径，是因为它内部使用 -1（无限）超时。 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };

    ret = i2c_master_bus_add_device(i2c_get_bus_handle(), &dev_cfg, &s_tp_dev);
    if (ret != ESP_OK)
    {
        printf("tp_sample: i2c_master_bus_add_device failed, ret = %d\n", ret);
        s_tp_dev = NULL;
        return ret;
    }

    if (xTaskCreatePinnedToCore(tp_sample_task, "tp_sample", TP_SAMPLE_STACK, NULL,
                                TP_SAMPLE_TASK_PRIO, NULL,
                                TP_SAMPLE_TASK_CORE) != pdPASS)
    {
        printf("tp_sample: task create failed\n");
        return ESP_FAIL;
    }

    return ESP_OK; // 全部初始化成功，返回 ESP_OK
}

/**
 * @brief 触摸采样任务：独立于 LVGL 读取触摸控制器并发布快照
 *
 * @param arg 未使用
 *
 * @note  为什么必须独立成任务：
 *        1) esp_lcd_touch_read_data() 最终走 esp_lcd_panel_io_i2c_v2.c，
 *           该处把传输超时硬编码为 -1（portMAX_DELAY）。FT6x36 一旦 NACK，
 *           i2c_master.c 的 “while (i2c_ll_is_bus_busy())” 永不超时，
 *           调用它的任务会永久卡死（曾触发 LVGL 任务饿死 IDLE1 的 task_wdt）。
 *        2) LVGL 的输入设备在 lv_timer_handler() 里每 30ms 采样一次，
 *           而预览界面每帧要 30~40ms 逐块阻塞刷屏，采样经常整段被跳过，
 *           100ms 的快按可能一次都没采到，表现为“按钮时灵时不灵”。
 *        此处用带 20ms 超时的直读、20ms 周期无条件采样，并把“按下→抬起”
 *        记成粘滞标志 s_tp_click_pending，LVGL 无论晚到多久都能取到这次点击。
 */
static void tp_sample_task(void *arg)
{
    (void)arg;

    bool last_pressed = false;
    uint16_t last_x = 0;
    uint16_t last_y = 0;
    uint32_t last_loop_us = esp_timer_get_time(); /* 循环节拍基准（卡死自愈 + 诊断共用） */
    uint32_t press_run_ms = 0;                    /* 本次连续按下的累计时长 */
    bool stuck_latch = false;                     /* 已判定控制器残留触点：持续压制快照 */
    uint16_t stuck_x = 0;
    uint16_t stuck_y = 0;

#if TP_DIAG_LOG
    uint32_t diag_report_us = last_loop_us;
#endif

    while (true)
    {
        /* 采样循环的实际间隔：明显大于 20ms 说明本任务被其它任务抢占（CPU 争用） */
        uint32_t loop_us = esp_timer_get_time();
        uint32_t gap_ms = (uint32_t)((loop_us - last_loop_us) / 1000);
        last_loop_us = loop_us;
#if TP_DIAG_LOG
        if (gap_ms > s_tp_diag_gap_max_ms)
        {
            s_tp_diag_gap_max_ms = gap_ms;
        }
#endif

        uint8_t reg = TP_REG_NUM_TOUCH;
        uint8_t cnt = 0;
        uint8_t cnt_seen = 0; /* 诊断：本次读到的触点数量原始值 */
        bool pressed = false;
        bool stuck_release = false; /* 卡死自愈触发的假抬起：不发布粘滞点击 */
        uint16_t x = last_x;
        uint16_t y = last_y;
        uint16_t raw_x = 0; /* 诊断：控制器原始坐标 */
        uint16_t raw_y = 0;

        if (i2c_master_transmit_receive(s_tp_dev, &reg, 1, &cnt, 1,
                                        TP_SAMPLE_TIMEOUT_MS) == ESP_OK)
        {
            cnt_seen = cnt;
            pressed = (cnt & 0x0F) > 0;

            if (pressed)
            {
                uint8_t data[TP_READ_LEN] = {0};
                reg = TP_REG_READ_DATA;

                if (i2c_master_transmit_receive(s_tp_dev, &reg, 1, data, TP_READ_LEN,
                                                TP_SAMPLE_TIMEOUT_MS) == ESP_OK)
                {
                    /* 原始坐标：竖屏下 x 0..319、y 0..479 */
                    uint16_t rx = ((uint16_t)(data[0x03] & 0x0F) << 8) | data[0x04];
                    uint16_t ry = ((uint16_t)(data[0x05] & 0x0F) << 8) | data[0x06];
                    uint16_t dx;
                    uint16_t dy;

                    raw_x = rx; /* 诊断：保留控制器原始坐标 */
                    raw_y = ry;

#if TP_NATIVE_TO_DISPLAY
                    /* 必须与 tp_init 的 flags 完全一致（esp_lcd_touch 内部顺序：先 mirror 再 swap）
                     *   mirror_y: y = y_max - y = 479 - ry
                     *   swap_xy : x = y、y = x
                     * 结果：dx = 479 - ry、dy = rx */
                    dx = (uint16_t)((LCD_WIDTH - 1) - ry);
                    dy = rx;

                    x = (dx < LCD_WIDTH) ? dx : (LCD_WIDTH - 1);
                    y = (dy < LCD_HEIGHT) ? dy : (LCD_HEIGHT - 1);
#else
                    /* 未启用坐标变换：直接使用原始坐标 */
                    x = (rx < LCD_WIDTH) ? rx : (LCD_WIDTH - 1);
                    y = (ry < LCD_HEIGHT) ? ry : (LCD_HEIGHT - 1);
#endif
                }
                else
                {
                    /* 坐标读取失败：本轮沿用上一次坐标，不改变按下状态 */
                    pressed = last_pressed;
                }
            }
            else
            {
                x = last_x;
                y = last_y;
            }
        }
        else
        {
            /* 状态读取失败：本轮不做任何状态变更，等下一次采样 */
#if TP_DIAG_LOG
            s_tp_diag_i2c_fail++;
#endif
            vTaskDelay(pdMS_TO_TICKS(TP_SAMPLE_PERIOD_MS));
            continue;
        }

        /* ---- 卡死自愈：连续“按下”过久视为控制器残留触点 ----
           快照一旦永久停在 pressed，LVGL 输入将永远无法恢复到正常点击流程，
           待机计时也永不超时（屏幕常亮）。判定后持续把快照压成“抬起”，
           直到控制器真正报告抬起，或坐标明显移动（说明是新的真实触摸）。 */
        if (stuck_latch)
        {
            if (!pressed)
            {
                stuck_latch = false; /* 控制器真的抬起了 */
                press_run_ms = 0;
            }
            else if ((uint16_t)((x > stuck_x) ? (x - stuck_x) : (stuck_x - x)) > TP_STUCK_MOVE_PX ||
                     (uint16_t)((y > stuck_y) ? (y - stuck_y) : (stuck_y - y)) > TP_STUCK_MOVE_PX)
            {
                stuck_latch = false; /* 坐标换了位置：按真实的新触摸处理 */
                press_run_ms = 0;
            }
            else
            {
                pressed = false; /* 仍卡在同一位置：继续压制，且不产生点击 */
            }
        }
        else if (pressed)
        {
            press_run_ms += gap_ms;
            if (press_run_ms >= TP_STUCK_PRESS_MS)
            {
                printf("[TP] stuck pressed %lums at (%u,%u) -> suppress\n",
                       (unsigned long)press_run_ms, (unsigned)x, (unsigned)y);
                stuck_latch = true;
                stuck_x = x;
                stuck_y = y;
                press_run_ms = 0;
                pressed = false;
                stuck_release = true;
            }
        }
        else
        {
            press_run_ms = 0;
        }

        portENTER_CRITICAL(&s_tp_lock);
        if (pressed)
        {
            if (!s_tp_pressed)
            {
#if TP_DIAG_LOG
                s_tp_diag_press_edges++;
#endif
            }
            s_tp_pressed = true;
            s_tp_x = x;
            s_tp_y = y;
        }
        else if (last_pressed)
        {
            /* 抬起：发布粘滞点击，坐标取按下期间的最后位置 */
            s_tp_pressed = false;
            if (!stuck_release) /* 自愈的假抬起不发布点击，避免误触按钮 */
            {
                s_tp_click_pending = true;
                s_tp_click_x = x;
                s_tp_click_y = y;
#if TP_DIAG_LOG
                s_tp_click_us = esp_timer_get_time();
#endif
            }
        }
        else
        {
            s_tp_pressed = false;
        }
        portEXIT_CRITICAL(&s_tp_lock);

#if TP_DIAG_LOG
        /* 边沿日志：直接对比“控制器原始坐标”与“映射后显示坐标”，
           可一眼区分“控制器没报触摸”与“坐标映射/命中位置不对” */
        if (pressed && !last_pressed)
        {
            printf("[TP] down cnt=%u raw=(%u,%u) -> (%u,%u)\n",
                   (unsigned)cnt_seen, (unsigned)raw_x, (unsigned)raw_y,
                   (unsigned)x, (unsigned)y);
        }
        else if (!pressed && last_pressed)
        {
            printf("[TP] up (%u,%u)%s\n", (unsigned)x, (unsigned)y,
                   stuck_release ? " [stuck]" : "");
        }
#endif

        last_pressed = pressed;
        last_x = x;
        last_y = y;

#if TP_DIAG_LOG
        uint32_t now_us = esp_timer_get_time();
        if (now_us - diag_report_us >= 1000000)
        {
            /* 每秒输出一行，对比“人脸在画面内 / 不在画面内”两种工况即可定位成因 */
            printf("[TP] 1s gap_max=%lums press=%lu clicks_lat_max=%lums i2c_fail=%lu\n",
                   (unsigned long)s_tp_diag_gap_max_ms,
                   (unsigned long)s_tp_diag_press_edges,
                   (unsigned long)s_tp_latency_max_ms,
                   (unsigned long)s_tp_diag_i2c_fail);

            s_tp_diag_gap_max_ms = 0;
            s_tp_diag_press_edges = 0;
            s_tp_diag_i2c_fail = 0;
            s_tp_latency_max_ms = 0;
            diag_report_us = now_us;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(TP_SAMPLE_PERIOD_MS));
    }
}

esp_lcd_touch_handle_t tp_get_handle(void) // 获取触摸屏设备句柄的接口函数
{
    return g_ft6x36_handle; // 返回全局触摸设备句柄，供上层读取触摸数据使用
}

bool tp_snapshot_pressed(uint16_t *x, uint16_t *y)
{
    bool pressed;

    portENTER_CRITICAL(&s_tp_lock);
    pressed = s_tp_pressed;
    if (x != NULL)
    {
        *x = s_tp_x;
    }
    if (y != NULL)
    {
        *y = s_tp_y;
    }
    portEXIT_CRITICAL(&s_tp_lock);

    return pressed;
}

bool tp_click_take(uint16_t *x, uint16_t *y)
{
    bool pending;

    portENTER_CRITICAL(&s_tp_lock);
    pending = s_tp_click_pending;
    s_tp_click_pending = false;
    if (pending)
    {
        if (x != NULL)
        {
            *x = s_tp_click_x;
        }
        if (y != NULL)
        {
            *y = s_tp_click_y;
        }
    }
    portEXIT_CRITICAL(&s_tp_lock);

    return pending;
}
