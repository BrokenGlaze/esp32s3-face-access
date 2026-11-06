#ifndef __I2S_H__
#define __I2S_H__

/* ============================ 文件功能概述 ============================ */
/* 本头文件是 I2S 音频外设驱动模块的接口定义，包含：                        */
/*   1. I2S 外设引脚号宏（MCLK/BCLK/WS/DOUT）；                            */
/*   2. 音频示例配置宏（采样率、MCLK 倍频、MCLK 频率）；                   */
/*   3. I2S 初始化与录音接口函数声明。                                     */
/* ======================================================================== */

#include "driver/gpio.h"      /* GPIO 驱动头文件，提供 GPIO_NUM_x 引脚宏定义 */
#include "driver/i2s_std.h"   /* I2S 标准模式驱动头文件，提供 i2s_chan_handle_t 等类型 */
#include "esp_err.h"          /* ESP 错误码类型定义 */

/* ====================== I2S 引脚宏定义 ================================= */
#define I2S_NUM         (0)            /* I2S 外设编号：使用 I2S0 */
#define I2S_MCK_IO      (GPIO_NUM_38)  /* 主时钟 MCLK 引脚：GPIO38，用于同步数据传输 */
#define I2S_BCK_IO      (GPIO_NUM_14)  /* 位时钟 BCLK 引脚：GPIO14，用于同步数据位传输 */
#define I2S_WS_IO       (GPIO_NUM_13)  /* 字选择 WS 引脚：GPIO13，用于选择左右声道 */
#define I2S_DO_IO       (GPIO_NUM_45)  /* 数据输出 DO 引脚：GPIO45，向 ES8311 发送音频数据 */
#define I2S_DI_IO       (GPIO_NUM_12)  /* 数据输入 DI 引脚：GPIO12，接收麦克风数据（当前未使用） */

/* ====================== 音频示例配置宏 ================================= */
#define EXAMPLE_SAMPLE_RATE     (16000)                       /* 采样率：16kHz（需与 ES8311 一致） */
/* I2S 时钟使用 384 倍频，MCLK = 16kHz * 384 = 6.144MHz，与 ES8311
 * 初始化序列中的分频配置匹配（256 倍频会产生噪声） */
#define EXAMPLE_MCLK_MULTIPLE   (384)                         /* MCLK 倍频系数：384 倍 */
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE) /* MCLK 频率：6.144MHz */

/* ====================== 函数声明 ======================================= */
/**
 * @brief 初始化 I2S 音频外设
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则返回对应错误码
 *
 * 功能：创建 I2S 标准模式发送/接收双通道（主模式、32bit、立体声），
 *       配置 MCLK/BCLK/WS/DOUT/DIN 引脚并使能收发通道：
 *       - 发送通道用于向 ES8311 写入音频数据（播放）；
 *       - 接收通道保留，可用于后续接入麦克风采集。
 */
extern esp_err_t i2s_driver_init(void);

/**
 * @brief 从 I2S 接收通道读取音频数据（麦克风录音数据）
 * @param audio_buffer 存放读取数据的缓冲区指针
 * @param len          要读取的数据长度（字节数）
 * @param bytes_read   实际读取到的字节数（输出参数，可为 NULL）
 * @param timeout_ms   等待超时时间（毫秒）
 * @return esp_err_t 返回 ESP_OK 表示读取成功，否则返回对应错误码
 *
 * 功能：从 I2S 接收通道（DIN 引脚）读取音频数据（当前工程未使用麦克风采集）。
 */
extern esp_err_t i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief 获取 I2S 收发通道句柄（供 esp_codec_dev 等组件复用）
 * @param tx 输出参数：发送通道句柄
 * @param rx 输出参数：接收通道句柄
 * @return esp_err_t 返回 ESP_OK 表示获取成功，否则返回对应错误码
 *
 * 功能：返回 i2s_driver_init() 创建的 I2S0 收发通道句柄，供 ES8311
 *       编解码设备的数据接口使用（audio_codec_new_i2s_data）。
 */
extern esp_err_t i2s_get_chan_handle(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);

#endif
