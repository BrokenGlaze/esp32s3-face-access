/**
 * @file pca9557.c
 * @brief PCA9557 I/O 扩展芯片驱动源文件
 *
 * 使用说明：
 *   1. 硬件连接：
 *      - PCA9557 的 SDA/SCL 引脚接到 ESP32S3 对应的 I2C 引脚（见 i2c.h 中 I2C_PIN_SDA /
 * I2C_PIN_SCL）；
 *      - 器件地址由 A0/A1/A2 引脚决定，本驱动默认地址为 0x19（见 pca9557.h 中
 * PCA9557_DEVICE_ADDR）；
 *      - SDA/SCL 建议外接 4.7kΩ 上拉电阻。
 *   2. 初始化流程：
 *      - 先调用 i2c_init() 完成 I2C 总线初始化；
 *      - 再调用 pca9557_init() 将 IO0~IO2 配置为输出模式并设定初始电平。
 *   3. 寄存器说明（PCA9557 内部寄存器）：
 *      - 0x00：输入寄存器（只读），对应位为 1 表示该 IO 为高电平；
 *      - 0x01：输出寄存器（读写），控制 IO 输出电平；
 *      - 0x03：配置寄存器（读写），对应位写 1 为输入模式、写 0 为输出模式。
 *   4. 读取 IO 输入状态：调用 pca9557_register_read(0x00, buf, 1) 即可。
 *
 * @note 依赖自定义 i2c.h。I2C 读写接口根据 I2C_VERSION 宏自动选择：
 *       - I2C_VERSION_1（旧版驱动）：i2c_master_write_to_device /
 *         i2c_master_write_read_device（通过端口号访问）；
 *       - I2C_VERSION_2（新版驱动）：i2c_master_transmit /
 *         i2c_master_transmit_receive（通过设备句柄访问，设备句柄在
 *         pca9557_init() 中调用 i2c_master_bus_add_device 注册获取）。
 */

#include "pca9557.h"           // 自定义 PCA9557 头文件，提供 pca9557_init 函数声明
#include "driver/gpio.h"       // GPIO 驱动头文件，提供 gpio 的函数接口
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "i2c.h"               // 自定义 I2C 头文件，提供版本宏、i2c_get_bus_handle 等接口
#include <stdio.h>             // 标准输入输出头文件，提供 printf 等函数

#if (I2C_VERSION == I2C_VERSION_2)
/* 新版驱动：PCA9557 设备句柄（静态全局变量，在 pca9557_init() 中注册获取，仅本文件内部使用） */
static i2c_master_dev_handle_t pca9557_dev_handle = NULL;
#endif

/**
 * @brief 初始化 PCA9557：将 IO0~IO2 配置为输出模式并设置初始电平
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则返回对应错误码
 *
 * 功能：在新版驱动下先通过 i2c_master_bus_add_device 将 PCA9557 注册到
 *       I2C 总线获取设备句柄，再向配置寄存器(0x03)写入 0xF8（IO0~IO2 为
 *       输出、其它引脚为输入），向输出寄存器(0x01)写入 0x05（IO0=1 高电平、
 *       IO1=0 低电平、IO2=1 高电平）。
 */
esp_err_t pca9557_init(void)
{
    esp_err_t ret = ESP_OK; /* 定义返回值变量，初始化为 ESP_OK */

#if (I2C_VERSION == I2C_VERSION_2)
    /* 新版驱动：将 PCA9557 设备注册到 I2C 总线，获取设备句柄供后续读写使用 */
    i2c_device_config_t pca9557_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, /* 设备地址长度：7 位地址 */
        .device_address = PCA9557_DEVICE_ADDR, /* PCA9557 设备地址 0x19 */
        .scl_speed_hz = I2C_FREQ_HZ,           /* I2C 通信时钟频率 100kHz */
    };
    ret = i2c_master_bus_add_device(i2c_get_bus_handle(), &pca9557_dev_config, &pca9557_dev_handle);
    if (ret != ESP_OK) /* 判断设备注册是否成功 */
    {
        printf("pca9557 add device failed, ret = %d\n", ret); /* 注册失败打印错误信息 */
        return ret;                                            /* 返回错误码，终止初始化 */
    }
#endif

    /* 向配置寄存器(0x03)写入 0xF8：低 3 位(IO0~IO2)为输出模式，高 5 位保持输入模式 */
    ret = pca9557_register_write_byte(0x03, 0xf8);
    if (ret != ESP_OK)                                  /* 判断写入是否失败 */
    {
        printf("pca9557_init failed, ret = %d\n", ret); /* 打印初始化失败信息及错误码 */
        return ret;                                     /* 返回错误码，结束初始化 */
    }

    /* 向输出寄存器(0x01)写入 0x05：IO0=1(LCD_CS 高)、IO1=0(PA_EN 低)、IO2=1(DVP_PWDN 高) */
    ret = pca9557_register_write_byte(0x01, 0x05);
    if (ret != ESP_OK)                                  /* 判断写入是否失败 */
    {
        printf("pca9557_init failed, ret = %d\n", ret); /* 打印初始化失败信息及错误码 */
    }

    return ret; /* 返回执行结果 */
}

/**
 * @brief 向 PCA9557 指定寄存器写入一个字节数据
 * @param reg_addr 目标寄存器地址（如 0x01 输出、0x03 配置）
 * @param data     要写入的 8 位数据
 * @return esp_err_t 返回 ESP_OK 表示写入成功，否则返回对应错误码
 *
 * 功能：根据 I2C_VERSION 宏选择旧版或新版 I2C 写入接口，向 PCA9557
 *       发送 [寄存器地址 + 数据] 两个字节完成写入。
 */
esp_err_t pca9557_register_write_byte(uint8_t reg_addr, uint8_t data)
{
    /* 构造写入缓冲区：第 0 字节为寄存器地址，第 1 字节为要写入的数据 */
    uint8_t write_buf[2] = {reg_addr, data};

#if (I2C_VERSION == I2C_VERSION_1)
    /* 旧版驱动：通过"端口号 + 设备地址"直接写入，超时时间 1000ms */
    return i2c_master_write_to_device(
        I2C_NUM,
        PCA9557_DEVICE_ADDR,
        write_buf,
        sizeof(write_buf),
        pdMS_TO_TICKS(1000));
#else
    /* 新版驱动：通过设备句柄完成写入，超时时间 1000ms */
    return i2c_master_transmit(pca9557_dev_handle, write_buf, sizeof(write_buf), pdMS_TO_TICKS(1000));
#endif
}

/**
 * @brief 读取 PCA9557 指定寄存器的值
 * @param reg_addr 寄存器地址（如 0x00 输入、0x01 输出、0x03 配置）
 * @param data     存放读取数据的缓冲区指针
 * @param len      要读取的字节数
 * @return esp_err_t 返回 ESP_OK 表示读取成功，否则返回对应错误码
 *
 * 功能：根据 I2C_VERSION 宏选择旧版或新版 I2C 先写后读接口，
 *       先发送 1 字节寄存器地址，再连续读取 len 字节数据。
 */
esp_err_t pca9557_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
#if (I2C_VERSION == I2C_VERSION_1)
    /* 旧版驱动：调用 I2C 写读组合函数，先写寄存器地址再读数据，超时时间 1000ms */
    return i2c_master_write_read_device(
        I2C_NUM,             /* I2C 总线号 */
        PCA9557_DEVICE_ADDR, /* PCA9557 器件地址 0x19 */
        &reg_addr,           /* 待读取的寄存器地址 */
        1,                   /* 寄存器地址字节数为 1 */
        data,                /* 读取数据的存放缓冲区 */
        len,                 /* 读取数据的长度 */
        pdMS_TO_TICKS(1000)); /* I2C 通信超时时间 1000ms */
#else
    /* 新版驱动：通过设备句柄先发送寄存器地址再读取数据，超时时间 1000ms */
    return i2c_master_transmit_receive(pca9557_dev_handle, &reg_addr, 1, data, len, pdMS_TO_TICKS(1000));
#endif
}

/**
 * @brief 设置 PCA9557 指定 IO 引脚的输出电平状态
 * @param gpio_pin 目标引脚号（0~2，对应 IO0~IO2）
 * @param level    输出电平：1 为高电平，0 为低电平
 * @return esp_err_t 返回 ESP_OK 表示设置成功，否则返回对应错误码
 *
 * 功能：先读取输出寄存器(0x01)当前值保留其它引脚状态，再通过位操作
 *       修改目标引脚对应位后写回，实现单引脚电平控制。
 */
esp_err_t pca9557_set_output_state(uint8_t gpio_pin, uint8_t level)
{
    uint8_t data;            /* 变量用于暂存读到的当前输出端口状态 */
    esp_err_t ret = ESP_OK;  /* 定义返回值变量，初始化为 ESP_OK */

    /* 读取输出寄存器(0x01)当前值，保留其它引脚状态 */
    ret = pca9557_register_read(0x01, &data, sizeof(data));
    if (ret != ESP_OK) /* 判断读取是否失败 */
    {
        printf("pca9557_register_read failed, ret = %d\n", ret); /* 打印读取失败信息 */
        return ret;                                              /* 返回错误码 */
    }

    /* 根据 level 设置指定 IO 引脚对应的输出电平位 */
    if (level)
    {
        data |= (1 << gpio_pin); /* 置 1：对应引脚输出高电平 */
    }
    else
    {
        data &= ~(1 << gpio_pin); /* 清 0：对应引脚输出低电平 */
    }

    /* 将修改后的值写回输出寄存器(0x01) */
    ret = pca9557_register_write_byte(0x01, data);
    if (ret != ESP_OK) /* 判断写入是否失败 */
    {
        printf("pca9557_register_write_byte failed, ret = %d\n", ret); /* 打印写入失败信息 */
        return ret;                                                    /* 返回错误码 */
    }

    return ret; /* 返回执行结果 */
}
