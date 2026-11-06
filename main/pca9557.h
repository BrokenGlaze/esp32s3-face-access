/**
 * @file pca9557.h
 * @brief PCA9557 I/O 扩展芯片驱动头文件
 *
 * 使用说明：
 *   1. 在 main 中先调用 i2c_init() 完成 I2C 总线初始化，
 *      再调用 pca9557_init() 初始化 PCA9557 的 IO 方向及输出电平；
 *   2. 读取 IO 输入状态：调用 pca9557_register_read(0x00, buf, 1)，
 *      返回数据的对应位即为各 IO 的输入电平（1 为高电平）；
 *   3. 如需修改输出电平，可参考 pca9557.c 中的寄存器写方式
 *      （输出寄存器 0x01、配置寄存器 0x03）。
 *
 * @note PCA9557 的器件地址由硬件引脚 A0/A1/A2 决定，本驱动默认 0x19。
 */

#ifndef __PCA9557_H__
#define __PCA9557_H__

#include "i2c.h"                      // 依赖 I2C 头文件，提供 I2C_NUM、esp_err_t 等定义

#define PCA9557_DEVICE_ADDR        0x19   // PCA9557 器件地址（默认 0x19，可依据 A0~A2 调整）

extern esp_err_t pca9557_init(void);                              // 初始化 PCA9557 的引脚方向及输出电平
extern esp_err_t pca9557_register_read(uint8_t reg_addr, uint8_t *data, size_t len); // 读取指定寄存器的值
extern esp_err_t pca9557_set_output_state(uint8_t gpio_pin, uint8_t level); // 设置指定引脚的输出电平状态
extern esp_err_t pca9557_register_write_byte(uint8_t reg_addr, uint8_t data); // 写入指定寄存器的值
#endif
