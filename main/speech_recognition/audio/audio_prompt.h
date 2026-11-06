#pragma once

/**
 * @file audio_prompt.h
 * @brief 固定语音片段播报模块
 *
 * 用于在人脸识别失败、未检测到人脸等场景播放预先录制的 PCM 提示音。
 * 音频文件以 16kHz、单声道、16bit 小端 PCM 格式嵌入固件（见 CMakeLists
 * 的 EMBED_FILES），播放时在内部 RAM 中展开为 32bit 立体声帧后写入 I2S TX。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 提示音类型 */
typedef enum
{
    AUDIO_PROMPT_FACE_ERROR = 0, // 人脸识别失败提示音
    AUDIO_PROMPT_NO_FACE,        // 未检测到人脸提示音
    AUDIO_PROMPT_FACE_TRUE,      // 人脸识别成功提示音
    AUDIO_PROMPT_INPUT_TRUE,     // 人脸录入成功提示音
    AUDIO_PROMPT_INPUT_ERROR,    // 人脸录入失败提示音
} audio_prompt_id_t;

/**
 * @brief 初始化播报模块：I2S TX、ES8311、功放使能，并创建播放任务
 * @return ESP_OK 成功，其他值失败
 * @note 需要先调用 i2c_init() 与 pca9557_init()
 */
esp_err_t audio_prompt_init(void);

/**
 * @brief 异步播放指定提示音（不阻塞调用者）
 * @param id 提示音类型
 * @note 队列已满或模块未初始化时直接忽略，避免重复提示堆积
 */
void audio_prompt_play(audio_prompt_id_t id);

/**
 * @brief 设置播报音量（软件增益百分比），并立即写入 NVS 掉电保存
 * @param percent 音量百分比，取值 1~100，超出范围会被钳位
 * @note 与 ES8311_VOICE_VOLUME 同为 PCM 样本的线性缩放系数，
 *       100 表示原始满幅；只影响语音播报，不影响其他音频路径
 */
void audio_prompt_set_volume(uint8_t percent);

/**
 * @brief 读取当前播报音量
 * @return 音量百分比（1~100）
 */
uint8_t audio_prompt_get_volume(void);

/**
 * @brief 开启/关闭语音播报，并立即写入 NVS 掉电保存
 * @param enabled false 表示关闭，此后所有提示音请求直接丢弃
 */
void audio_prompt_set_enabled(bool enabled);

/**
 * @brief 查询语音播报是否开启
 * @return true 表示开启
 */
bool audio_prompt_is_enabled(void);

#ifdef __cplusplus
}
#endif
