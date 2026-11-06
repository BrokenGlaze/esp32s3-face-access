/**
 * @file    camera.c
 * @brief   DVP 摄像头（GC0308）驱动实现：初始化 esp_camera，并通过
 *          生产者-消费者双任务将采集到的图像帧实时显示到 LCD。
 *
 * @note    适配本工程模块化风格：
 *          - 摄像头电源由 PCA9557 扩展 IO 第 2 位（DVP_PWDN）控制
 *          - SCCB 复用 I2C0 总线（SIOD=GPIO1 / SIOC=GPIO2），须用新版 I2C 驱动
 *            （IDF 5.5 + esp32-camera sccb-ng.c 通过 i2c_master_get_bus_handle 复用）
 *          - 图像格式 RGB565、QVGA(320x240)、双帧缓冲、帧缓冲位于 PSRAM
 *          - 屏幕方向由 lcd.h 的 LCD_ORIENTATION 宏决定（横屏 480x320 / 竖屏 320x480），
 *            camera_lcd_start() 预览模式从帧画面中央裁剪 LCD_WIDTH x LCD_HEIGHT 区域，
 *            帧尺寸不足（如 QVGA）时自动贴边显示，裁剪可以做到不变形
 *          - 人脸识别流程（main.c 调用 app_human_face_recognition()）不再使用
 *            camera_lcd_start()，而是由 AI 模块自行采集/显示整帧
 *          - 绘制时分块传输（每次 160 行），避免一次传输整帧导致显示不完整
 *          - 采集任务绑定 Core1，显示任务绑定 Core0，通过队列传递帧指针
 */

#include "camera.h"
#include "esp_camera.h"    // 摄像头驱动接口：esp_camera_init / fb_get / fb_return
#include "esp_heap_caps.h" // PSRAM 分配接口：heap_caps_malloc（IDF 5.5 起由 esp_heap_caps.h 提供）
#include "esp_lcd_panel_ops.h" // 面板操作接口：esp_lcd_panel_draw_bitmap
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lcd.h"     // LCD 面板句柄与屏幕尺寸宏（LCD_WIDTH/LCD_HEIGHT）
#include "pca9557.h" // IO 扩展芯片接口：pca9557_set_output_state
#include <assert.h>  // 断言接口：assert
#include <stdio.h>
#include <string.h> // 内存拷贝接口：memcpy

#define CAMERA_TAG "camera"

/* LCD 显示帧队列：在采集任务与显示任务之间传递 camera_fb_t* 指针 */
static QueueHandle_t s_lcd_frame_queue = NULL;

/* PSRAM 裁剪缓冲：用于从摄像头帧中裁剪出满屏 LCD_WIDTH x LCD_HEIGHT 的连续像素数据 */
static uint16_t* s_crop_buf = NULL;

/**
 * @brief 摄像头采集任务（生产者，Core1）
 * @param arg 任务参数（未使用）
 *
 * 循环调用 esp_camera_fb_get() 获取一帧图像，成功后把帧指针送入队列；
 * 队列满时阻塞等待显示任务消费，保证双缓冲帧率与内存安全。
 */
static void camera_grab_task(void* arg)
{
    while (1)
    {
        camera_fb_t* frame = esp_camera_fb_get(); // 获取一帧（RGB565）
        if (frame)
        {
            xQueueSend(s_lcd_frame_queue, &frame, portMAX_DELAY); // 交给显示任务
        }
        /* frame == NULL 表示取帧失败，继续循环重试 */
    }
}

/**
 * @brief LCD 显示任务（消费者，Core0）
 * @param arg 任务参数（未使用）
 *
 * 从队列取出一帧，从 VGA(640x480) 画面中央裁剪 LCD_WIDTH x LCD_HEIGHT 区域
 * （满屏且不变形），随后调用 esp_camera_fb_return() 回收帧缓冲，使其可被复用。
 */
static void camera_display_task(void* arg)
{
    esp_lcd_panel_handle_t panel = lcd_get_panel_handle(); // LCD 面板句柄
    camera_fb_t* frame = NULL;

    /* 在 PSRAM 中分配满屏裁剪缓冲（LCD_WIDTH x LCD_HEIGHT x 2 字节） */
    s_crop_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_crop_buf != NULL);

    while (1)
    {
        if (!xQueueReceive(s_lcd_frame_queue, &frame, portMAX_DELAY))
        {
            continue;
        }

        /* 从帧画面中央裁剪出满屏区域；帧尺寸不足时自动贴边（QVGA 下裁剪起点为 0） */
        int crop_x = (frame->width - LCD_WIDTH) / 2;
        int crop_y = (frame->height - LCD_HEIGHT) / 2;
        int crop_w = (frame->width < LCD_WIDTH) ? frame->width : LCD_WIDTH;
        int crop_h = (frame->height < LCD_HEIGHT) ? frame->height : LCD_HEIGHT;
        if (crop_x < 0)
        {
            crop_x = 0;
        }   
        if (crop_y < 0)
        {
            crop_y = 0;
        }

        /* 逐行拷贝中央区域到裁剪缓冲（每行拷贝 min(帧宽,屏宽) 像素，防止越界） */
        uint16_t* src_row = (uint16_t*)frame->buf + crop_y * frame->width + crop_x;
        uint16_t* dst = s_crop_buf;
        for (int y = 0; y < crop_h; y++)
        {
            memcpy(dst, src_row, crop_w * sizeof(uint16_t));
            dst += crop_w;
            src_row += frame->width;
        }

        /* 分块绘制（每次 160 行），避免一次传输整帧导致显示不完整 */
        for (int y = 0; y < crop_h; y += 160)
        {
            int rows = (y + 160 > crop_h) ? crop_h - y : 160; // 末块不足 160 行时截断
            esp_lcd_panel_draw_bitmap(panel, 0, y, crop_w, y + rows, s_crop_buf + y * crop_w);
        }

        esp_camera_fb_return(frame); // 回收帧缓冲，供摄像头驱动复用
    }
}

esp_err_t camera_init(void)
{
    /* 1. 通过 PCA9557 拉低 DVP_PWDN，打开摄像头电源 */
    esp_err_t ret = pca9557_set_output_state(CAMERA_PWDN_PIN, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(CAMERA_TAG, "camera pwdn set failed, ret=%d", ret);
        return ret;
    }

    /* 2. 配置摄像头参数 */
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_1, // 用于生成 XCLK（ESP32-S3 不使用）
        .ledc_timer = LEDC_TIMER_1,     // 用于生成 XCLK（ESP32-S3 不使用）

        /* DVP 8 位并行数据线 */
        .pin_d0 = CAMERA_PIN_D0,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d7 = CAMERA_PIN_D7,

        /* 同步信号 */
        .pin_xclk = CAMERA_PIN_XCLK,   // XCLK 时钟（ESP32-S3 自动生成）
        .pin_pclk = CAMERA_PIN_PCLK,   // 像素时钟输入
        .pin_vsync = CAMERA_PIN_VSYNC, // 垂直同步
        .pin_href = CAMERA_PIN_HREF,   // 水平参考

        /* SCCB 控制线：SDA 写 -1 表示复用已初始化的 I2C0 总线 */
        .pin_sccb_sda = -1,
        .pin_sccb_scl = CAMERA_PIN_SIOC,
        .sccb_i2c_port = CAMERA_SCCB_I2C_PORT,

        /* 控制引脚（电源/复位由 PCA9557 与上电默认控制） */
        .pin_pwdn = CAMERA_PIN_PWDN,
        .pin_reset = CAMERA_PIN_RESET,

        /* 时钟与图像格式 */
        .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
        .pixel_format = PIXFORMAT_RGB565, // RGB565：每像素 2 字节，便于 LCD 直显
        .frame_size = FRAMESIZE_QVGA, // QVGA：320x240（匹配人脸识别 AI 流水线，帧率更高）
        .jpeg_quality = 12,          // JPEG 质量（RGB565 模式不使用）

        /* 帧缓冲：双缓冲，分配在 PSRAM */
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    /* 3. 初始化摄像头驱动（内部完成传感器检测与 GC0308 寄存器配置） */
    ret = esp_camera_init(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(CAMERA_TAG, "Camera init failed with error 0x%x", ret);
        return ret;
    }

    /* 4. 对 GC0308 开启水平镜像，保证图像方向与 LCD 一致 */
    sensor_t* s = esp_camera_sensor_get(); // 获取摄像头传感器句柄，用于配置镜像/画质等参数
    if (s && s->id.PID == GC0308_PID)
    {
        s->set_hmirror(s, 1); // 开启水平镜像，与 LCD 横屏方向保持一致
    }

    ESP_LOGI(CAMERA_TAG, "Camera GC0308 init ok");
    return ESP_OK;
}

void camera_lcd_start(void)
{
    /* 1. 创建帧队列：深度 2（双缓冲），元素为 camera_fb_t* 指针 */
    s_lcd_frame_queue = xQueueCreate(2, sizeof(camera_fb_t*));

    /* 2. 采集任务绑定 Core1（与显示任务分离，充分利用双核） */
    xTaskCreatePinnedToCore(camera_grab_task, "camera_grab", 3 * 1024, NULL, 5, NULL, 1);

    /* 3. 显示任务绑定 Core0 */
    xTaskCreatePinnedToCore(camera_display_task, "camera_display", 4 * 1024, NULL, 5, NULL, 0);
}
