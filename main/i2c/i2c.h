#ifndef __I2C_H__
#define __I2C_H__

/* ============================ 文件功能概述 ============================ */
/* 本头文件是 I2C 总线驱动模块的统一接口定义，支持两套 ESP-IDF I2C 驱动：  */
/*   1. 旧版驱动（I2C_VERSION_1）：使用 i2c_param_config + i2c_driver_install 初始化                    */
/*      读写时直接通过"端口号+设备地址"访问总线。                           */
/*   2. 新版驱动（I2C_VERSION_2）：使用 i2c_new_master_bus + i2c_master_bus_add_device 初始化，          */
/*      读写时通过"总线/设备句柄"访问，接口更安全、支持 DMA 与队列。        */
/*                                                                         */
/* 通过宏 I2C_VERSION 在编译期选择使用哪一套接口，切换版本只需修改一处。    */
/* ======================================================================== */

/* ====================== I2C 接口版本定义与选择 ======================== */
#define I2C_VERSION_1 1 /* 旧版 I2C 驱动接口标识 */
#define I2C_VERSION_2 2 /* 新版 I2C 驱动接口标识 */

/* 当前选用的 I2C 接口版本：修改此宏的值即可在旧版/新版接口之间切换 */
#ifndef I2C_VERSION
#define I2C_VERSION I2C_VERSION_2
#endif

/* 根据所选版本引入对应的 I2C 驱动头文件与 GPIO 引脚宏定义头文件 */
#include "driver/gpio.h" /* GPIO 驱动头文件，提供 GPIO_NUM_x 引脚宏定义 */

#if (I2C_VERSION == I2C_VERSION_1)
#include "driver/i2c.h" /* 旧版 I2C 驱动头文件，提供 i2c_param_config 等接口 */
#elif (I2C_VERSION == I2C_VERSION_2)
#include "driver/i2c_master.h" /* 新版 I2C 驱动头文件，提供 i2c_new_master_bus 等接口 */
#endif

/* ====================== 引脚与参数宏定义 =============================== */
#define I2C_NUM I2C_NUM_0      /* I2C 外设编号：使用 I2C0 总线 */
#define I2C_PIN_SCL GPIO_NUM_2 /* I2C 时钟线 SCL 引脚：GPIO2 */
#define I2C_PIN_SDA GPIO_NUM_1 /* I2C 数据线 SDA 引脚：GPIO1 */
#define I2C_FREQ_HZ 100000     /* I2C 通信时钟频率：100kHz（标准模式） */

/* ====================== 函数声明 ======================================= */
/**
 * @brief 初始化 I2C 总线（自动根据 I2C_VERSION 宏调用旧版或新版驱动接口）
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则返回对应错误码
 *
 * 功能：将 I2C_NUM 外设配置为主机模式并完成引脚、速率等参数配置。
 * 注意：必须在任何挂载在 I2C 总线上的外设操作（PCA9557、FT6x36 触摸屏等）
 *       之前调用。
 */
extern esp_err_t i2c_init(void);

#if (I2C_VERSION == I2C_VERSION_2)
/**
 * @brief 获取新版 I2C 总线句柄
 * @return i2c_master_bus_handle_t 返回 I2C 总线句柄，供上层模块添加从机设备使用
 *
 * 功能：返回 i2c_init() 创建的总线句柄，上层模块（如触摸屏 esp_lcd 组件）
 *       可通过该句柄调用 i2c_master_bus_add_device 注册各自的从机设备。
 * 注意：必须先调用 i2c_init() 成功初始化总线后再调用，否则返回 NULL。
 */
extern i2c_master_bus_handle_t i2c_get_bus_handle(void);
#endif

#endif
