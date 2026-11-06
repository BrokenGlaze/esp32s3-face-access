/* ============================ 文件功能概述 ============================ */
/* 本文件是 I2C 总线驱动模块的实现，通过宏 I2C_VERSION 在编译期选择使用   */
/* 旧版或新版 ESP-IDF I2C 驱动接口：                                      */
/*   1. 旧版接口（I2C_VERSION == I2C_VERSION_1，IDF < 5.5）：             */
/*      使用 i2c_param_config + i2c_driver_install 初始化总线；           */
/*   2. 新版接口（I2C_VERSION == I2C_VERSION_2，IDF >= 5.5）：            */
/*      使用 i2c_new_master_bus 创建总线句柄，并对外提供句柄获取函数，    */
/*      供上层模块（PCA9557、FT6x36 触摸屏等）添加各自的从机设备。        */
/* ====================================================================== */

#include "i2c.h"                 /* 自定义 I2C 头文件，提供版本宏、引脚宏与函数声明 */
#include "freertos/FreeRTOS.h"   /* FreeRTOS 内核头文件，提供任务、延时等接口 */
#include "freertos/task.h"       /* FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数 */
#include <stdio.h>               /* 标准输入输出头文件，提供 printf 等函数 */

#if (I2C_VERSION == I2C_VERSION_2)
/* 新版 I2C 驱动总线句柄：静态全局变量，仅本文件内部访问，通过 i2c_get_bus_handle() 对外提供 */
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
#endif

/**
 * @brief I2C 总线初始化函数
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则返回对应错误码
 *
 * 功能：根据 I2C_VERSION 宏选择对应的初始化方式：
 *   - 旧版（I2C_VERSION_1）：通过 i2c_param_config 配置参数 + i2c_driver_install
 *     安装驱动，完成后通过"端口号"访问总线；
 *   - 新版（I2C_VERSION_2）：通过 i2c_new_master_bus 创建总线句柄，完成后
 *     通过 i2c_get_bus_handle() 获取句柄，再调用 i2c_master_bus_add_device
 *     注册从机设备。
 */
esp_err_t i2c_init(void)
{
#if (I2C_VERSION == I2C_VERSION_1)
    /* ==================== 旧版 I2C 接口（IDF < 5.5） ==================== */
    esp_err_t ret = ESP_OK; /* 定义返回值变量并初始化为 ESP_OK */

    /* 定义 I2C 配置结构体，描述总线的工作模式与电气参数 */
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,             /* 设置 I2C 工作模式：主机模式 */
        .sda_io_num = I2C_PIN_SDA,           /* 设置 SDA 数据线引脚（GPIO1） */
        .sda_pullup_en = GPIO_PULLUP_ENABLE, /* 使能 SDA 内部上拉电阻 */
        .scl_io_num = I2C_PIN_SCL,           /* 设置 SCL 时钟线引脚（GPIO2） */
        .scl_pullup_en = GPIO_PULLUP_ENABLE, /* 使能 SCL 内部上拉电阻 */
        .master.clk_speed = I2C_FREQ_HZ,     /* 设置 I2C 时钟频率：100kHz */
    };

    /* 将上述配置参数应用到 I2C_NUM 外设的控制器寄存器中 */
    ret = i2c_param_config(I2C_NUM, &i2c_config);
    if (ret != ESP_OK)
    {
        return ret; /* 参数配置失败则直接返回错误码 */
    }

    /* 安装 I2C 驱动程序，参数依次为：外设号、工作模式、发送缓冲 0、接收缓冲 0、中断标志 0 */
    ret = i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    return ret;
#else
    /* ==================== 新版 I2C 接口（IDF >= 5.5） ==================== */
    /* 定义 I2C 主总线配置结构体，描述总线端口、引脚与电气参数 */
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM,               /* I2C 端口号 */
        .sda_io_num = I2C_PIN_SDA,         /* SDA 数据线引脚号 */
        .scl_io_num = I2C_PIN_SCL,         /* SCL 时钟线引脚号 */
        .clk_source = I2C_CLK_SRC_DEFAULT, /* 时钟源，使用默认时钟 */
        .glitch_ignore_cnt = 7,            /* 毛刺忽略计数，过滤干扰信号 */
        .intr_priority = 0,                /* 中断优先级，0 表示使用默认值 */
        .trans_queue_depth = 0,            /* 传输队列深度，0 表示不使用队列 */
        .flags =
        {
            .enable_internal_pullup = 1,   /* 使能内部上拉电阻 */
        },
    };

    /* 创建 I2C 主总线，成功后将总线句柄保存到全局变量，供其他模块添加从机设备使用 */
    return i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
#endif
}

#if (I2C_VERSION == I2C_VERSION_2)
/**
 * @brief 获取新版 I2C 总线句柄
 * @return i2c_master_bus_handle_t 返回 I2C 总线句柄，供上层模块添加从机设备使用
 *
 * 功能：返回 i2c_init() 创建的总线句柄。
 * 使用示例（在 PCA9557 驱动中注册从机设备）：
 *     i2c_device_config_t dev_cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
 *                                     .device_address = 0x19,
 *                                     .scl_speed_hz = I2C_FREQ_HZ };
 *     i2c_master_dev_handle_t dev_handle;
 *     i2c_master_bus_add_device(i2c_get_bus_handle(), &dev_cfg, &dev_handle);
 * 注意：必须在调用 i2c_init() 成功初始化总线之后使用，否则返回 NULL。
 */
i2c_master_bus_handle_t i2c_get_bus_handle(void)
{
    return i2c_bus_handle; /* 返回保存的总线句柄 */
}
#endif
