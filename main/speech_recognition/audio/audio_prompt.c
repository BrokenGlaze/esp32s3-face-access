/**
 * @file audio_prompt.c
 * @brief 固定语音片段播报实现
 *
 * 播放链路：PCM(Flash) -> 展开为 32bit 立体声 -> i2s_channel_write ->
 * ES8311 DAC -> 功放 -> 喇叭。
 */

#include "audio_prompt.h"

#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "i2s.h"
#include "nvs.h"
#include "pca9557.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h" // 诊断日志，便于确认 I2S/功放初始化与播报是否真正执行

#define PCM_CHUNK_SAMPLES 512 // 每批处理的单声道样本数（16bit）

static const char *TAG = "audio_prompt";

/* 内嵌提示音：16kHz / 单声道 / 16bit little-endian */
extern const uint8_t face_error_pcm_start[] asm("_binary_face_error_pcm_start");
extern const uint8_t face_error_pcm_end[] asm("_binary_face_error_pcm_end");
extern const uint8_t face_none_pcm_start[] asm("_binary_face_none_pcm_start");
extern const uint8_t face_none_pcm_end[] asm("_binary_face_none_pcm_end");
extern const uint8_t face_true_pcm_start[] asm("_binary_face_true_pcm_start");
extern const uint8_t face_true_pcm_end[] asm("_binary_face_true_pcm_end");
extern const uint8_t input_true_pcm_start[] asm("_binary_input_true_pcm_start");
extern const uint8_t input_true_pcm_end[] asm("_binary_input_true_pcm_end");
extern const uint8_t input_error_pcm_start[] asm("_binary_input_error_pcm_start");
extern const uint8_t input_error_pcm_end[] asm("_binary_input_error_pcm_end");

static QueueHandle_t s_prompt_queue = NULL;
static bool s_inited = false;

/* 运行时音量与开关：由管理员界面修改，默认值与编译期配置一致。
 * 都是单字节标量，任意任务写入都不需要额外同步 */
static volatile uint8_t s_volume = ES8311_VOICE_VOLUME;
static volatile bool s_enabled = true;

/* NVS 命名空间与键：修改后立即写入，实现掉电保存 */
#define AUDIO_NVS_NS      "audio_cfg"
#define AUDIO_NVS_VOLUME  "volume"
#define AUDIO_NVS_ENABLED "enabled"

/* 32bit 立体声发送缓冲：512 样本 × 2 声道 × 4 字节 = 4KB，放在内部 RAM 供 I2S DMA 使用 */
static uint32_t s_tx_buf[PCM_CHUNK_SAMPLES * 2];

/**
 * @brief 从 NVS 载入音量与开关（无记录或 NVS 不可用时保留默认值）
 */
static void audio_prompt_load_config(void)
{
    nvs_handle_t handle;
    uint8_t volume = s_volume;
    uint8_t enabled = 1;

    if (nvs_open(AUDIO_NVS_NS, NVS_READONLY, &handle) != ESP_OK)
    {
        return; /* 首次上电尚无记录 */
    }

    if (nvs_get_u8(handle, AUDIO_NVS_VOLUME, &volume) == ESP_OK)
    {
        if (volume < 1)
        {
            volume = 1;
        }
        else if (volume > 100)
        {
            volume = 100;
        }
        s_volume = volume;
    }

    if (nvs_get_u8(handle, AUDIO_NVS_ENABLED, &enabled) == ESP_OK)
    {
        s_enabled = (enabled != 0);
    }

    nvs_close(handle);
}

/**
 * @brief 把当前音量与开关写入 NVS
 */
static void audio_prompt_save_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(AUDIO_NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "[AUD] nvs open failed, ret=%d", (int)ret);
        return;
    }

    ret = nvs_set_u8(handle, AUDIO_NVS_VOLUME, s_volume);
    if (ret == ESP_OK)
    {
        ret = nvs_set_u8(handle, AUDIO_NVS_ENABLED, s_enabled ? 1 : 0);
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "[AUD] nvs save failed, ret=%d", (int)ret);
    }
}

/**
 * @brief 把 16bit 单声道 PCM 展开成 32bit 立体声并写入 I2S
 */
static void audio_play_pcm(const uint8_t *data, size_t len)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    uint8_t volume = s_volume;

    if (i2s_get_chan_handle(&tx, &rx) != ESP_OK || tx == NULL)
    {
        ESP_LOGE(TAG, "[AUD] i2s channel not ready, skip playback");
        return;
    }

    if (volume < 1)
    {
        volume = 1;
    }
    else if (volume > 100)
    {
        volume = 100;
    }

    ESP_LOGI(TAG, "[AUD] playback start, pcm bytes=%d, volume=%d", (int)len, (int)volume);

    const int16_t *src = (const int16_t *)data;
    size_t samples = len / sizeof(int16_t);

    while (samples > 0)
    {
        size_t n = samples > PCM_CHUNK_SAMPLES ? PCM_CHUNK_SAMPLES : samples;

        /* 播报过程中被关闭：尽快停止，避免关掉开关后还要把整段念完 */
        if (!s_enabled)
        {
            ESP_LOGI(TAG, "[AUD] playback aborted (disabled)");
            break;
        }

        for (size_t i = 0; i < n; i++)
        {
            /* 软件音量：按当前音量百分比线性缩放样本幅度，
             * 避免直接调低 DAC 寄存器导致声音被衰减成静音 */
            int32_t scaled = (int32_t)src[i] * volume / 100;

            /* ES8311 为 16bit 模式，样本放在 32bit 槽的高 16 位 */
            uint32_t v = ((uint32_t)(uint16_t)scaled) << 16;
            s_tx_buf[i * 2] = v;
            s_tx_buf[i * 2 + 1] = v;
        }

        size_t written = 0;
        i2s_channel_write(tx, s_tx_buf, n * 2 * sizeof(uint32_t), &written, portMAX_DELAY);

        src += n;
        samples -= n;
    }

    ESP_LOGI(TAG, "[AUD] playback done");
}

static void audio_prompt_task(void *arg)
{
    (void)arg;
    audio_prompt_id_t id;

    while (true)
    {
        if (xQueueReceive(s_prompt_queue, &id, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (id)
        {
        case AUDIO_PROMPT_FACE_ERROR:
            audio_play_pcm(face_error_pcm_start,
                           (size_t)(face_error_pcm_end - face_error_pcm_start));
            break;
        case AUDIO_PROMPT_NO_FACE:
            audio_play_pcm(face_none_pcm_start,
                           (size_t)(face_none_pcm_end - face_none_pcm_start));
            break;
        case AUDIO_PROMPT_FACE_TRUE:
            audio_play_pcm(face_true_pcm_start,
                           (size_t)(face_true_pcm_end - face_true_pcm_start));
            break;
        case AUDIO_PROMPT_INPUT_TRUE:
            audio_play_pcm(input_true_pcm_start,
                           (size_t)(input_true_pcm_end - input_true_pcm_start));
            break;
        case AUDIO_PROMPT_INPUT_ERROR:
            audio_play_pcm(input_error_pcm_start,
                           (size_t)(input_error_pcm_end - input_error_pcm_start));
            break;
        default:
            break;
        }
    }
}

esp_err_t audio_prompt_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    /* 先恢复上次保存的音量与开关，再初始化硬件 */
    audio_prompt_load_config();

    /* 1. I2S TX（同时会创建 RX，录放共用一条总线） */
    esp_err_t ret = i2s_driver_init();
    ESP_LOGI(TAG, "[AUD] i2s_driver_init ret=%d", ret);
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 2. ES8311 DAC 配置 */
    ret = es8311_codec_init();
    ESP_LOGI(TAG, "[AUD] es8311_codec_init ret=%d", ret);
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 3. 打开功放（PCA9557 IO1 = PA_EN） */
    ret = pca9557_set_output_state(1, 1);
    ESP_LOGI(TAG, "[AUD] PA enable ret=%d", ret);
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 4. 播放队列与任务
     * 绑定核心 0、优先级 6：
     * 核心 1 上摄像头采集与 LCD 预览都是优先级 5 且长期可运行，若把播报任务
     * 放在核心 1（原优先级 4），一旦预览流水线跑满就会被完全饿死，表现为
     * “识别成功/失败都没有声音”。放到核心 0 并高于 AI 任务（5），
     * 播放时靠 i2s_channel_write 的 DMA 阻塞自然让出 CPU，不影响识别。 */
    s_prompt_queue = xQueueCreate(2, sizeof(audio_prompt_id_t));
    if (s_prompt_queue == NULL)
    {
        ESP_LOGE(TAG, "[AUD] queue create failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(audio_prompt_task, "audio_prompt", 4 * 1024, NULL, 6, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "[AUD] task create failed (internal RAM)");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "[AUD] init done");
    return ESP_OK;
}

void audio_prompt_play(audio_prompt_id_t id)
{
    if (!s_enabled)
    {
        ESP_LOGI(TAG, "[AUD] play ignored (voice disabled), id=%d", (int)id);
        return;
    }

    if (!s_inited || s_prompt_queue == NULL)
    {
        ESP_LOGW(TAG, "[AUD] play ignored (not initialized), id=%d", (int)id);
        return;
    }

    /* 不阻塞调用者；队列满说明上一段还没播完，直接丢弃本次请求 */
    if (xQueueSend(s_prompt_queue, &id, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "[AUD] play dropped (queue full), id=%d", (int)id);
    }
}

void audio_prompt_set_volume(uint8_t percent)
{
    if (percent < 1)
    {
        percent = 1;
    }
    else if (percent > 100)
    {
        percent = 100;
    }

    if (s_volume == percent)
    {
        return; /* 值未变化就不写 Flash */
    }

    s_volume = percent;
    audio_prompt_save_config();
}

uint8_t audio_prompt_get_volume(void)
{
    return s_volume;
}

void audio_prompt_set_enabled(bool enabled)
{
    if (s_enabled == enabled)
    {
        return;
    }

    s_enabled = enabled;
    audio_prompt_save_config();
}

bool audio_prompt_is_enabled(void)
{
    return s_enabled;
}
