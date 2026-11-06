

/**
 * ============================================================
 *  文件名称 : uart.c —— UART 串口驱动模块（ESP32-S3 / ESP-IDF）
 *  功能描述 : 提供 UART 串口初始化接口 uart_init()，完成串口参数
 *             配置、引脚映射以及驱动安装，供 main.c 调用。
 *
 *  【引脚定义】
 *    UART_TX_PIN = GPIO_NUM_43
 *    UART_RX_PIN = GPIO_NUM_44
 *    波特率       = 由 uart_init(baud_rate) 参数指定（示例为 115200）
 *
 *  【代码逻辑说明】
 *    1. uart_param_config(): 配置波特率、数据位、校验位、停止位、流控等；
 *    2. uart_set_pin():      将 UART0 映射到指定 TX/RX 引脚（GPIO43/44）；
 *    3. uart_driver_install(): 安装 UART 驱动并分配收发环形缓冲区。
 * ============================================================
 */


#include <stdio.h>                 // 标准输入输出头文件，提供 printf 等函数
#include "freertos/FreeRTOS.h"     // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"         // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "driver/gpio.h"           // GPIO 驱动头文件，提供 gpio 的函数接口
#include "driver/uart.h"           // UART 驱动头文件，提供 uart 的函数接口



/**
 * @brief UART 初始化函数：配置串口参数、引脚映射并安装 UART 驱动
 *
 * @param baud_rate 串口波特率，例如 115200
 */
void uart_init(uint32_t baud_rate)
{
    // 定义UART配置结构体变量，用于存储串口配置参数
    uart_config_t uart_config;

    // 设置串口波特率
    uart_config.baud_rate = baud_rate;

    // 设置数据位长度为8位
    uart_config.data_bits = UART_DATA_8_BITS;

    // 禁用奇偶校验位
    uart_config.parity = UART_PARITY_DISABLE;

    // 设置停止位数量为1位
    uart_config.stop_bits = UART_STOP_BITS_1;

    // 禁用硬件流控（RTS/CTS）
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    // 配置串口时钟源为APB总线时钟
    uart_config.source_clk = UART_SCLK_APB;

    // 设置硬件流控接收阈值
    uart_config.rx_flow_ctrl_thresh = 122;

    // 应用串口参数配置
    uart_param_config(UART_NUM_0, &uart_config);

    // 将 UART0 映射到指定引脚：TX->GPIO43，RX->GPIO44，不使用流控（RTS/CTS）
    uart_set_pin(UART_NUM_0, GPIO_NUM_43, GPIO_NUM_44, -1, -1);

    // 安装 UART 驱动：分配 2048 字节接收/发送环形缓冲区，事件队列大小 20
    // （参数 5 传 NULL 表示不注册事件回调，参数 6 为中断分配标志 0）
    uart_driver_install(UART_NUM_0, 2048 , 2048 , 20, NULL, 0);
}
