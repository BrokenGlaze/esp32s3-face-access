/**
 * @file i2s.c
 * @brief I2S 音频外设驱动源文件
 *
 * 功能说明：
 *   1. 本文件实现了 I2S 标准模式发送/接收双通道的初始化（主模式、32bit、
 *      立体声），配置 MCLK/BCLK/WS/DOUT/DIN 引脚，并与 ES8311 编解码
 *      芯片的采样率对齐（16kHz）；
 *   2. 32bit 槽位保留，接收通道可用于后续接入麦克风采集；
 *   3. 实现麦克风录音接口 i2s_read()（从 I2S 接收通道读取数据），以及
 *      i2s_get_chan_handle()（向 esp_codec_dev 暴露收发通道句柄）。
 *
 * 使用说明：
 *   - 初始化顺序：先调用 i2s_driver_init()，再调用 es8311_codec_init()
 *     （依赖 I2C，需确保 i2c_init() 已先执行）。
 */

#include "i2s.h"                 /* 自定义 I2S 头文件，提供引脚宏、采样率宏与函数声明 */
#include "freertos/FreeRTOS.h"   /* FreeRTOS 内核头文件，提供 pdMS_TO_TICKS 宏 */
#include "esp_check.h"           /* ESP 检查头文件，提供 ESP_ERROR_CHECK 宏 */

/* I2S 发送通道句柄：全局静态变量，初始化后供播放使用 */
static i2s_chan_handle_t tx_handle = NULL;

/* I2S 接收通道句柄：全局静态变量，初始化后供录音接口使用 */
static i2s_chan_handle_t rx_handle = NULL;

/**
 * @brief 初始化 I2S 音频外设（标准模式发送/接收双通道）
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则触发断言终止程序
 *
 * 功能：
 *   1. 调用 i2s_new_channel 同时创建 I2S 发送/接收两个通道（全双工），
 *      并设置 auto_clear 自动清除 DMA 缓冲区中的旧数据；
 *   2. 配置标准模式（Philips I2S 格式）：采样率 16kHz、32bit 数据宽度、
 *      立体声双声道（32bit 槽位下每个 L/R 帧内承载 ES7210 的 4 路 16bit
 *      通道数据），映射 MCLK/BCLK/WS/DOUT/DIN 引脚；
 *   3. 使用默认 MCLK 倍频（与参考工程一致，MCLK=16kHz*256=4.096MHz，
 *      ES7210 从机模式可直接由此分频出 16kHz 采样时钟）；
 *   4. 调用 i2s_channel_init_std_mode 应用配置并使能发送、接收两个通道。
 *
 * 注意：I2S 默认采样率为 48kHz，必须将采样率调整为 16kHz 与 ES7210/ES8311
 *       保持一致才能正常同步。
 */
esp_err_t i2s_driver_init(void)
{
    /* 配置 I2S 收发通道：指定 I2S 外设编号与主模式角色 */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    /* 使能自动清除功能：自动清除 DMA 缓冲区中的旧数据 */
    chan_cfg.auto_clear = true;

    /* 创建新的 I2S 通道：同时创建发送(tx)与接收(rx)通道，实现全双工 */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    /* 配置 I2S 标准模式参数（收发通道共用同一套时钟与时隙配置） */
    i2s_std_config_t std_cfg = {
        /* 时钟配置：采样率 16kHz，MCLK 固定为 384 倍频（6.144MHz），
         * 与 ES8311 初始化序列中按 6.144MHz 配置的分频系数保持一致 */
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
        /* 时隙配置：32bit 数据宽度、立体声模式（左右声道）
         * 32bit 槽位用于承载 ES7210 的 4 路 16bit 通道数据 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        /* GPIO 引脚配置 */
        .gpio_cfg =
        {
            /* 主时钟 MCLK：用于同步数据传输，提供高精度时钟源 */
            .mclk = I2S_MCK_IO,
            /* 位时钟 BCLK：用于同步每个数据位的传输 */
            .bclk = I2S_BCK_IO,
            /* 字选择 WS：用于选择左右声道（字选择时钟） */
            .ws = I2S_WS_IO,
            /* 数据输出 DOUT：向 ES8311 发送音频数据（播放） */
            .dout = I2S_DO_IO,
            /* 数据输入 DIN：接收 ES8311 ADC 输出的麦克风数据（录音/语音识别） */
            .din = I2S_DI_IO,
            /* 引脚反相标志：全部不反相，保持默认逻辑电平 */
            .invert_flags =
            {
                .mclk_inv = false, /* 主时钟不反相 */
                .bclk_inv = false, /* 位时钟不反相 */
                .ws_inv = false,   /* 字选择不反相 */
            },
        },
    };

    /* ES8311 的时钟寄存器按 6.144MHz MCLK 计算，默认的 256 倍频
     * （4.096MHz）会导致 MCLK 与芯片内部分频不匹配，产生明显噪声 */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    /* 初始化 I2S 发送通道为标准模式并应用上述全部配置 */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    /* 初始化 I2S 接收通道为标准模式（与发送通道同采样率/位宽） */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    /* 使能 I2S 发送通道（向 ES8311 播放提示音） */
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    /* 使能 I2S 接收通道（保留麦克风采集能力） */
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    return ESP_OK;
}

/**
 * @brief 从 I2S 接收通道读取音频数据（麦克风录音数据）
 * @param audio_buffer 存放读取数据的缓冲区指针
 * @param len          要读取的数据长度（字节数）
 * @param bytes_read   实际读取到的字节数（输出参数，可为 NULL）
 * @param timeout_ms   等待超时时间（毫秒）
 * @return esp_err_t 返回 ESP_OK 表示读取成功，否则返回对应错误码
 *
 * 功能：从 I2S 接收通道（DIN 引脚）读取音频数据（当前工程未使用麦克风采集）。
 *       注意：调用前必须先调用 i2s_driver_init() 完成接收通道初始化。
 */
esp_err_t i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    /* 通过 I2S 接收通道读取数据，超时时间由调用方指定 */
    return i2s_channel_read(rx_handle, audio_buffer, len, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

/**
 * @brief 获取 I2S 收发通道句柄
 * @param tx 输出参数：发送通道句柄
 * @param rx 输出参数：接收通道句柄
 * @return esp_err_t 返回 ESP_OK 表示获取成功，否则返回对应错误码
 *
 * 功能：返回 i2s_driver_init() 创建的 I2S0 收发通道句柄，供 ES7210
 *       编解码设备的数据接口（esp_codec_dev）复用。
 */
esp_err_t i2s_get_chan_handle(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    /* 校验输出参数指针 */
    if (!tx || !rx)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 返回收发通道句柄 */
    *tx = tx_handle;
    *rx = rx_handle;

    /* 句柄有效则返回成功，否则返回失败 */
    return (*tx && *rx) ? ESP_OK : ESP_FAIL;
}
