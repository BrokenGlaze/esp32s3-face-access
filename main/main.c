/**
 * @file    main.c
 * @brief   主程序入口：初始化 LED、I2C、PCA9557、LCD、LVGL 界面、摄像头、
 *          I2S 与 ES8311 外设，打开音频功放后启动人脸识别任务；
 *          LED 用作开门/锁门状态指示（识别成功开门 3 秒后自动锁门）。
 *
 * @note    使用说明：
 *          - 板级初始化顺序：led_init -> i2c_init -> pca9557_init
 *            -> camera_init -> lcd_init -> tp_init -> lvgl_ui_init
 *            （有先后依赖，不可颠倒；摄像头必须在 LCD/LVGL 之前初始化，
 *              避免 SPI DMA 与射频占用资源导致 DVP 取不到帧）
 *          - LCD 与摄像头服务于人脸检测/识别：LCD 先初始化（LVGL 端口与摄像头
 *            预览都依赖 lcd_get_panel_handle / lcd_get_io_handle），摄像头
 *            DVP_PWDN 由 PCA9557 的 IO2 控制
 *          - lvgl_ui_init() 之后所有画面更新都必须通过 lvgl_ui 接口完成，
 *            不能再调用 esp_lcd_panel_draw_bitmap 等裸绘接口，否则会和 LVGL
 *            的 flush 抢 SPI 总线
 *          - 人脸识别 app_human_face_recognition() 依赖 LCD/摄像头/LVGL 初始化
 *            完成，内部创建采集、AI 推理、预览刷新与按键任务
 *
 *          ESP-IDF 版本：必须用 5.5.0 版本，否则会报错
 *          i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
 */

#include "audio_prompt.h"      // 固定语音片段播报模块，提供 audio_prompt_init/play
#include "camera.h"            // 自定义摄像头头文件，提供 camera_init 函数声明
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "i2c.h"               // 自定义 I2C 头文件，提供 i2c_init 函数声明
#include "lcd.h"               // 自定义 LCD 头文件，提供 lcd_init、lcd_set_color 函数声明
#include "led.h"               // 自定义 LED 头文件，提供 led_init、led_on、led_off 函数声明
#include "lvgl_ui.h"           // LVGL 界面：摄像头预览、登入按钮、左下角信息
#include "nvs_flash.h"         // NVS：保存语音播报音量/开关与管理员密码
#include "pca9557.h"           // 自定义 PCA9557 头文件，提供 pca9557_init 函数声明
#include "tp.h"                // 自定义触摸屏头文件，提供 tp_init 函数声明
#include "wifi_manager.h"      // Wi-Fi 管理：初始化、扫描、连接、NVS 凭据
#include "time_sync.h"         // SNTP 校时：给历史记录提供时间戳
#include "bemfa_mqtt.h"        // 巴法云 MQTT：设备在线状态与识别通知
#include "bemfa_uploader.h"    // 巴法云照片上传：识别成功即上传历史照片
#include "who_human_face_recognition.hpp" // 人脸识别模块，提供 app_human_face_recognition 函数声明
#include <stdio.h> // 标准输入输出头文件，提供 printf 等函数

/**
 * @brief 应用主函数：程序入口，负责初始化外设并启动人脸识别与语音播报
 */
void app_main(void) // 应用主函数：ESP-IDF 程序入口，系统启动后自动调用
{

    led_init(); // 初始化 LED，设置 GPIO10 为输出模式
    led_off(); // 关闭 LED（高电平），表示门处于锁定状态

    // 初始化 NVS：语音播报音量/开关与管理员密码需要掉电保存，必须在使用前完成
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase(); // 分区表变化或空间耗尽：擦除后重新初始化
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK)
    {
        // NVS 不可用只影响设置保存，其余功能继续运行
        printf("nvs_flash_init failed, ret = %d\n", (int)nvs_ret);
    }

    // 初始化 I2C
    esp_err_t ret = i2c_init(); // 调用自定义 i2c_init 初始化 I2C 总线，返回错误码
    if (ret != ESP_OK)          // 判断 I2C 初始化是否成功（返回 ESP_OK 表示成功）
    {
        printf("i2c_init failed, ret = %d\n", ret); // 初始化失败时通过串口打印错误信息及错误码
        return;                                     // 失败则直接退出，不继续执行后续初始化
    }

    // 初始化 PCA9557
    ret = pca9557_init(); // 调用自定义 pca9557_init 初始化 PCA9557 扩展 IO，返回错误码
    if (ret != ESP_OK)    // 判断 PCA9557 初始化是否成功
    {
        printf("pca9557_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                         // 失败则直接退出，不继续执行后续初始化
    }

    // 等待摄像头模块上电稳定（PCA9557 的 IO2 已拉低，DVP 摄像头开始供电）
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // 初始化摄像头（RGB565 / QVGA，内部通过 PCA9557 IO2 打开 DVP_PWDN）
    ret = camera_init(); // 初始化 esp_camera，返回错误码
    if (ret != ESP_OK)   // 判断摄像头初始化是否成功
    {
        printf("camera_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                         // 失败则直接退出，不继续执行后续初始化
    }

    // 初始化 LCD（人脸识别画面显示）
    ret = lcd_init(); // 初始化 ST7796 SPI 屏（横屏 480x320）
    if (ret != ESP_OK) // 判断 LCD 初始化是否成功
    {
        printf("lcd_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                      // 失败则直接退出，不继续执行后续初始化
    }
    lcd_set_color(0); // 清屏为黑色（LVGL 初始化前的背景）

    // 初始化触摸屏（FT6x36，LVGL 触摸输入依赖此初始化）
    ret = tp_init();
    if (ret != ESP_OK)
    {
        // 触摸屏失败不影响人脸识别主流程，仅打印错误继续运行
        printf("tp_init failed, ret = %d\n", ret);
    }

    // 初始化 LVGL 界面：端口、摄像头预览、登入按钮、左下角信息
    // 之后所有画面更新都必须走 lvgl_ui 接口，不能再裸绘 LCD，否则会和 LVGL 抢 SPI
    ret = lvgl_ui_init();
    if (ret != ESP_OK)
    {
        printf("lvgl_ui_init failed, ret = %d\n", ret);
        return;
    }

    // 初始化语音播报（I2S TX + ES8311 + 功放，仅播放固定提示音）
    ret = audio_prompt_init();
    if (ret != ESP_OK)
    {
        // 播报失败不影响人脸识别，仅打印错误继续运行
        printf("audio_prompt_init failed, ret = %d\n", ret);
    }

    // 启动人脸识别（创建摄像头采集、AI 推理、预览刷新等任务）
    app_human_face_recognition();

    /* app_human_face_recognition 会等 AI 模型创建完成并释放“模型下沉”占位内存后返回，
       此时内部 RAM 已让给网络功能，紧接着初始化 Wi-Fi。
       摄像头任务此时处于“不取帧”状态，Wi-Fi 初始化不会与摄像头 DMA 冲突 */
    esp_err_t wifi_ret = wifi_manager_init();
    if (wifi_ret != ESP_OK)
    {
        printf("wifi_manager_init failed, ret = %d\n", (int)wifi_ret);
    }

    /* Wi-Fi 初始化结束（无论成功失败）再开启摄像头采集 */
    human_face_camera_start();

    /* 联网后校时 + 连接巴法云 MQTT（在线状态/识别通知） */
    time_sync_init();
    bemfa_mqtt_start();

    /* 照片上传任务：识别成功的照片在后台排队上传（未联网/未校时时自动等待） */
    bemfa_uploader_start();

    /* app_main 至此返回：人脸识别与音频播报由独立的 FreeRTOS 任务持续运行，
     * 本主任务会被 FreeRTOS 自动删除，不影响各任务的执行 */
}
