
/**
 * ============================================================
 *  文件名称 : pwm.c —— LEDC（PWM）驱动模块（ESP32-S3 / ESP-IDF）
 *  功能描述 : 提供 LEDC 定时器与通道初始化接口 pwm_led_init()，
 *             将 GPIO10 配置为 PWM 输出，用于驱动 LED 亮度。
 *
 *  【引脚与参数定义】
 *    LED_PIN     = GPIO_NUM_10
 *    PWM 频率    = 100Hz
 *    分辨率      = 10 位（占空比范围 0 ~ 1023）
 *    速度模式    = LEDC_LOW_SPEED_MODE
 *    定时器编号  = LEDC_TIMER_0
 *    通道编号    = LEDC_CHANNEL_0
 *
 *  【代码逻辑说明】
 *    1. ledc_timer_config():   配置 LEDC 定时器（频率、分辨率、时钟源等）；
 *    2. ledc_channel_config(): 配置输出通道并关联定时器，映射到 GPIO10，
 *                              同时设置初始占空比与初始输出电平。
 * ============================================================
 */

#include "driver/gpio.h"       // GPIO 驱动头文件，提供 gpio 的函数接口
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include <stdio.h>             // 标准输入输出头文件，提供 printf 等函数

// 包含 ESP32 LEDC（LED Control）驱动库
// LEDC 是 ESP32 专用的 PWM 控制器，用于生成 PWM 信号
#include "driver/ledc.h"

/**
 * @brief LEDC PWM 初始化函数：配置 PWM 定时器与输出通道，使 GPIO10 输出 PWM 信号
 *
 * 注意：本函数使用 LEDC 硬件 PWM 控制器生成波形，而非普通的 GPIO 推挽输出。
 */
void pwm_led_init(void)
{
    // 定义 LEDC 定时器配置结构体，PWM 输出频率 100Hz
    ledc_timer_config_t ledc_timer = {
        // 设置定时器速度模式：低速模式
        .speed_mode = LEDC_LOW_SPEED_MODE,

        // 设置定时器编号：定时器0
        .timer_num = LEDC_TIMER_0,

        // 设置PWM分辨率：10位（0-1023）
        // 分辨率越高，占空比调节越精细
        .duty_resolution = LEDC_TIMER_10_BIT,

        // 设置PWM频率：100Hz
        // 频率越高，PWM周期越短
        .freq_hz = 100,

        // 设置定时器分频器：自动计算
        // ESP32会根据频率和分辨率自动计算分频器值
        .clk_cfg = LEDC_AUTO_CLK,
    };
    // 将定时器配置应用到 LEDC 硬件，使定时器生效
    ledc_timer_config(&ledc_timer);

    // 定义 LEDC 通道配置结构体
    ledc_channel_config_t ledc_channel = {
        // 设置通道速度模式：低速模式
        .speed_mode = LEDC_LOW_SPEED_MODE,

        // 设置通道编号：通道0
        .channel = LEDC_CHANNEL_0,

        // 设置定时器编号：定时器0
        // 通道必须关联到一个定时器
        .timer_sel = LEDC_TIMER_0,

        // 设置GPIO引脚：GPIO10
        // PWM信号将从该引脚输出
        .gpio_num = GPIO_NUM_10,

        // 设置初始占空比：0
        // 占空比范围：0-1023（基于10位分辨率，对应0-100%亮度）
        // 计算公式：占空比值 = (亮度百分比 / 100) * 1023
        .duty = 0,

        // 设置初始输出电平：低电平
        .hpoint = 0,
    };

    // 将通道配置应用到 LEDC 硬件，PWM 信号从 GPIO10 输出
    ledc_channel_config(&ledc_channel);
}

// void pwm_lcd_init(void)
// {

// }
