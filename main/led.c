

#include <stdio.h>                 // 标准输入输出头文件，提供 printf 等函数
#include "freertos/FreeRTOS.h"     // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"         // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "driver/gpio.h"           // GPIO 驱动头文件，提供 gpio 的函数接口
#include "led.h"                   // 自定义 LED 头文件，提供 led_init 函数声明


/**
 * @brief LED 初始化函数：将 GPIO 配置为推挽输出模式，并设置初始状态为熄灭
 */
void led_init(void)
{
    // 定义 GPIO 配置结构体，设置各配置项
    gpio_config_t gpio = {
        .pin_bit_mask = 1 << LED_PIN,             // 位掩码：选中 GPIO10 引脚（bit10 置 1）
        .mode = GPIO_MODE_OUTPUT,                 // 模式：推挽输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,        // 上拉：禁用内部上拉电阻
        .pull_down_en = GPIO_PULLDOWN_DISABLE,    // 下拉：禁用内部下拉电阻
        .intr_type = GPIO_INTR_DISABLE,           // 中断：不使用中断（禁用 GPIO 中断）
    };
    // gpio的配置：将上述配置写入硬件寄存器，完成 GPIO 初始化
    gpio_config(&gpio);
    // 灯为熄灭状态：输出高电平（本工程中低电平点亮、高电平熄灭）
    gpio_set_level(LED_PIN, 1);
}

/**
 * @brief 点亮 LED：向 GPIO10 输出低电平
 */
void led_on(void)
{
    // 输出低电平点亮 LED（本工程低电平点亮、高电平熄灭）
    gpio_set_level(LED_PIN, 0);
}

/**
 * @brief 熄灭 LED：向 GPIO10 输出高电平
 */
void led_off(void)
{
    // 输出高电平熄灭 LED（本工程低电平点亮、高电平熄灭）
    gpio_set_level(LED_PIN, 1);
}

/**
 * @brief 读取 LED 当前状态：低电平为点亮
 */
bool led_get_state(void)
{
    return gpio_get_level(LED_PIN) == 0;
}
