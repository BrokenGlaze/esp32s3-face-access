#ifndef __LED_H__
#define __LED_H__

#include <stdbool.h>

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LED_PIN GPIO_NUM_10        // 宏定义：LED 所连接的 GPIO 引脚号（GPIO10）

/**
 * @brief LED 初始化函数：将 GPIO 配置为推挽输出模式，并设置初始状态为熄灭
 */
extern void led_init(void);

/**
 * @brief 点亮 LED：向 GPIO 输出低电平（本工程低电平点亮、高电平熄灭）
 */
extern void led_on(void);

/**
 * @brief 熄灭 LED：向 GPIO 输出高电平（本工程低电平点亮、高电平熄灭）
 */
extern void led_off(void);

/**
 * @brief 读取 LED 当前状态
 * @return true 点亮；false 熄灭
 */
extern bool led_get_state(void);

#ifdef __cplusplus
}
#endif

#endif
