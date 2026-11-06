/**
 * @file es8311_codec.c
 * @brief ES8311 音频编解码芯片驱动（兼容新旧版 I2C 接口）
 *
 * 根据 i2c.h 的 I2C_VERSION 宏，在编译期选择两套实现：
 *   - I2C_VERSION_1（旧版驱动）：复用 espressif es8311 组件，
 *     初始化代码与 11-audio_es8311 参考工程完全一致；
 *   - I2C_VERSION_2（新版驱动）：espressif 组件仅支持旧版 I2C，
 *     因此改为直接寄存器操作完成初始化。
 *
 * 默认配置：16kHz 采样率、6.144MHz 主时钟、16bit、I2S 从机模式、音量 70%。
 * 使用：i2c_init() -> es8311_codec_init()。
 */

#include "es8311_codec.h"
#include "i2s.h"              /* 复用采样率/MCLK 等音频参数宏（需与 I2S 一致） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"

#define TAG "es8311"

#if (I2C_VERSION == I2C_VERSION_1)
#include "es8311.h"           /* espressif es8311 组件头文件（仅旧版 I2C 可用） */
#else
/* 新版 I2C 驱动：ES8311 设备句柄（es8311_codec_init() 中注册获取） */
static i2c_master_dev_handle_t s_dev = NULL;

/* 写寄存器：新版驱动通过设备句柄传输 */
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(1000));
}

/* 初始化寄存器序列 [地址, 值]，复位后各寄存器为默认值，可直接写完整值（16kHz/6.144MHz/16bit/I2S从机） */
static const uint8_t es8311_init_seq[][2] = {
    /* 时钟：使能 MCLK，并按 6.144MHz/16kHz 配置分频系数 */
    {0x01, 0x3F}, /* 使能全部时钟（MCLK 输入、不反相） */
    {0x02, 0x48}, /* 预分频 3、预倍频 1 */
    {0x03, 0x10}, /* 单倍速采样、ADC OSR=16 */
    {0x04, 0x10}, /* DAC OSR=16 */
    {0x05, 0x00}, /* ADC/DAC 时钟分频 1 */
    {0x06, 0x03}, /* BCLK 分频 4（bit5=0 不反相） */
    {0x07, 0x00}, /* LRCK 分频高字节 */
    {0x08, 0xFF}, /* LRCK 分频低字节 */

    /* 音频格式：I2S 从机模式（reg00 bit6=0 保持默认）、16bit */
    {0x09, 0x0C}, /* DAC 串行端口 16bit */
    {0x0A, 0x0C}, /* ADC 串行端口 16bit */

    /* 上电与系统配置 */
    {0x0D, 0x01}, /* 上电模拟电路 */
    {0x0E, 0x02}, /* 使能模拟 PGA 与 ADC 调制器 */
    {0x12, 0x00}, /* 上电 DAC */
    {0x13, 0x10}, /* 使能耳机驱动输出 */
    {0x1C, 0x6A}, /* ADC 均衡器旁路、数字域直流偏移消除 */
    {0x37, 0x08}, /* DAC 均衡器旁路 */

    /* 麦克风与音量 */
    {0x17, 0xC8}, /* ADC 增益 */
    {0x14, 0x1A}, /* 模拟麦克风（禁用数字麦克风）、最大 PGA 增益 */
    {0x32, ES8311_DAC_VOLUME_REG}, /* DAC 固定基准音量（软件增益见 ES8311_VOICE_VOLUME） */
};
#endif /* I2C_VERSION == I2C_VERSION_1 */

esp_err_t es8311_codec_init(void)
{
#if (I2C_VERSION == I2C_VERSION_1)
    /* ==================== 旧版 I2C：复用 espressif es8311 组件 ==================== */
    /* 创建 ES8311 编解码器句柄，指定 I2C 编号和设备地址 */
    es8311_handle_t es_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    /* 检查句柄是否创建成功，失败则返回错误 */
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");

    /* 配置 ES8311 时钟参数 */
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,          /* 主时钟不反相 */
        .sclk_inverted = false,          /* 位时钟不反相 */
        .mclk_from_mclk_pin = true,      /* 主时钟来自 MCLK 引脚 */
        .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,   /* 主时钟频率 6.144MHz */
        .sample_frequency = EXAMPLE_SAMPLE_RATE   /* 采样频率 16kHz */
    };

    /* 初始化编解码器，设置时钟配置和 16 位分辨率 */
    ESP_RETURN_ON_ERROR(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
                        TAG, "es8311 init failed");
    /* 配置 ES8311 采样频率 */
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE),
                        TAG, "set es8311 sample frequency failed");
    /* 设置 ES8311 音量 */
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, ES8311_VOICE_VOLUME, NULL),
                        TAG, "set es8311 volume failed");
    /* 配置 ES8311 麦克风（禁用） */
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false),
                        TAG, "set es8311 microphone failed");

    return ESP_OK;
#else
    /* ==================== 新版 I2C：直接寄存器操作 ==================== */
    /* 将 ES8311 注册到 I2C 总线，获取设备句柄 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_DEVICE_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus_handle(), &dev_cfg, &s_dev),
                        TAG, "add device failed");

    /* 复位序列：0x1F 复位 -> 延时 20ms -> 0x00 解除复位 -> 0x80 上电 */
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x00, 0x1F), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x00, 0x00), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x00, 0x80), TAG, "reset failed");

    /* 依次写入初始化寄存器序列 */
    for (size_t i = 0; i < sizeof(es8311_init_seq) / sizeof(es8311_init_seq[0]); i++) {
        ESP_RETURN_ON_ERROR(es8311_write_reg(es8311_init_seq[i][0], es8311_init_seq[i][1]),
                            TAG, "write reg failed");
    }

    return ESP_OK;
#endif /* I2C_VERSION == I2C_VERSION_1 */
}
